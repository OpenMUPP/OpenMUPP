#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hardware test of a multi image with an STM32F1 target and a UART loopback.

The dongle must have a multi image with the Versaloon and the CMSIS-DAP
personalities. Connect it to an STM32F1 target on SWD and JTAG (SWDIO/TMS,
SWCLK/TCK, TDI, TDO, nTRST, nRST and GND), and connect the UART TX pin to the
UART RX pin (PA9 to PA10 on the Mini and the WeAct H523). Each
tool runs SWD before JTAG: on the WeAct H523, all JTAG steps failed without
nTRST and nRST, and passed with them.

The script runs these steps in this order:

  1. Switch to the Versaloon personality (mupp_personality.py).
  2. Serial loopback on the CDC-ACM port (serial_loopback_test.py).
  3. OpenOCD with the vsllink driver, SWD and JTAG, at each speed.
  4. Switch to the CMSIS-DAP personality.
  5. Serial loopback on the CDC-ACM port, and the DAP_UART_* loopback
     (dap_uart_loopback_test.py).
  6. OpenOCD with the cmsis-dap driver, SWD and JTAG, at each speed.
  7. pyOCD, SWD and JTAG, at each speed (pyocd_jtag_test.py).
  8. probe-rs, SWD and JTAG, at each speed.

Each OpenOCD run sets the target clock to 64 MHz (stm32f1_clock.tcl), then
writes a 32 KiB image into the flash, verifies it and reads it back. It then
does reset and halt, two single steps, a hardware breakpoint, RAM writes and
reads, and run and halt. The image has a vector table and a loop at
0x08000040, as the image of pyocd_jtag_test.py.

probe-rs writes the image, verifies it, reads it back, writes and reads RAM
and resets the target. probe-rs cannot set the target clock before each
command, and the target runs at 8 MHz after a reset. TCK must not be above
F_CPU / 6, so the script skips probe-rs JTAG above 1333 kHz.

The first OpenOCD run reads the device ID and the flash size of the target.
The script uses them to select the pyOCD target and the probe-rs chip, as
long as --pyocd-target and --probe-rs-chip are not given. A low or medium
density chip needs the Keil.STM32F1xx_DFP pack in pyOCD
(`pyocd pack install stm32f103c8`).

With --serial, OpenOCD, pyOCD and probe-rs use only the dongle with this
serial number.

The full output of each step goes into a log file in --log-dir. The script
ends with a table of the results, and exits with code 1 when a step fails.
The script erases the flash of the target. It leaves the dongle in the
CMSIS-DAP personality.

Usage:
    python3 probe_hw_test.py
    python3 probe_hw_test.py --skip probe-rs --jtag-speeds 1000
    python3 probe_hw_test.py --full-uart --log-dir /tmp/hw-test

Needs pyusb and pyserial, OpenOCD, pyOCD and probe-rs. Use --skip for a
tool that is not installed. The unit tests in test_probe_hw_test.py need no
hardware.
"""

import argparse
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
CLOCK_SCRIPT = os.path.join(TOOLS_DIR, "stm32f1_clock.tcl")

FLASH_BASE = 0x08000000
LOOP_PC = FLASH_BASE + 0x40
STACK_TOP = 0x20005000
DEFAULT_SIZE = 32 * 1024
SEED = 0x5A17
# The target runs at 8 MHz after reset, and JTAG needs TCK <= F_CPU / 6.
SETUP_SPEED_KHZ = 1000
# probe-rs cannot set the target clock: TCK <= 8 MHz / 6
PROBE_RS_JTAG_MAX_KHZ = 1333
# 4000 kHz: the untimed SWD engine of CMSIS-DAP on the STM32F1 (swd::maxClockHz)
DEFAULT_SWD_SPEEDS = [1000, 2000, 4000]
DEFAULT_JTAG_SPEEDS = [1000, 9000]
STEP_TIMEOUT_S = 300

USB_IDS = {"versaloon": (0x0483, 0xA038), "cmsis-dap": (0x1209, 0x0001)}
OPENOCD_INTERFACES = {
    "versaloon": ["-f", "interface/vsllink.cfg"],
    "cmsis-dap": ["-f", "interface/cmsis-dap.cfg", "-c", "cmsis-dap vid_pid 0x1209 0x0001"],
}
CPU_TAP_IDCODE = "0x3ba00477"
DEFAULT_PYOCD_TARGET = "stm32f103rc"
DEFAULT_PROBE_RS_CHIP = "STM32F103RC"

# Short UART runs; --full-uart runs the default cases of the scripts.
QUICK_BAUDS = ["115200", "921600", "2000000"]
QUICK_SERIAL_ARGS = ["--bauds", *QUICK_BAUDS, "--formats", "8N", "8E", "--stop-bits", "1", "2"]
QUICK_DAP_UART_ARGS = ["--bauds", *QUICK_BAUDS, "--formats", "8N1", "8E1", "8N2"]

PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"

# OpenOCD checks. Each check prints "CHECK PASS|FAIL <name>: <detail>".
OPENOCD_CHECKS = r"""
proc mupp_check {name ok {detail ""}} {
	if {$ok} { set r PASS } else { set r FAIL }
	echo "CHECK $r $name: [string map {"\n" " "} $detail]"
}

