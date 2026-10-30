"""Host tests for probe_hw_test.py. They need no tools and no hardware.

Run: python3 -m unittest discover -s tools -p "test_*.py"
"""

import argparse
import contextlib
import io
import os
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_hw_test as hw  # noqa: E402


def silence(test):
    """Hides the step lines of the Runner: in a CI log they look like hardware failures."""
    redirect = contextlib.redirect_stdout(io.StringIO())
    redirect.__enter__()
    test.addCleanup(redirect.__exit__, None, None, None)


def openocd_log(failed=(), jtag_tap=True, chip="0x414 flash_kb=512"):
    lines = ["Open On-Chip Debugger 0.12.0"]
    if jtag_tap:
        lines.append("Info : JTAG tap: stm32f1x.cpu tap/device found: 0x3ba00477 (mfg: 0x23b)")
    lines.append(f"INFO dev_id={chip}")
    for name in hw.OPENOCD_EXPECTED:
        lines.append(f"CHECK {'FAIL' if name in failed else 'PASS'} {name}: detail")
    return "\n".join(lines)


class ImageTest(unittest.TestCase):
    def test_vector_table_and_loop(self):
        image = hw.make_image(1024)
        self.assertEqual(len(image), 1024)
        sp, reset = struct.unpack_from("<2I", image)
        self.assertEqual(sp, hw.STACK_TOP)
        self.assertEqual(reset, hw.LOOP_PC | 1)
        self.assertEqual(struct.unpack_from("<2H", image, 0x40), (0x3101, 0xE7FD))

    def test_same_seed_same_image(self):
        self.assertEqual(hw.make_image(), hw.make_image())


class ParseTest(unittest.TestCase):
    def test_checks(self):
        text = "CHECK PASS flash write: wrote 32768 bytes\nnoise\nCHECK FAIL RAM words: \n"
        self.assertEqual(hw.parse_checks(text),
                         [("flash write", True, "wrote 32768 bytes"), ("RAM words", False, "")])

    def test_chip_info(self):
        self.assertEqual(hw.parse_chip_info("x\nINFO dev_id=0x410 flash_kb=64\n"), (0x410, 64))
        self.assertIsNone(hw.parse_chip_info("no info"))

    def test_probe_rs_words(self):
        self.assertEqual(hw.parse_probe_rs_words("deadbeef 00000001\n12345678\n"), [0xDEADBEEF, 1, 0x12345678])

    def test_probe_rs_words_with_address(self):
        text = "20000000: deadbeef 12345678\n20000008: a5a5a5a5 5a5a5a5a\n"
        self.assertEqual(hw.parse_probe_rs_words(text), [0xDEADBEEF, 0x12345678, 0xA5A5A5A5, 0x5A5A5A5A])

    def test_script_python(self):
        with tempfile.TemporaryDirectory() as d:
            for name, line in (("venv", f"#!{sys.executable}\n"), ("envpy", "#!/usr/bin/env python3\n"),
                               ("shell", "#!/bin/sh\n")):
                path = os.path.join(d, name)
                with open(path, "w") as f:
                    f.write(line)
                os.chmod(path, 0o755)
            with mock.patch.dict(os.environ, {"PATH": d + os.pathsep + os.environ["PATH"]}):
                self.assertEqual(hw.script_python("venv"), sys.executable)
                self.assertEqual(hw.script_python("envpy"), shutil.which("python3"))
                self.assertIsNone(hw.script_python("shell"))
                self.assertIsNone(hw.script_python("no-such-command"))

    def test_chip_for(self):
        self.assertEqual(hw.chip_for(0x410, 64), ("stm32f103c8", "STM32F103C8"))
        self.assertEqual(hw.chip_for(0x414, 512), ("stm32f103rc", "STM32F103RE"))
        self.assertIsNone(hw.chip_for(0x418, 256))


