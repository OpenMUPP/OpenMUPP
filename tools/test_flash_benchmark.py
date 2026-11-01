# SPDX-License-Identifier: GPL-3.0-or-later

"""Host tests for flash_benchmark.py. They use a fake OpenOCD, so they need
no OpenOCD and no hardware.

Run: python3 -m unittest discover -s tools -p "test_*.py"
"""

import argparse
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flash_benchmark as fb  # noqa: E402

FIRMWARE = "openmupp v0.1.0, board mini"


def openocd_log(size, write_s=1.5, verify_s=0.25, read_s=0.5, firmware=FIRMWARE):
    """A log in the format of OpenOCD 0.12."""
    return "\n".join([
        "Open On-Chip Debugger 0.12.0",
        "Info : Versaloon Library: 0.1",
        f"Info : {firmware}",
        "Info : USB_TO_XXX abilities: 0x00000008:0x000000C0:0xC0000002",
        "target clock: 64 MHz (HSI)",
        f"wrote {size} bytes from file /tmp/x/image.bin in {write_s:f}s (21.333 KiB/s)",
        f"verified {size} bytes from file /tmp/x/image.bin in {verify_s:f}s (128.000 KiB/s)",
        f"dumped {size} bytes in {read_s:f}s (64.000 KiB/s)",
    ])


class FakeOpenOcd:
    """Replaces subprocess.run. It writes the dump file like dump_image."""

    def __init__(self, returncode=0, corrupt=False, timeout=False, log=None):
        self.returncode = returncode
        self.corrupt = corrupt
        self.timeout = timeout
        self.log = log
        self.commands = []

    def __call__(self, cmd, capture_output, text, timeout):
        self.commands.append(cmd)
        if self.timeout:
            raise subprocess.TimeoutExpired(cmd, timeout, output=b"Info : partial")
        dump_cmd = next(c for c in cmd if c.startswith("dump_image "))
        dump_path = dump_cmd.split("{")[1].split("}")[0]
        image_cmd = next(c for c in cmd if c.startswith("flash write_image "))
        image_path = image_cmd.split("{")[1].split("}")[0]
        with open(image_path, "rb") as f:
            data = bytearray(f.read())
        if self.corrupt:
            data[10] ^= 0xFF
        with open(dump_path, "wb") as f:
            f.write(data)
        log = self.log if self.log is not None else openocd_log(len(data))
        return subprocess.CompletedProcess(cmd, self.returncode, stdout="", stderr=log)


def make_args(**overrides):
    args = argparse.Namespace(
        openocd="openocd", interface=fb.DEFAULT_INTERFACE, target=fb.DEFAULT_TARGET,
        search=[], extra=[], size=1024, timeout=10, clock_setup=True,
    )
    for key, value in overrides.items():
        setattr(args, key, value)
    return args


class ImageTest(unittest.TestCase):
    def test_same_seed_gives_same_image(self):
        self.assertEqual(fb.make_image(4096), fb.make_image(4096))
        self.assertEqual(len(fb.make_image(4096)), 4096)

    def test_image_is_not_blank_flash(self):
        self.assertNotEqual(fb.make_image(256), b"\xff" * 256)


class ParseOutputTest(unittest.TestCase):
    def test_reads_all_steps_and_firmware(self):
        result = fb.parse_output(openocd_log(32768))
        self.assertEqual(result["write_bytes"], 32768)
        self.assertAlmostEqual(result["write_s"], 1.5)
        self.assertAlmostEqual(result["verify_s"], 0.25)
        self.assertAlmostEqual(result["read_s"], 0.5)
        self.assertEqual(result["firmware"], FIRMWARE)
        self.assertEqual(result["target_clock"], "64 MHz (HSI)")

    def test_reads_old_firmware_text(self):
        result = fb.parse_output(openocd_log(1024, firmware="Versaloon(0x15)by Simon(compiled on Jan  1 2013)"))
        self.assertEqual(result["firmware"], "Versaloon(0x15)by Simon(compiled on Jan  1 2013)")

    def test_reads_firmware_text_without_abilities_line(self):
        for firmware in ("openmupp v1.0.0, board mini", "versaloon-ng v0.12.0, board mini"):
            log = openocd_log(1024, firmware=firmware).replace("USB_TO_XXX abilities", "other")
            self.assertEqual(fb.parse_output(log)["firmware"], firmware)

    def test_missing_steps_are_absent(self):
        result = fb.parse_output("Error: no device found")
        self.assertNotIn("write_s", result)
        self.assertNotIn("firmware", result)