proc mupp_try {name script} {
	set failed [catch {uplevel 1 $script} result]
	mupp_check $name [expr {!$failed}] $result
	return [expr {!$failed}]
}

# "pc (/32): 0x08000040" -> 134217792
proc mupp_reg {name} {
	set text [reg $name]
	if {![regexp {(0x[0-9a-fA-F]+)} $text all value]} { error "no value in '$text'" }
	return [expr {$value + 0}]
}

proc mupp_word {addr} {
	return [expr {[lindex [read_memory $addr 32 1] 0] + 0}]
}

proc mupp_same {a b} {
	if {[llength $a] != [llength $b]} { return 0 }
	foreach x $a y $b { if {$x != $y} { return 0 } }
	return 1
}

proc mupp_state {} {
	return [[target current] curstate]
}

# TCK must not be above F_CPU / 6: set the target to 64 MHz first.
proc mupp_fast {speed} {
	stm32f1_clock_max
	adapter speed $speed
	if {[string match *jtag* [transport select]]} { jtag arp_init }
}

proc mupp_flash {image dump size speed} {
	reset halt
	echo [format "INFO dev_id=0x%03x flash_kb=%d" \
		[expr {[mupp_word 0xE0042000] & 0xfff}] [expr {[mupp_word 0x1FFFF7E0] & 0xffff}]]
	mupp_fast $speed
	if {![mupp_try "flash write" {flash write_image erase $image 0x08000000 bin}]} { return }
	mupp_try "verify" {verify_image $image 0x08000000 bin}
	mupp_try "read back" {dump_image $dump 0x08000000 $size}
}

proc mupp_debug {speed} {
	adapter speed 1000
	reset halt
	mupp_fast $speed
	set pc [mupp_reg pc]
	mupp_check "reset halt pc" [expr {$pc == 0x08000040}] [format 0x%08x $pc]
	set sp [mupp_reg sp]
	mupp_check "sp from vector table" [expr {$sp == 0x20005000}] [format 0x%08x $sp]

	set r1 [mupp_reg r1]
	step
	step
	set r1b [mupp_reg r1]
	mupp_check "two steps run one adds" [expr {$r1b == (($r1 + 1) & 0xffffffff)}] "r1 $r1 -> $r1b"

	bp 0x08000040 2 hw
	resume
	if {[catch {wait_halt 1000}]} {
		mupp_check "hardware breakpoint halts" 0 "no halt"
		halt
	} else {
		set pc [mupp_reg pc]
		set r1c [mupp_reg r1]
		mupp_check "hardware breakpoint halts" [expr {$pc == 0x08000040 && $r1c != $r1b}] \
			[format "pc 0x%08x, r1 %d -> %d" $pc $r1b $r1c]
	}
	rbp 0x08000040

	set words {0xdeadbeef 0x12345678 0xa5a5a5a5 0x5a5a5a5a}
	write_memory 0x20000000 32 $words
	mupp_check "RAM words" [mupp_same $words [read_memory 0x20000000 32 4]]
	# 4 KiB: the smallest STM32F103 has 6 KiB RAM.
	set block {}
	for {set i 0} {$i < 1024} {incr i} {
		lappend block [expr {($i * 2654435761 + 12345) & 0xffffffff}]
	}
	write_memory 0x20000100 32 $block
	mupp_check "RAM block 4 KiB" [mupp_same $block [read_memory 0x20000100 32 1024]]

	resume
	sleep 200
	mupp_check "running" [expr {[mupp_state] eq "running"}] [mupp_state]
	halt
	mupp_check "halt" [expr {[mupp_state] eq "halted"}] [mupp_state]
}