class VerdictTest(unittest.TestCase):
    image = hw.make_image(256)

    def test_pass(self):
        self.assertEqual(hw.openocd_verdict(openocd_log(), 0, "jtag", self.image, self.image), (hw.PASS, []))

    def test_failed_check(self):
        status, notes = hw.openocd_verdict(openocd_log(failed=("verify",)), 0, "swd", self.image, self.image)
        self.assertEqual(status, hw.FAIL)
        self.assertEqual(notes, ["verify: detail"])

    def test_no_connection(self):
        status, notes = hw.openocd_verdict("Info : x\nError: no device", 1, "swd", self.image, None)
        self.assertEqual((status, notes), (hw.FAIL, ["no check ran", "Info : x", "Error: no device", "exit code 1"]))

    def test_missing_check_and_exit_code(self):
        log = openocd_log().replace("CHECK PASS halt: detail", "")
        status, notes = hw.openocd_verdict(log, 1, "swd", self.image, self.image)
        self.assertEqual((status, notes), (hw.FAIL, ["missing check: halt", "exit code 1"]))

    def test_step_group_error_shows_error_lines(self):
        log = "Error: SWD DPIDR read failed\nCHECK FAIL flash steps: Traceback\nCHECK FAIL debug steps: Traceback"
        status, notes = hw.openocd_verdict(log, 1, "swd", self.image, None)
        self.assertEqual((status, notes), (hw.FAIL, ["flash steps: Traceback", "debug steps: Traceback",
                                                     "Error: SWD DPIDR read failed",
                                                     "exit code 1"]))

    def test_read_back_differs(self):
        status, notes = hw.openocd_verdict(openocd_log(), 0, "swd", self.image, b"\xff" * 256)
        self.assertEqual((status, notes), (hw.FAIL, ["read back differs from the image"]))

    def test_jtag_needs_cpu_tap(self):
        status, notes = hw.openocd_verdict(openocd_log(jtag_tap=False), 0, "jtag", self.image, self.image)
        self.assertEqual(status, hw.FAIL)
        self.assertIn("CPU TAP 0x3ba00477 not found", notes)
        self.assertEqual(hw.openocd_verdict(openocd_log(jtag_tap=False), 0, "swd", self.image, self.image)[0],
                         hw.PASS)


class ArgsTest(unittest.TestCase):
    def test_defaults(self):
        args = hw.parse_args([])
        self.assertEqual((args.swd_speeds, args.jtag_speeds, args.skip), ([1000, 2000, 4000], [1000, 9000], []))

    def test_skip(self):
        self.assertEqual(hw.parse_skip("probe-rs, uart"), ["probe-rs", "uart"])
        with self.assertRaises(argparse.ArgumentTypeError):
            hw.parse_skip("probers")

    def test_bad_speeds(self):
        for text in ("", "0", "fast"):
            with self.assertRaises(argparse.ArgumentTypeError):
                hw.parse_speeds(text)

    def test_openocd_command(self):
        args = hw.parse_args(["-s", "scripts"])
        args.image = hw.make_image(256)
        cmd = hw.openocd_command(args, "cmsis-dap", "jtag", 9000, "c.tcl", "C:\\img.bin", "d.bin")
        self.assertEqual(cmd[:3], ["openocd", "-s", "scripts"])
        self.assertIn("cmsis-dap vid_pid 0x0403 0x9b3c", cmd)
        self.assertIn("transport select jtag", cmd)
        self.assertIn("vsl_run {C:/img.bin} {d.bin} 256 9000", cmd)
        self.assertEqual(cmd[-1], "shutdown")


class RunnerTest(unittest.TestCase):
    def test_order(self):
        args = hw.parse_args([])
        calls = []
        runner = hw.Runner(args)
        runner.switch = lambda p: calls.append(("switch", p)) or True
        runner.openocd = lambda p: calls.append(("openocd", p))
        runner.pyocd = lambda: calls.append(("pyocd",))
        runner.uart = lambda p: calls.append(("uart", p))
        runner.probe_rs = lambda: calls.append(("probe-rs",))
        runner.all()
        self.assertEqual(calls, [("switch", "versaloon"), ("uart", "versaloon"), ("openocd", "versaloon"),
                                 ("switch", "cmsis-dap"), ("uart", "cmsis-dap"), ("openocd", "cmsis-dap"),
                                 ("pyocd",), ("probe-rs",)])

    def test_failed_switch_stops_the_personality(self):
        runner = hw.Runner(hw.parse_args(["--skip", "cmsis-dap"]))
        runner.switch = lambda p: False
        runner.openocd = lambda p: self.fail("openocd after a failed switch")
        runner.uart = lambda p: self.fail("uart after a failed switch")
        runner.all()

    def test_select_chip_keeps_the_options(self):
        silence(self)
        runner = hw.Runner(hw.parse_args(["--probe-rs-chip", "STM32F103ZE"]))
        runner.select_chip(0x410, 64)
        self.assertEqual((runner.args.pyocd_target, runner.args.probe_rs_chip), ("stm32f103c8", "STM32F103ZE"))


