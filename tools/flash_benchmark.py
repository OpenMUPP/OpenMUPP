#!/usr/bin/env python3
"""Flash speed benchmark of the Versaloon dongle with OpenOCD.

The script writes a test image into the flash of an STM32F1 target, verifies
it and reads it back, in JTAG mode and in SWD mode, at each adapter speed.
Each run is a new OpenOCD process. The script reads the times from the
OpenOCD output ("wrote ... in", "verified ... in", "dumped ... in"), and also
measures the time of the full OpenOCD process.

Before the measurement, stm32f1_clock.tcl sets the target clock to 64 MHz
from the HSI. At the 8 MHz reset clock, the target limits JTAG to about
1.3 MHz (TCK <= F_CPU / 6).

The test image is pseudo-random data with a fixed seed. Thus the old and the
new firmware write the same bytes. The script erases the flash of the target.

Usage:
    # Flash the old firmware into the dongle, then:
    python3 flash_benchmark.py run --label old --output old.json
    # Flash the new firmware into the dongle, then:
    python3 flash_benchmark.py run --label new --output new.json
    # Make the Markdown table for the MR:
    python3 flash_benchmark.py compare old.json new.json

    python3 flash_benchmark.py run --transports swd --swd-speeds 2000 --repeat 5

The default image is 32 KiB, so that it fits into all STM32F103 devices from
the x6 (32 KiB) up. Use --size 65536 for an STM32F103x8 or larger.

The unit tests in test_flash_benchmark.py need no OpenOCD and no hardware.
"""

import argparse
import json
import os
import random
import re
import statistics
import subprocess
import sys
import tempfile
import time

DEFAULT_OPENOCD = "openocd"
DEFAULT_INTERFACE = "interface/vsllink.cfg"
DEFAULT_TARGET = "target/stm32f1x.cfg"
CLOCK_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stm32f1_clock.tcl")
# The target runs at 8 MHz after reset, and JTAG needs TCK <= F_CPU / 6.
# Thus connect at this speed, set the target clock, then set the test speed.
SETUP_SPEED_KHZ = 1000
FLASH_BASE = 0x08000000
DEFAULT_SIZE = 32 * 1024
SEED = 0x5A17
# JTAG: 1000 kHz bit-bang, 4500 kHz and 9000 kHz SPI fast path.
DEFAULT_JTAG_SPEEDS = [1000, 4500, 9000]
# SWD: the vsllink driver has only fixed steps. It gives delay 1 (235 kHz)
# for 235 to 1849 kHz, and delay 0 (1.85 MHz) for 1850 kHz and more.
DEFAULT_SWD_SPEEDS = [250, 2000]
DEFAULT_REPEAT = 3
DEFAULT_TIMEOUT_S = 300
# compare lists a step as slower only above this ratio. Two runs of the same
# firmware differ by a few percent.
SLOWER_RATIO = 1.05

STEPS = ("write", "verify", "read")
STEP_PATTERNS = {
    "write": re.compile(r"wrote (\d+) bytes from file .* in (\d+(?:\.\d+)?)s"),
    "verify": re.compile(r"verified (\d+) bytes (?:from file .* )?in (\d+(?:\.\d+)?)s"),
    "read": re.compile(r"dumped (\d+) bytes in (\d+(?:\.\d+)?)s"),
}
# The vsllink driver logs the GET_INFO text of the firmware, and then the
# USB_TO_XXX abilities. Without the abilities line, take a Versaloon line.
FIRMWARE_PATTERN = re.compile(r"Info : (.*)\n.*USB_TO_XXX abilities")
FIRMWARE_FALLBACK_PATTERN = re.compile(r"Info : (.*versaloon.*)", re.IGNORECASE)
CLOCK_PATTERN = re.compile(r"target clock: (.*)")


def make_image(size, seed=SEED):
    """Returns `size` pseudo-random bytes. The same seed gives the same bytes."""
    return random.Random(seed).randbytes(size)


def tcl_path(path):
    """Returns a path for a Tcl word in braces. Tcl needs forward slashes."""
    return "{" + path.replace("\\", "/") + "}"


