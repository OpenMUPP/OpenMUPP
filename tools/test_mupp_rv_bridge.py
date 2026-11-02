# SPDX-License-Identifier: GPL-3.0-or-later

"""Host tests for mupp_rv_bridge.py. They need no pyusb and no hardware.

Run: python3 -m unittest discover -s tools -p "test_*.py"
"""

import contextlib
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mupp_rv_bridge as rb  # noqa: E402
from test_mupp_probe import FakeDevice, fake_usb  # noqa: E402


class RequestTest(unittest.TestCase):
    def test_modes(self):
        self.assertEqual(rb.bridge_request("on"), bytes([0x81, 0x01]))
        self.assertEqual(rb.bridge_request("off"), bytes([0x81, 0x00]))
        self.assertEqual(rb.bridge_request("auto"), bytes([0x81, 0x02]))
        self.assertEqual(rb.bridge_request("stored"), bytes([0x81, 0x03]))

    def test_store(self):
        self.assertEqual(rb.bridge_request("off", store=True), bytes([0x81, 0x80]))
        self.assertEqual(rb.bridge_request("on", store=True), bytes([0x81, 0x81]))
        self.assertEqual(rb.bridge_request("auto", store=True), bytes([0x81, 0x82]))


class CheckReplyTest(unittest.TestCase):
    def test_done(self):
        rb.check_reply([0x81, 0x00])

    def test_no_bridge(self):
        with self.assertRaisesRegex(rb.BridgeError, "no RISC-V bridge"):
            rb.check_reply([0xFF])

    def test_refused(self):
        with self.assertRaisesRegex(rb.BridgeError, "status 255"):
            rb.check_reply([0x81, 0xFF])

    def test_other_response(self):
        for reply in ([], [0x80, 0x00], [0x81]):
            with self.assertRaises(rb.BridgeError):
                rb.check_reply(reply)


class MainTest(unittest.TestCase):
    def run_main(self, argv, devices):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = rb.main(argv, fake_usb(devices))
        return result, stdout.getvalue(), stderr.getvalue()

    def test_on(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0x81, 0x00])
        result, out, _ = self.run_main(["on"], [device])
        self.assertEqual(result, 0)
        self.assertEqual(device.writes, [(0x03, bytes([0x81, 0x01]))])
        self.assertIn("is on", out)

    def test_off(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0x81, 0x00])
        self.assertEqual(self.run_main(["off"], [device])[0], 0)
        self.assertEqual(device.writes, [(0x03, bytes([0x81, 0x00]))])

    def test_auto(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0x81, 0x00])
        result, out, _ = self.run_main(["auto"], [device])
        self.assertEqual(result, 0)
        self.assertEqual(device.writes, [(0x03, bytes([0x81, 0x02]))])
        self.assertIn("is auto", out)

    def test_store_option(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0x81, 0x00])
        result, out, _ = self.run_main(["off", "--store"], [device])
        self.assertEqual(result, 0)
        self.assertEqual(device.writes, [(0x03, bytes([0x81, 0x80]))])
        self.assertIn("is off, stored", out)

    def test_store_of_stored(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0x81, 0x00])
        with self.assertRaises(SystemExit):
            self.run_main(["stored", "--store"], [device])
        self.assertEqual(device.writes, [])

    def test_failed_store(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0x81, 0xFF])
        result, _, err = self.run_main(["on", "--store"], [device])
        self.assertEqual(result, 1)
        self.assertIn("failed store", err)

    def test_image_without_bridge(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0xFF])
        result, _, err = self.run_main(["on"], [device])
        self.assertEqual(result, 1)
        self.assertIn("MUPP_CMSIS_DAP_RV_BRIDGE", err)

    def test_no_response(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=None)
        result, _, err = self.run_main(["on"], [device])
        self.assertEqual(result, 1)
        self.assertIn("Cannot send", err)

    def test_only_cmsis_dap(self):
        versaloon = FakeDevice(0x0483, 0xA038)
        result, _, err = self.run_main(["on"], [versaloon])
        self.assertEqual(result, 1)
        self.assertIn("No dongle", err)
        self.assertEqual(versaloon.writes, [])

    def test_more_than_one(self):
        a = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", serial="A", reply=[0x81, 0x00])
        b = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", serial="B", reply=[0x81, 0x00])
        self.assertEqual(self.run_main(["on"], [a, b])[0], 1)
        self.assertEqual(self.run_main(["--serial", "B", "on"], [a, b])[0], 0)
        self.assertEqual(a.writes, [])
        self.assertEqual(len(b.writes), 1)


if __name__ == "__main__":
    unittest.main()