class StepRunTest(unittest.TestCase):
    """Runner.run with real child processes and a real log directory."""

    def setUp(self):
        silence(self)
        self.dir = tempfile.TemporaryDirectory()
        self.runner = hw.Runner(hw.parse_args(["--log-dir", self.dir.name]))

    def tearDown(self):
        self.dir.cleanup()

    def test_output_and_exit_code(self):
        code, output = self.runner.run("ok", [sys.executable, "-c", "print('hi'); raise SystemExit(3)"])
        self.assertEqual(code, 3)
        # The output has no command header, so the verdicts see only the tool output.
        self.assertTrue(output.startswith("hi\n"), output)
        with open(self.runner.log_path("ok")) as log:
            self.assertTrue(log.read().startswith("$ "))

    def test_no_stdin(self):
        code, output = self.runner.run("stdin", [sys.executable, "-c", "input()"])
        self.assertNotEqual(code, 0)
        self.assertIn("EOFError", output)

    def test_timeout(self):
        code, output = self.runner.run("slow", [sys.executable, "-c", "import time; time.sleep(5)"], timeout=0.5)
        self.assertEqual(code, -1)
        self.assertIn("TIMEOUT", output)

    def test_missing_tool(self):
        self.assertEqual(self.runner.run("missing", ["no-such-tool-for-the-test"])[0], 127)


def stub_runner(test, argv, replies):
    """A Runner whose run() answers from `replies`: {step suffix: (code, output)}."""
    silence(test)
    runner = hw.Runner(hw.parse_args(argv))
    runner.args.image = hw.make_image(256)
    runner.args.log_dir = tempfile.mkdtemp()
    test.addCleanup(shutil.rmtree, runner.args.log_dir)
    runner.calls = []

    def run(step, cmd, timeout=hw.STEP_TIMEOUT_S):
        runner.calls.append((step, cmd))
        for prefix, reply in replies.items():
            if step.endswith(prefix):
                return reply
        return 0, ""
    runner.run = run
    return runner


def words(data):
    return " ".join(f"{w:08x}" for w in struct.unpack(f"<{len(data) // 4}I", data))


class PyocdVerdictTest(unittest.TestCase):
    def verdicts(self, code, output, argv=()):
        runner = stub_runner(self, ["--swd-speeds", "1000", "--jtag-speeds", "1000", *argv], {"kHz": (code, output)})
        runner.pyocd()
        return runner

    def test_pass(self):
        runner = self.verdicts(0, "PASS flash read-back\nRESULT PASS\n")
        self.assertEqual([status for _, status, _ in runner.results], [hw.PASS, hw.PASS])

    def test_fail_line_fails_the_step(self):
        runner = self.verdicts(0, "FAIL RAM write/read\nRESULT PASS\n")
        self.assertEqual(runner.results[0][1], hw.FAIL)
        self.assertIn("FAIL RAM write/read", runner.results[0][2])

    def test_no_result_line_fails_the_step(self):
        self.assertEqual(self.verdicts(0, "Traceback ...\n").results[0][1], hw.FAIL)

    def test_serial_goes_to_pyocd(self):
        runner = self.verdicts(0, "RESULT PASS\n", ["--serial", "ABC123"])
        self.assertIn("--uid", runner.calls[0][1])
        self.assertEqual(runner.calls[0][1][-1], "ABC123")


class ProbeRsVerdictTest(unittest.TestCase):
    ram = "20000000: deadbeef 12345678 a5a5a5a5 5a5a5a5a\n"

    def verdict(self, replies, speeds=("--jtag-speeds", "1000")):
        runner = stub_runner(self, ["--swd-speeds", "1000", *speeds], replies)
        runner.probe_rs()
        return runner

    def good(self, image):
        return {" read": (0, "08000000: " + words(image)), " read RAM": (0, self.ram)}

    def test_pass(self):
        runner = self.verdict(self.good(hw.make_image(256)))
        self.assertEqual([status for _, status, _ in runner.results], [hw.PASS, hw.PASS])

    def test_read_back_differs(self):
        replies = self.good(hw.make_image(256))
        replies[" read"] = (0, "08000000: " + words(b"\xff" * 256))
        self.assertEqual(self.verdict(replies).results[0][1], hw.FAIL)

    def test_verify_text_fails_the_step(self):
        replies = self.good(hw.make_image(256))
        replies[" verify"] = (0, "Verification failed: contents do not match")
        self.assertEqual(self.verdict(replies).results[0][1], hw.FAIL)

    def test_failed_download_skips_the_rest(self):
        runner = self.verdict({" download": (1, "Error: no probe")})
        self.assertEqual(runner.results[0][1], hw.FAIL)
        self.assertFalse(any(step.endswith(" verify") for step, _ in runner.calls))

    def test_jtag_above_the_limit_is_skipped(self):
        runner = self.verdict(self.good(hw.make_image(256)), ("--jtag-speeds", "1333,1334"))
        self.assertEqual([(step, status) for step, status, _ in runner.results][1:],
                         [("probe-rs jtag 1333 kHz", hw.PASS), ("probe-rs jtag 1334 kHz", hw.SKIP)])


if __name__ == "__main__":
    unittest.main()
