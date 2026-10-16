"""Host tests for probe_hw_test.py. They need no tools and no hardware.

Run: python3 -m unittest discover -s tools -p "test_*.py"
"""

import argparse
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_hw_test as hw  # noqa: E402


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
        self.assertEqual((args.swd_speeds, args.jtag_speeds, args.skip), ([1000, 2000], [1000, 9000], []))

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
        runner = hw.Runner(hw.parse_args(["--probe-rs-chip", "STM32F103ZE"]))
        runner.select_chip(0x410, 64)
        self.assertEqual((runner.args.pyocd_target, runner.args.probe_rs_chip), ("stm32f103c8", "STM32F103ZE"))


if __name__ == "__main__":
    unittest.main()