def openocd_command(args, transport, speed_khz, image_path, dump_path):
    """Returns the argument list of one OpenOCD run."""
    image = tcl_path(image_path)
    cmd = [args.openocd]
    for script_dir in args.search:
        cmd += ["-s", script_dir]
    cmd += ["-f", args.interface, "-c", f"transport select {transport}", "-f", args.target]
    if args.clock_setup:
        cmd += ["-f", CLOCK_SCRIPT]
    for command in args.extra:
        cmd += ["-c", command]
    # The target script sets its own adapter speed, so set the speeds after it.
    commands = ["init", "reset halt"]
    if args.clock_setup:
        commands = [f"adapter speed {SETUP_SPEED_KHZ}"] + commands + ["stm32f1_clock_max"]
    commands.append(f"adapter speed {speed_khz}")
    if transport == "jtag":
        # The old firmware sets up the JTAG pins again on each speed change,
        # and the TAP goes to Test-Logic-Reset. Sync the TAP state of OpenOCD.
        commands.append("jtag arp_init")
    for command in commands + [
        f"flash write_image erase {image} 0x{FLASH_BASE:08x} bin",
        f"verify_image {image} 0x{FLASH_BASE:08x} bin",
        f"dump_image {tcl_path(dump_path)} 0x{FLASH_BASE:08x} {args.size}",
        "reset run",
        "shutdown",
    ]:
        cmd += ["-c", command]
    return cmd


def parse_output(text):
    """Returns the step times, sizes and firmware text of one OpenOCD log.

    The result has `<step>_s` and `<step>_bytes` for each step that the log
    shows, `firmware` when the log shows the GET_INFO text, and `target_clock`
    when the log shows the result of stm32f1_clock_max.
    """
    result = {}
    for step, pattern in STEP_PATTERNS.items():
        match = pattern.search(text)
        if match:
            result[f"{step}_bytes"] = int(match.group(1))
            result[f"{step}_s"] = float(match.group(2))
    match = FIRMWARE_PATTERN.search(text) or FIRMWARE_FALLBACK_PATTERN.search(text)
    if match:
        result["firmware"] = match.group(1).strip()
    match = CLOCK_PATTERN.search(text)
    if match:
        result["target_clock"] = match.group(1).strip()
    return result


def run_once(args, transport, speed_khz, image, image_path, dump_path, runner=subprocess.run):
    """Runs OpenOCD one time. Returns a result dictionary.

    `ok` is false when OpenOCD fails, when a step is missing from the log, or
    when the read-back data is not equal to the image.
    """
    if os.path.exists(dump_path):
        os.remove(dump_path)
    cmd = openocd_command(args, transport, speed_khz, image_path, dump_path)
    start = time.monotonic()
    try:
        proc = runner(cmd, capture_output=True, text=True, timeout=args.timeout)
        returncode, output = proc.returncode, (proc.stdout or "") + (proc.stderr or "")
    except subprocess.TimeoutExpired as exc:
        returncode, output = None, _text(exc.stdout) + _text(exc.stderr)
    total_s = time.monotonic() - start

    result = {"transport": transport, "speed_khz": speed_khz, "total_s": round(total_s, 3)}
    result.update(parse_output(output))
    errors = []
    if returncode is None:
        errors.append(f"timeout after {args.timeout} s")
    elif returncode != 0:
        errors.append(f"OpenOCD exit code {returncode}")
    missing = [step for step in STEPS if f"{step}_s" not in result]
    if args.clock_setup and "target_clock" not in result:
        missing.insert(0, "target clock")
    if missing:
        errors.append("missing in log: " + ", ".join(missing))
    if not errors:
        try:
            with open(dump_path, "rb") as f:
                if f.read() != image:
                    errors.append("read-back data is not equal to the image")
        except OSError as exc:
            errors.append(f"cannot read the dump: {exc}")
    result["ok"] = not errors
    if errors:
        result["error"] = "; ".join(errors)
        result["log_tail"] = output.strip().splitlines()[-15:]
    return result


def _text(data):
    if data is None:
        return ""
    return data.decode(errors="replace") if isinstance(data, bytes) else data


def kib_per_s(size_bytes, seconds):
    return size_bytes / 1024 / seconds if seconds > 0 else float("inf")


def summarize(runs):
    """Groups the passed runs by transport and speed. Returns one row per group
    with the median time of each step and of the full process."""
    groups = {}
    for run in runs:
        key = (run["transport"], run["speed_khz"])
        row = groups.setdefault(key, {"transport": key[0], "speed_khz": key[1], "runs": 0, "failed": 0, "_ok": []})
        row["runs"] += 1
        if run["ok"]:
            row["_ok"].append(run)
        else:
            row["failed"] += 1
    rows = []
    for key in sorted(groups):
        row = groups[key]
        ok = row.pop("_ok")
        for name in [f"{step}_s" for step in STEPS] + ["total_s"]:
            row[name] = statistics.median(run[name] for run in ok) if ok else None
        rows.append(row)
    return rows