class CommandTest(unittest.TestCase):
    def test_command_order(self):
        cmd = fb.openocd_command(make_args(extra=["reset_config srst_only"]), "swd", 2000, "/a/image.bin", "/a/dump.bin")
        self.assertLess(cmd.index("transport select swd"), cmd.index(fb.DEFAULT_TARGET))
        self.assertLess(cmd.index(fb.DEFAULT_TARGET), cmd.index(fb.CLOCK_SCRIPT))
        self.assertLess(cmd.index(fb.CLOCK_SCRIPT), cmd.index("reset_config srst_only"))
        # The target script sets 1000 kHz. The setup speed and the test speed come after it.
        order = [cmd.index(c) for c in ("reset_config srst_only", "adapter speed 1000", "init", "reset halt",
                                         "stm32f1_clock_max", "adapter speed 2000",
                                         "flash write_image erase {/a/image.bin} 0x08000000 bin")]
        self.assertEqual(order, sorted(order))
        self.assertIn("flash write_image erase {/a/image.bin} 0x08000000 bin", cmd)
        self.assertIn("dump_image {/a/dump.bin} 0x08000000 1024", cmd)
        self.assertEqual(cmd[-1], "shutdown")

    def test_jtag_syncs_tap_after_speed_change(self):
        cmd = fb.openocd_command(make_args(), "jtag", 9000, "/a/image.bin", "/a/dump.bin")
        self.assertEqual(cmd.index("jtag arp_init"), cmd.index("adapter speed 9000") + 2)
        self.assertLess(cmd.index("jtag arp_init"), cmd.index("flash write_image erase {/a/image.bin} 0x08000000 bin"))
        swd = fb.openocd_command(make_args(), "swd", 2000, "/a/image.bin", "/a/dump.bin")
        self.assertNotIn("jtag arp_init", swd)

    def test_no_clock_setup(self):
        cmd = fb.openocd_command(make_args(clock_setup=False), "jtag", 1000, "/a/image.bin", "/a/dump.bin")
        self.assertNotIn(fb.CLOCK_SCRIPT, cmd)
        self.assertNotIn("stm32f1_clock_max", cmd)
        self.assertLess(cmd.index("reset halt"), cmd.index("adapter speed 1000"))

    def test_clock_script_exists(self):
        with open(fb.CLOCK_SCRIPT) as f:
            self.assertIn("proc stm32f1_clock_max", f.read())

    def test_windows_path_uses_forward_slashes(self):
        self.assertEqual(fb.tcl_path(r"C:\tmp\image.bin"), "{C:/tmp/image.bin}")


class RunOnceTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.image = fb.make_image(1024)
        self.image_path = os.path.join(self.dir.name, "image.bin")
        self.dump_path = os.path.join(self.dir.name, "dump.bin")
        with open(self.image_path, "wb") as f:
            f.write(self.image)

    def tearDown(self):
        self.dir.cleanup()

    def run_with(self, fake):
        return fb.run_once(make_args(), "jtag", 9000, self.image, self.image_path, self.dump_path, runner=fake)

    def test_pass(self):
        result = self.run_with(FakeOpenOcd())
        self.assertTrue(result["ok"], result.get("error"))
        self.assertEqual(result["firmware"], FIRMWARE)
        self.assertEqual((result["transport"], result["speed_khz"]), ("jtag", 9000))

    def test_wrong_read_back_fails(self):
        result = self.run_with(FakeOpenOcd(corrupt=True))
        self.assertFalse(result["ok"])
        self.assertIn("not equal", result["error"])

    def test_exit_code_fails(self):
        result = self.run_with(FakeOpenOcd(returncode=1))
        self.assertFalse(result["ok"])
        self.assertIn("exit code 1", result["error"])

    def test_missing_step_fails(self):
        result = self.run_with(FakeOpenOcd(log="wrote 1024 bytes from file x in 1.0s (1 KiB/s)"))
        self.assertFalse(result["ok"])
        self.assertIn("verify, read", result["error"])

    def test_timeout_fails(self):
        result = self.run_with(FakeOpenOcd(timeout=True))
        self.assertFalse(result["ok"])
        self.assertIn("timeout", result["error"])

    def test_missing_clock_fails(self):
        log = openocd_log(1024).replace("target clock: 64 MHz (HSI)", "")
        result = self.run_with(FakeOpenOcd(log=log))
        self.assertFalse(result["ok"])
        self.assertIn("target clock", result["error"])

    def test_old_dump_is_removed(self):
        with open(self.dump_path, "wb") as f:
            f.write(self.image)
        result = self.run_with(lambda cmd, **kw: subprocess.CompletedProcess(cmd, 0, stdout=openocd_log(1024), stderr=""))
        self.assertFalse(result["ok"])
        self.assertIn("cannot read the dump", result["error"])