proc mupp_run {image dump size speed} {
	if {[catch {mupp_flash $image $dump $size $speed} err]} { mupp_check "flash steps" 0 $err }
	if {[catch {mupp_debug $speed} err]} { mupp_check "debug steps" 0 $err }
	adapter speed 1000
	reset run
}
"""
OPENOCD_EXPECTED = (
    "flash write", "verify", "read back", "reset halt pc", "sp from vector table",
    "two steps run one adds", "hardware breakpoint halts", "RAM words", "RAM block 4 KiB",
    "running", "halt",
)
# mupp_run reports these when mupp_flash or mupp_debug stops on an error.
STEP_GROUPS = ("flash steps", "debug steps")
CHECK_PATTERN = re.compile(r"^CHECK (PASS|FAIL) ([^:]+):(.*)$", re.MULTILINE)
INFO_PATTERN = re.compile(r"INFO dev_id=0x([0-9a-fA-F]+) flash_kb=(\d+)")

# STM32F1 DBGMCU_IDCODE device ID -> (pyOCD target, probe-rs chip) by flash KiB.
# Low and medium density have 1 KiB flash pages, high density 2 KiB pages,
# so the flash algorithm must match the density. The pyOCD targets of low
# and medium density come from the Keil.STM32F1xx_DFP pack.
CHIPS = {
    0x412: {32: ("stm32f103c6", "STM32F103C6")},
    0x410: {64: ("stm32f103c8", "STM32F103C8"), 128: ("stm32f103cb", "STM32F103CB")},
    0x414: {256: ("stm32f103rc", "STM32F103RC"), 384: ("stm32f103rc", "STM32F103RD"),
            512: ("stm32f103rc", "STM32F103RE")},
}


def make_image(size=DEFAULT_SIZE, seed=SEED):
    """Vector table (SP, reset vector at the loop), the loop `adds r1, #1` /
    `b` back to the adds at 0x08000040, then pseudo-random data to `size`."""
    vectors = struct.pack("<16I", STACK_TOP, *([LOOP_PC | 1] * 15))
    head = vectors + struct.pack("<2H", 0x3101, 0xE7FD)
    rnd = random.Random(seed)
    return head + rnd.randbytes(size - len(head))


def tcl_path(path):
    """A path as a Tcl word in braces. Tcl needs forward slashes."""
    return "{" + path.replace("\\", "/") + "}"


def chip_for(dev_id, flash_kb):
    """(pyOCD target, probe-rs chip) for an STM32F103, or None when not known."""
    return CHIPS.get(dev_id, {}).get(flash_kb)


def parse_checks(text):
    """[(name, passed, detail)] of the CHECK lines of an OpenOCD log."""
    return [(m.group(2).strip(), m.group(1) == PASS, m.group(3).strip()) for m in CHECK_PATTERN.finditer(text)]


def parse_chip_info(text):
    """(dev_id, flash_kb) from an OpenOCD log, or None."""
    match = INFO_PATTERN.search(text)
    return (int(match.group(1), 16), int(match.group(2))) if match else None


def parse_probe_rs_words(text):
    """The 32-bit words of the output of `probe-rs read b32`. A line can
    start with its address ("20000000: deadbeef ..."): that is not data."""
    words = []
    for line in text.splitlines():
        words += [int(word, 16) for word in re.findall(r"\b[0-9a-fA-F]{8}\b", line.split(":")[-1])]
    return words


def script_python(command):
    """The interpreter in the #! line of an installed Python command (pipx
    and venv installs have their own), or None."""
    path = shutil.which(command)
    try:
        with open(path, "rb") as f:
            line = f.readline().decode(errors="replace")
    except (TypeError, OSError):
        return None
    if not line.startswith("#!"):
        return None
    parts = line[2:].split()
    # "#!/usr/bin/env python3" names the interpreter in the second word.
    if parts and os.path.basename(parts[0]) == "env":
        parts = parts[1:]
    return shutil.which(parts[0]) if parts and "python" in os.path.basename(parts[0]) else None


def openocd_command(args, personality, transport, speed_khz, checks_path, image_path, dump_path):
    cmd = [args.openocd]
    for script_dir in args.search:
        cmd += ["-s", script_dir]
    cmd += OPENOCD_INTERFACES[personality]
    if args.serial:
        cmd += ["-c", f"adapter serial {args.serial}"]
    cmd += ["-c", f"transport select {transport}", "-f", "target/stm32f1x.cfg", "-f", CLOCK_SCRIPT, "-f", checks_path]
    # The target script sets its own adapter speed, so set the speed after it.
    for command in [
        f"adapter speed {SETUP_SPEED_KHZ}",
        "init",
        f"mupp_run {tcl_path(image_path)} {tcl_path(dump_path)} {len(args.image)} {speed_khz}",
        "shutdown",
    ]:
        cmd += ["-c", command]
    return cmd


def openocd_verdict(text, returncode, transport, image, dump):
    """(status, notes) of one OpenOCD run."""
    checks = parse_checks(text)
    if not checks:
        # OpenOCD did not connect: the log tail tells more than the check list.
        return FAIL, ["no check ran"] + text.strip().splitlines()[-3:] + [f"exit code {returncode}"]
    notes = [f"{name}: {detail[:120]}" for name, passed, detail in checks if not passed]
    names = {name for name, _, _ in checks}
    if names & set(STEP_GROUPS):
        # A step group stopped on an error: the Error lines tell why, not the missing checks.
        notes += [line for line in text.splitlines() if line.startswith("Error:")][:5]
    else:
        notes += [f"missing check: {name}" for name in OPENOCD_EXPECTED if name not in names]
    if dump is not None and dump != image:
        notes.append("read back differs from the image")
    if transport == "jtag" and f"tap/device found: {CPU_TAP_IDCODE}" not in text:
        notes.append(f"CPU TAP {CPU_TAP_IDCODE} not found")
    if returncode != 0:
        notes.append(f"exit code {returncode}")
    return (FAIL if notes else PASS), notes


class Runner:
    def __init__(self, args):
        self.args = args
        self.results = []  # (step, status, notes)
        self.chip = None

    def record(self, step, status, notes=()):
        self.results.append((step, status, list(notes)))
        print(f"{status} {step}")
        for note in notes:
            print(f"     {note}")

    def skipped(self, *tags):
        return any(tag in self.args.skip for tag in tags)

    def log_path(self, step):
        return os.path.join(self.args.log_dir, re.sub(r"[^A-Za-z0-9_.-]+", "_", step) + ".log")

    def run(self, step, cmd, timeout=STEP_TIMEOUT_S):
        """Runs one command and returns (returncode, output). The output goes
        into the log file while the command runs, so `tail -f` shows it."""
        path = self.log_path(step)
        print(f"---- {step}: {' '.join(cmd)}\n     log: {path}", flush=True)
        start = time.monotonic()
        # The Python tools print through a pipe: make them flush each line.
        env = dict(os.environ, PYTHONUNBUFFERED="1")
        with open(path, "w") as log:
            log.write(f"$ {' '.join(cmd)}\n")
            log.flush()
            header = log.tell()
            try:
                proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, env=env)
            except FileNotFoundError as error:
                log.write(f"{error}\n")
                return 127, str(error)
            try:
                code = proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                log.write(f"\nTIMEOUT after {timeout} s\n")
                code = -1
            log.write(f"\n[exit {code}, {time.monotonic() - start:.1f} s]\n")
        with open(path, errors="replace") as log:
            log.seek(header)
            return code, log.read()

    def run_simple(self, step, cmd):
        """A step that passes on exit code 0."""
        code, output = self.run(step, cmd)
        tail = [] if code == 0 else output.strip().splitlines()[-5:] + [f"exit code {code}"]
        self.record(step, PASS if code == 0 else FAIL, tail)
        return code == 0

    def python_tool(self, name, *tool_args):
        return [sys.executable, os.path.join(TOOLS_DIR, name), *tool_args]

    def switch(self, personality):
        step = f"switch to {personality}"
        ok = self.run_simple(step, self.python_tool("mupp_personality.py", *self.serial_args(), "switch", personality))
        if ok:
            time.sleep(self.args.settle)  # udev needs time for the CDC-ACM port
        return ok

    def serial_args(self):
        return ["--serial", self.args.serial] if self.args.serial else []

    def find_port(self, personality):
        if self.args.port:
            return self.args.port
        try:
            from serial.tools import list_ports
        except ImportError:
            return None
        vid, pid = USB_IDS[personality]
        deadline = time.monotonic() + 5
        while True:
            ports = [p.device for p in list_ports.comports()
                     if (p.vid, p.pid) == (vid, pid) and (not self.args.serial or p.serial_number == self.args.serial)]
            if ports or time.monotonic() > deadline:
                return ports[0] if ports else None
            time.sleep(0.2)

    def uart(self, personality):
        if self.skipped("uart"):
            return
        port = self.find_port(personality)
        if port is None:
            self.record(f"{personality} serial loopback", FAIL, ["no CDC-ACM port found (use --port)"])
        else:
            extra = [] if self.args.full_uart else QUICK_SERIAL_ARGS
            self.run_simple(f"{personality} serial loopback", self.python_tool("serial_loopback_test.py", port, *extra))
        if personality == "cmsis-dap":
            extra = [] if self.args.full_uart else QUICK_DAP_UART_ARGS
            if port is not None:
                extra += ["--serial-port", port]
            self.run_simple("cmsis-dap DAP_UART loopback",
                            self.python_tool("dap_uart_loopback_test.py", *self.serial_args(), *extra))

    def speeds(self, transport):
        return self.args.jtag_speeds if transport == "jtag" else self.args.swd_speeds

    def openocd(self, personality):
        if self.skipped("openocd"):
            return
        checks_path = os.path.join(self.args.log_dir, "mupp_checks.tcl")
        image_path = os.path.join(self.args.log_dir, "image.bin")
        with open(checks_path, "w") as f:
            f.write(OPENOCD_CHECKS)
        with open(image_path, "wb") as f:
            f.write(self.args.image)
        for transport in ("swd", "jtag"):
            for speed in self.speeds(transport):
                step = f"openocd {personality} {transport} {speed} kHz"
                dump_path = os.path.join(self.args.log_dir, re.sub(r"\W+", "_", step) + ".bin")
                if os.path.exists(dump_path):
                    os.remove(dump_path)
                code, output = self.run(step, openocd_command(self.args, personality, transport, speed,
                                                              checks_path, image_path, dump_path))
                dump = open(dump_path, "rb").read() if os.path.exists(dump_path) else None
                status, notes = openocd_verdict(output, code, transport, self.args.image, dump)
                self.record(step, status, notes)
                if self.chip is None and parse_chip_info(output):
                    self.select_chip(*parse_chip_info(output))

    def select_chip(self, dev_id, flash_kb):
        self.chip = (dev_id, flash_kb)
        known = chip_for(dev_id, flash_kb)
        print(f"     target: device ID 0x{dev_id:03x}, {flash_kb} KiB flash"
              + ("" if known else " (not known, using the defaults)"))
        if known:
            self.args.pyocd_target = self.args.pyocd_target or known[0]
            self.args.probe_rs_chip = self.args.probe_rs_chip or known[1]

    def pyocd(self):
        if self.skipped("pyocd"):
            return
        target = self.args.pyocd_target or DEFAULT_PYOCD_TARGET
        for transport in ("swd", "jtag"):
            for speed in self.speeds(transport):
                step = f"pyocd {transport} {speed} kHz"
                code, output = self.run(step, [self.args.pyocd_python, os.path.join(TOOLS_DIR, "pyocd_jtag_test.py"),
                                               str(speed * 1000), "--protocol", transport, "--target", target,
                                               *(["--uid", self.args.serial] if self.args.serial else [])])
                fails = [line for line in output.splitlines() if line.startswith("FAIL ")]
                ok = code == 0 and "RESULT PASS" in output and not fails
                notes = [] if ok else (fails or output.strip().splitlines()[-5:]) + [f"exit code {code}"]
                self.record(step, PASS if ok else FAIL, notes)

    def probe_rs(self):
        if self.skipped("probe-rs"):
            return
        image_path = os.path.join(self.args.log_dir, "image.bin")
        with open(image_path, "wb") as f:
            f.write(self.args.image)
        chip = self.args.probe_rs_chip or DEFAULT_PROBE_RS_CHIP
        vid, pid = USB_IDS["cmsis-dap"]
        probe = f"{vid:04x}:{pid:04x}" + (f":{self.args.serial}" if self.args.serial else "")
        image_words = list(struct.unpack(f"<{len(self.args.image) // 4}I", self.args.image))
        ram = [0xDEADBEEF, 0x12345678, 0xA5A5A5A5, 0x5A5A5A5A]
        for transport in ("swd", "jtag"):
            for speed in self.speeds(transport):
                step = f"probe-rs {transport} {speed} kHz"
                if transport == "jtag" and speed > PROBE_RS_JTAG_MAX_KHZ:
                    self.record(step, SKIP, [f"the target runs at 8 MHz: TCK must not be above {PROBE_RS_JTAG_MAX_KHZ} kHz"])
                    continue
                common = ["--chip", chip, "--protocol", transport, "--speed", str(speed), "--probe", probe,
                          "--non-interactive"]
                image_args = ["--binary-format", "bin", "--base-address", f"0x{FLASH_BASE:08x}", image_path]
                notes = []
                code, out = self.run(step + " download", [self.args.probe_rs, "download", *common,
                                                          "--disable-progressbars", *image_args])
                if code != 0:
                    notes.append(f"download: exit code {code}: " + " ".join(out.strip().splitlines()[-2:]))
                else:
                    # probe-rs verify exits with 0 on a mismatch: check the text.
                    code, out = self.run(step + " verify", [self.args.probe_rs, "verify", *common, *image_args])
                    if code != 0 or "failed" in out.lower():
                        notes.append("verify: " + " ".join(out.strip().splitlines()[-2:]))
                    code, out = self.run(step + " read", [self.args.probe_rs, "read", *common, "b32",
                                                          f"0x{FLASH_BASE:08x}", str(len(image_words))])
                    if code != 0 or parse_probe_rs_words(out) != image_words:
                        notes.append(f"read back differs from the image (exit code {code})")
                    code, out = self.run(step + " write RAM", [self.args.probe_rs, "write", *common, "b32",
                                                               "0x20000000", *(f"0x{w:08x}" for w in ram)])
                    code2, out = self.run(step + " read RAM", [self.args.probe_rs, "read", *common, "b32",
                                                               "0x20000000", str(len(ram))])
                    if code != 0 or code2 != 0 or parse_probe_rs_words(out) != ram:
                        notes.append(f"RAM write and read differ: {out.strip()[-80:]}")
                    code, out = self.run(step + " reset", [self.args.probe_rs, "reset", *common])
                    if code != 0:
                        notes.append(f"reset: exit code {code}")
                self.record(step, FAIL if notes else PASS, notes)

    def all(self):
        if not self.skipped("versaloon"):
            if self.switch("versaloon"):
                self.uart("versaloon")
                self.openocd("versaloon")
        if not self.skipped("cmsis-dap"):
            if self.switch("cmsis-dap"):
                self.uart("cmsis-dap")
                self.openocd("cmsis-dap")
                self.pyocd()
                self.probe_rs()

    def summary(self):
        print("\nResults (logs in %s):" % self.args.log_dir)
        for step, status, _ in self.results:
            print(f"  {status}  {step}")
        counts = {s: sum(1 for _, status, _ in self.results if status == s) for s in (PASS, FAIL, SKIP)}
        print(f"{counts[PASS]} passed, {counts[FAIL]} failed, {counts[SKIP]} skipped")
        return 1 if counts[FAIL] or not self.results else 0


def parse_speeds(text):
    try:
        speeds = [int(item) for item in text.split(",") if item.strip()]
    except ValueError:
        speeds = []
    if not speeds or any(speed <= 0 for speed in speeds):
        raise argparse.ArgumentTypeError(f"speeds must be positive kHz values: {text!r}")
    return speeds


SKIP_TAGS = ("versaloon", "cmsis-dap", "uart", "openocd", "pyocd", "probe-rs")


def parse_skip(text):
    tags = [tag.strip() for tag in text.split(",") if tag.strip()]
    bad = [tag for tag in tags if tag not in SKIP_TAGS]
    if bad:
        raise argparse.ArgumentTypeError(f"unknown skip tags {bad}: use {', '.join(SKIP_TAGS)}")
    return tags


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--skip", type=parse_skip, default=[],
                        help="comma-separated parts to skip: " + ", ".join(SKIP_TAGS))
    parser.add_argument("--swd-speeds", type=parse_speeds, default=DEFAULT_SWD_SPEEDS,
                        help="comma-separated SWD speeds in kHz (default: %(default)s)")
    parser.add_argument("--jtag-speeds", type=parse_speeds, default=DEFAULT_JTAG_SPEEDS,
                        help="comma-separated JTAG speeds in kHz (default: %(default)s)")
    parser.add_argument("--size", type=int, default=DEFAULT_SIZE, help="image size in bytes (default: %(default)s)")
    parser.add_argument("--serial", help="the serial number of the dongle (the chip UID)")
    parser.add_argument("--port", help="the CDC-ACM port (default: found by VID:PID)")
    parser.add_argument("--full-uart", action="store_true", help="run all baud rates and formats of the UART tests")
    parser.add_argument("--pyocd-target", help="pyOCD target type (default: from the device ID)")
    parser.add_argument("--pyocd-python", help="Python with pyOCD (default: the one of the pyocd command)")
    parser.add_argument("--probe-rs-chip", help="probe-rs chip name (default: from the device ID)")
    parser.add_argument("--settle", type=float, default=2.0, help="seconds to wait after a switch (default: 2)")
    parser.add_argument("--log-dir", help="directory for the logs (default: a new temporary directory)")
    parser.add_argument("--openocd", default="openocd", help="OpenOCD executable")
    parser.add_argument("-s", "--search", action="append", default=[], help="OpenOCD script directory")
    parser.add_argument("--probe-rs", default="probe-rs", help="probe-rs executable")
    args = parser.parse_args(argv)
    args.pyocd_python = args.pyocd_python or script_python("pyocd") or sys.executable
    if args.size < 0x100 or args.size % 4:
        parser.error("--size must be a multiple of 4 and at least 256")
    return args


def main(argv=None):
    args = parse_args(argv)
    args.image = make_image(args.size)
    if args.log_dir:
        os.makedirs(args.log_dir, exist_ok=True)
    else:
        args.log_dir = tempfile.mkdtemp(prefix="mupp-hw-test-")
    print("Logs:", args.log_dir)
    print("This test erases the flash of the target.")
    runner = Runner(args)
    try:
        runner.all()
    except KeyboardInterrupt:
        print("\nInterrupted")
        runner.record("interrupted", FAIL, ["the run did not end"])
    return runner.summary()


if __name__ == "__main__":
    sys.exit(main())