def fmt_time(value):
    return "-" if value is None else f"{value:.2f}"


def fmt_rate(size, seconds):
    return "-" if seconds is None else f"{kib_per_s(size, seconds):.1f}"


def summary_table(result):
    """Returns a Markdown table of one result file."""
    size = result["size"]
    lines = [
        f"Firmware: {result.get('firmware') or 'unknown'} (label `{result['label']}`), image {size} bytes, "
        f"target clock {result.get('target_clock') or 'unknown'}",
        "",
        "| Transport | kHz | Write s | Write KiB/s | Verify s | Read s | Read KiB/s | OpenOCD s | Failed |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in summarize(result["runs"]):
        lines.append(
            f"| {row['transport'].upper()} | {row['speed_khz']} "
            f"| {fmt_time(row['write_s'])} | {fmt_rate(size, row['write_s'])} "
            f"| {fmt_time(row['verify_s'])} "
            f"| {fmt_time(row['read_s'])} | {fmt_rate(size, row['read_s'])} "
            f"| {fmt_time(row['total_s'])} | {row['failed']}/{row['runs']} |"
        )
    return "\n".join(lines)


def compare_table(old, new):
    """Returns a Markdown table that compares two result files.

    The ratio is the time of the new firmware divided by the time of the old
    firmware. A step with a ratio above SLOWER_RATIO is in the slower list.
    """
    if old["size"] != new["size"]:
        raise ValueError(f"the image sizes are not equal: {old['size']} and {new['size']} bytes")
    if old.get("target_clock") != new.get("target_clock"):
        print(f"WARNING: the target clocks are not equal: {old.get('target_clock')} and {new.get('target_clock')}",
              file=sys.stderr)
    old_rows = {(r["transport"], r["speed_khz"]): r for r in summarize(old["runs"])}
    new_rows = {(r["transport"], r["speed_khz"]): r for r in summarize(new["runs"])}
    lines = [
        f"- `{old['label']}`: {old.get('firmware') or 'unknown'}",
        f"- `{new['label']}`: {new.get('firmware') or 'unknown'}",
        f"- Target clock: {new.get('target_clock') or 'unknown'}",
        f"- Image: {old['size']} bytes. Times are the median of each group, in seconds.",
        f"- Ratio: new time / old time. A ratio above {SLOWER_RATIO:.2f} counts as slower.",
        "",
        f"| Transport | kHz | Step | {old['label']} s | {new['label']} s | Ratio |",
        "|---|---:|---|---:|---:|---:|",
    ]
    slower = []
    for key in sorted(set(old_rows) | set(new_rows)):
        for step in list(STEPS) + ["total"]:
            a = old_rows.get(key, {}).get(f"{step}_s")
            b = new_rows.get(key, {}).get(f"{step}_s")
            ratio = f"{b / a:.2f}" if a and b is not None else "-"
            if a and b is not None and b > a * SLOWER_RATIO:
                slower.append(f"{key[0].upper()} {key[1]} kHz {step}")
            lines.append(f"| {key[0].upper()} | {key[1]} | {step} | {fmt_time(a)} | {fmt_time(b)} | {ratio} |")
    return "\n".join(lines), slower


def parse_speeds(text):
    try:
        speeds = [int(item) for item in text.split(",") if item.strip()]
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a list of kHz values: {text!r}")
    if not speeds or any(speed <= 0 for speed in speeds):
        raise argparse.ArgumentTypeError(f"speeds must be positive kHz values: {text!r}")
    return speeds


def cmd_run(args):
    if args.size <= 0 or args.size % 4:
        print("ERROR: --size must be a positive multiple of 4", file=sys.stderr)
        return 2
    image = make_image(args.size)
    plan = []
    for transport in args.transports:
        speeds = args.jtag_speeds if transport == "jtag" else args.swd_speeds
        plan += [(transport, speed) for speed in speeds]

    runs = []
    with tempfile.TemporaryDirectory(prefix="vsl-flash-bench-") as work_dir:
        image_path = os.path.join(work_dir, "image.bin")
        dump_path = os.path.join(work_dir, "dump.bin")
        with open(image_path, "wb") as f:
            f.write(image)
        for transport, speed in plan:
            for index in range(args.repeat):
                run = run_once(args, transport, speed, image, image_path, dump_path)
                runs.append(run)
                status = "PASS" if run["ok"] else "FAIL"
                print(
                    f"{status} {transport.upper()} {speed} kHz run {index + 1}/{args.repeat}: "
                    f"write {fmt_time(run.get('write_s'))} s, verify {fmt_time(run.get('verify_s'))} s, "
                    f"read {fmt_time(run.get('read_s'))} s, OpenOCD {run['total_s']:.2f} s",
                    flush=True,
                )
                if not run["ok"]:
                    print(f"  {run['error']}", file=sys.stderr)
                    for line in run["log_tail"]:
                        print(f"  | {line}", file=sys.stderr)

    firmware = next((run["firmware"] for run in runs if run.get("firmware")), None)
    clock = next((run["target_clock"] for run in runs if run.get("target_clock")), None)
    if not args.clock_setup:
        clock = "8 MHz (reset)"
    result = {"label": args.label, "firmware": firmware, "target_clock": clock, "size": args.size, "runs": runs}
    if args.output:
        with open(args.output, "w") as f:
            json.dump(result, f, indent=2)
            f.write("\n")
    print()
    print(summary_table(result))
    return 0 if all(run["ok"] for run in runs) else 1


def cmd_compare(args):
    results = []
    for path in (args.old, args.new):
        with open(path) as f:
            results.append(json.load(f))
    try:
        table, slower = compare_table(*results)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    print(table)
    failed = sum(not run["ok"] for result in results for run in result["runs"])
    if failed:
        print(f"\nFAILED: {failed} runs failed. The table uses only the passed runs.", file=sys.stderr)
    if slower:
        print("\nSLOWER: " + ", ".join(slower), file=sys.stderr)
    return 1 if failed or slower else 0


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    run = sub.add_parser("run", help="measure the flash speed with the dongle that is connected now")
    run.add_argument("--label", required=True, help="name of this firmware in the tables, for example old or new")
    run.add_argument("--output", help="write the results to this JSON file, for the compare command")
    run.add_argument("--transports", type=lambda t: t.split(","), default=["jtag", "swd"],
                     help="comma-separated list of jtag and swd (default: jtag,swd)")
    run.add_argument("--jtag-speeds", type=parse_speeds, default=DEFAULT_JTAG_SPEEDS,
                     help="comma-separated JTAG speeds in kHz (default: %(default)s)")
    run.add_argument("--swd-speeds", type=parse_speeds, default=DEFAULT_SWD_SPEEDS,
                     help="comma-separated SWD speeds in kHz (default: %(default)s)")
    run.add_argument("--repeat", type=int, default=DEFAULT_REPEAT, help="runs for each speed (default: %(default)s)")
    run.add_argument("--size", type=int, default=DEFAULT_SIZE,
                     help="image size in bytes, a multiple of 4 (default: %(default)s)")
    run.add_argument("--openocd", default=DEFAULT_OPENOCD, help="OpenOCD executable (default: %(default)s)")
    run.add_argument("--interface", default=DEFAULT_INTERFACE, help="interface script (default: %(default)s)")
    run.add_argument("--target", default=DEFAULT_TARGET, help="target script (default: %(default)s)")
    run.add_argument("-s", "--search", action="append", default=[], help="OpenOCD script search directory")
    run.add_argument("-c", "--extra", action="append", default=[],
                     help="extra OpenOCD command after the target script, for example 'reset_config srst_only'")
    run.add_argument("--no-clock-setup", dest="clock_setup", action="store_false",
                     help="keep the target at its 8 MHz reset clock (default: set it to 64 MHz from the HSI)")
    run.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT_S,
                     help="timeout of one OpenOCD run in seconds (default: %(default)s)")
    run.set_defaults(func=cmd_run)

    compare = sub.add_parser("compare", help="compare two result files as a Markdown table")
    compare.add_argument("old", help="JSON file of the old firmware")
    compare.add_argument("new", help="JSON file of the new firmware")
    compare.set_defaults(func=cmd_compare)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    if args.command == "run":
        bad = [t for t in args.transports if t not in ("jtag", "swd")]
        if bad:
            print(f"ERROR: unknown transport: {', '.join(bad)}", file=sys.stderr)
            return 2
        if args.repeat < 1:
            print("ERROR: --repeat must be 1 or more", file=sys.stderr)
            return 2
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