def run(transport, speed, write_s, ok=True):
    return {"transport": transport, "speed_khz": speed, "ok": ok,
            "write_s": write_s, "verify_s": 0.2, "read_s": 0.4, "total_s": write_s + 1}


def result(label, runs, size=32768):
    return {"label": label, "firmware": f"fw {label}", "size": size, "runs": runs}


class SummaryTest(unittest.TestCase):
    def test_median_uses_only_passed_runs(self):
        rows = fb.summarize([run("swd", 1000, 3.0), run("swd", 1000, 1.0), run("swd", 1000, 2.0),
                             run("swd", 1000, 99.0, ok=False)])
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["write_s"], 2.0)
        self.assertEqual((rows[0]["runs"], rows[0]["failed"]), (4, 1))

    def test_all_failed_gives_no_time(self):
        rows = fb.summarize([run("jtag", 1000, 1.0, ok=False)])
        self.assertIsNone(rows[0]["write_s"])
        self.assertIn("| - |", fb.summary_table(result("x", [run("jtag", 1000, 1.0, ok=False)])))

    def test_summary_table_rate(self):
        table = fb.summary_table(result("new", [run("jtag", 9000, 2.0)]))
        self.assertIn("| JTAG | 9000 | 2.00 | 16.0 |", table)


class CompareTest(unittest.TestCase):
    def test_not_slower(self):
        table, slower = fb.compare_table(result("old", [run("jtag", 1000, 4.0)]), result("new", [run("jtag", 1000, 2.0)]))
        self.assertEqual(slower, [])
        self.assertIn("| JTAG | 1000 | write | 4.00 | 2.00 | 0.50 |", table)

    def test_slower_is_reported(self):
        _, slower = fb.compare_table(result("old", [run("swd", 2000, 2.0)]), result("new", [run("swd", 2000, 3.0)]))
        self.assertIn("SWD 2000 kHz write", slower)
        self.assertIn("SWD 2000 kHz total", slower)

    def test_noise_is_not_slower(self):
        _, slower = fb.compare_table(result("old", [run("jtag", 9000, 1.31)]), result("new", [run("jtag", 9000, 1.33)]))
        self.assertEqual(slower, [])

    def test_speed_only_in_one_file(self):
        table, slower = fb.compare_table(result("old", [run("jtag", 1000, 4.0)]), result("new", [run("jtag", 9000, 1.0)]))
        self.assertIn("| JTAG | 9000 | write | - | 1.00 | - |", table)
        self.assertEqual(slower, [])

    def test_different_sizes_are_refused(self):
        with self.assertRaises(ValueError):
            fb.compare_table(result("old", [], size=1024), result("new", [], size=2048))

    def test_compare_command_fails_on_failed_runs(self):
        with tempfile.TemporaryDirectory() as d:
            paths = []
            runs = {"old": [run("jtag", 9000, 2.0)], "new": [run("jtag", 9000, 1.0, ok=False)]}
            for label in ("old", "new"):
                path = os.path.join(d, f"{label}.json")
                with open(path, "w") as f:
                    json.dump(result(label, runs[label]), f)
                paths.append(path)
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(fb.main(["compare", *paths]), 1)

    def test_compare_command_exit_code(self):
        with tempfile.TemporaryDirectory() as d:
            paths = []
            for label, write_s in (("old", 2.0), ("new", 3.0)):
                path = os.path.join(d, f"{label}.json")
                with open(path, "w") as f:
                    json.dump(result(label, [run("swd", 1000, write_s)]), f)
                paths.append(path)
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(fb.main(["compare", *paths]), 1)
                self.assertEqual(fb.main(["compare", paths[1], paths[0]]), 0)


class ArgumentTest(unittest.TestCase):
    def test_parse_speeds(self):
        self.assertEqual(fb.parse_speeds("1000,4500, 9000"), [1000, 4500, 9000])
        for bad in ("", "abc", "0", "-5"):
            with self.assertRaises(argparse.ArgumentTypeError):
                fb.parse_speeds(bad)

    def test_bad_transport(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(fb.main(["run", "--label", "x", "--transports", "spi"]), 2)

    def test_bad_size(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(fb.main(["run", "--label", "x", "--size", "1001"]), 2)


if __name__ == "__main__":
    unittest.main()
