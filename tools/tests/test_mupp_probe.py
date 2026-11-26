# SPDX-License-Identifier: GPL-3.0-or-later

"""Host tests for mupp_probe.py. They need no pyusb and no hardware.

Run: python3 -m unittest discover -s tools -p "test_*.py"
"""

import argparse
import contextlib
import errno
import io
import os
import sys
import types
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import mupp_probe as mp  # noqa: E402


class PersonalityOfTest(unittest.TestCase):
    def test_known_ids(self):
        self.assertEqual(mp.personality_of(0x0483, 0xA038), "versaloon")
        self.assertEqual(mp.personality_of(0x1A86, 0x8010), "wch-link")

    def test_legacy_cmsis_dap_id(self):
        self.assertEqual(mp.personality_of(0x0403, 0x9B3C), "cmsis-dap")

    def test_shared_id_is_unknown(self):
        self.assertIsNone(mp.personality_of(0x1209, 0x0001))

    def test_bootloader_id(self):
        self.assertEqual(mp.personality_of(0x0483, 0x5740), "bootloader")
        self.assertEqual(mp.personality_of(0x0483, 0xDF11), "bootloader")

    def test_unknown_id(self):
        self.assertIsNone(mp.personality_of(0x0483, 0x5741))


class SwitchRequestTest(unittest.TestCase):
    def test_versaloon_sends_switch_personality(self):
        self.assertEqual(mp.switch_request("versaloon", "cmsis-dap"), bytes([0x0E, 0x01]))

    def test_cmsis_dap_sends_vendor0(self):
        self.assertEqual(mp.switch_request("cmsis-dap", "versaloon"), bytes([0x80, 0x00]))

    def test_code_of_wch_link(self):
        self.assertEqual(mp.switch_request("versaloon", "wch-link"), bytes([0x0E, 0x02]))
        self.assertEqual(mp.switch_request("cmsis-dap", "wch-link"), bytes([0x80, 0x02]))

    def test_bootloader_code(self):
        self.assertEqual(mp.switch_request("cmsis-dap", "bootloader"), bytes([0x80, 0xB0]))

    def test_versaloon_sends_fw_update_for_the_bootloader(self):
        self.assertEqual(mp.switch_request("versaloon", "bootloader"), bytes([0x0F, 0xAA]))

    def test_wch_link_has_no_command(self):
        with self.assertRaises(mp.ProbeError):
            mp.switch_request("wch-link", "versaloon")


class CheckReplyTest(unittest.TestCase):
    def test_versaloon_accepted(self):
        mp.check_reply("versaloon", [0x00])

    def test_versaloon_refused(self):
        with self.assertRaisesRegex(mp.ProbeError, "one personality"):
            mp.check_reply("versaloon", [0x01])

    def test_versaloon_wrong_size(self):
        with self.assertRaises(mp.ProbeError):
            mp.check_reply("versaloon", [])

    def test_bootloader_refused(self):
        with self.assertRaisesRegex(mp.ProbeError, "no bootloader entry"):
            mp.check_reply("versaloon", [0x01], "bootloader")
        with self.assertRaisesRegex(mp.ProbeError, "status 255.*no bootloader entry"):
            mp.check_reply("cmsis-dap", [0x80, 0xFF], "bootloader")

    def test_cmsis_dap_accepted(self):
        mp.check_reply("cmsis-dap", [0x80, 0x00])

    def test_cmsis_dap_refused(self):
        with self.assertRaisesRegex(mp.ProbeError, "refused .*status 255.*one personality.*wch-link"):
            mp.check_reply("cmsis-dap", [0x80, 0xFF], "wch-link")

    def test_cmsis_dap_invalid(self):
        with self.assertRaisesRegex(mp.ProbeError, "one personality"):
            mp.check_reply("cmsis-dap", [0xFF])

    def test_cmsis_dap_other_response(self):
        with self.assertRaises(mp.ProbeError):
            mp.check_reply("cmsis-dap", [0x00, 0x00])


class FakeUsbError(Exception):
    def __init__(self, strerror, error_code=None, errno_=None):
        super().__init__(strerror)
        self.backend_error_code = error_code
        self.errno = errno_


def stall():
    return FakeUsbError("Pipe error", -9, errno.EPIPE)


class FakeDevice:
    """A dongle. ctrl_error and reply select how it answers."""

    def __init__(self, vid, pid, product="", serial="SN1", ctrl_error=None, reply=None, manufacturer="OpenMUPP", claim_error=None):
        self.idVendor = vid
        self.idProduct = pid
        self.iManufacturer = 1
        self.iProduct = 2
        self.iSerialNumber = 3
        self.bus = 1
        self.address = 7
        self.strings = {1: manufacturer, 2: product, 3: serial}
        self.ctrl_error = ctrl_error
        self.reply = reply
        self.claim_error = claim_error
        self.interface_calls = []
        self.ctrl_transfers = []
        self.writes = []
        self.read_timeouts = []

    def ctrl_transfer(self, *args):
        self.ctrl_transfers.append(args)
        if self.ctrl_error is not None:
            raise self.ctrl_error

    def is_kernel_driver_active(self, _interface):
        return False

    def write(self, endpoint, data, _timeout):
        self.writes.append((endpoint, bytes(data)))

    def read(self, _endpoint, _size, timeout):
        self.read_timeouts.append(timeout)
        if self.reply is None:
            raise FakeUsbError("Operation timed out", -7, errno.ETIMEDOUT)
        # A function gives the reply to the last request
        if callable(self.reply):
            return self.reply(self.writes[-1][1])
        return self.reply


def get_string(device, index):
    value = device.strings[index]
    if isinstance(value, Exception):
        raise value
    return value


def fake_claim(device, _interface):
    device.interface_calls.append("claim")
    if device.claim_error is not None:
        raise device.claim_error


def fake_usb(devices):
    """A module like pyusb that finds the given devices."""
    usb = types.SimpleNamespace()

    def find(find_all, idVendor, idProduct):
        return [d for d in devices if (d.idVendor, d.idProduct) == (idVendor, idProduct)]

    usb.core = types.SimpleNamespace(USBError=FakeUsbError, find=find)
    usb.util = types.SimpleNamespace(
        get_string=get_string,
        claim_interface=fake_claim,
        release_interface=lambda device, interface: device.interface_calls.append("release"),
        dispose_resources=lambda device: device.interface_calls.append("dispose"),
    )
    return usb


class FindDevicesTest(unittest.TestCase):
    def test_wch_link_needs_the_product_string(self):
        ours = FakeDevice(0x1209, 0x0001, "OpenMUPP WCH-Link")
        dap = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP")
        old = FakeDevice(0x1209, 0x0001, "Versaloon WCH-Link")
        other = FakeDevice(0x1209, 0x0001, "Some test device")
        real = FakeDevice(0x1A86, 0x8010, "WCH-Link", manufacturer="wch.cn")
        ours_wch_id = FakeDevice(0x1A86, 0x8010, "OpenMUPP WCH-Link")
        ours_probe_rs = FakeDevice(0x1A86, 0x8010, "WCH-Link")
        found = mp.find_devices(fake_usb([ours, dap, old, other, real, ours_wch_id, ours_probe_rs]))
        self.assertEqual(
            found,
            [("cmsis-dap", dap), ("wch-link", ours), ("wch-link", old), ("wch-link", ours_wch_id), ("wch-link", ours_probe_rs)],
        )

    def test_unreadable_product_string(self):
        for error in (FakeUsbError("Access denied", -3, errno.EACCES), NotImplementedError("not supported"), ValueError()):
            device = FakeDevice(0x1209, 0x0001, error)
            self.assertEqual(mp.find_devices(fake_usb([device])), [])

    def test_unreadable_serial(self):
        device = FakeDevice(0x0483, 0xA038, serial=NotImplementedError("not supported"))
        self.assertEqual(mp.find_devices(fake_usb([device])), [("versaloon", device)])
        self.assertEqual(mp.find_devices(fake_usb([device]), "A"), [])

    def test_bootloader_needs_the_product_string(self):
        ours = FakeDevice(0x0483, 0x5740, "Bootloader")
        other = FakeDevice(0x0483, 0x5740, "STM32 Virtual ComPort")
        self.assertEqual(mp.find_devices(fake_usb([ours, other])), [("bootloader", ours)])

    def test_rom_dfu_needs_no_product_string(self):
        dfu = FakeDevice(0x0483, 0xDF11, "DFU in FS Mode", serial="ROM")
        self.assertEqual(mp.find_devices(fake_usb([dfu])), [("bootloader", dfu)])

    def test_rom_dfu_has_a_strict_serial_check(self):
        # Another STM32 board in ROM DFU mode does not hide the selected dongle
        ours = FakeDevice(0x0483, 0xA038, serial="SN1")
        dfu = FakeDevice(0x0483, 0xDF11, serial="ROM")
        self.assertEqual(mp.find_devices(fake_usb([ours, dfu]), "SN1"), [("versaloon", ours)])
        self.assertEqual(mp.find_devices(fake_usb([ours, dfu]), "SN1", mp.NO_SERIAL_IDS), [("versaloon", ours), ("bootloader", dfu)])

    def test_unreadable_product_string_is_listed(self):
        denied = FakeDevice(0x1209, 0x0001, FakeUsbError("Access denied", -3, errno.EACCES))
        other = FakeDevice(0x1209, 0x0001, "Some test device")
        unreadable = []
        self.assertEqual(mp.find_devices(fake_usb([denied, other]), unreadable=unreadable), [])
        self.assertEqual(unreadable, [denied])

    def test_usb_id_needs_no_product_string(self):
        other = FakeDevice(0x1209, 0x0001, "Some test device")
        denied = FakeDevice(0x1209, 0x0001, FakeUsbError("Access denied", -3, errno.EACCES))
        unreadable = []
        found = mp.find_devices(fake_usb([other, denied]), usb_id=(0x1209, 0x0001), unreadable=unreadable)
        self.assertEqual(found, [("unknown", other), ("unknown", denied)])
        self.assertEqual(unreadable, [])

    def test_usb_id_selects_only_this_id(self):
        a = FakeDevice(0x0483, 0xA038)
        custom = FakeDevice(0x1234, 0x5678)
        self.assertEqual(mp.find_devices(fake_usb([a, custom]), usb_id=(0x1234, 0x5678)), [("unknown", custom)])

    def test_serial(self):
        a = FakeDevice(0x0483, 0xA038, serial="A")
        b = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", serial="B")
        self.assertEqual(mp.find_devices(fake_usb([a, b]), "B"), [("cmsis-dap", b)])


class ParseUsbIdTest(unittest.TestCase):
    def test_hex(self):
        self.assertEqual(mp.parse_usb_id("1209:0001"), (0x1209, 1))
        self.assertEqual(mp.parse_usb_id("1A86:8010"), (0x1A86, 0x8010))

    def test_bad_text(self):
        for text in ("1209", "1209:", "xyz:1", "1:2:3", "10000:1"):
            with self.assertRaises(argparse.ArgumentTypeError):
                mp.parse_usb_id(text)


class CommandShowTest(unittest.TestCase):
    def show(self, devices, usb_id=None):
        args = argparse.Namespace(serial=None, id=usb_id, active=None)
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = mp.command_show(fake_usb(devices), args)
        return result, stdout.getvalue(), stderr.getvalue()

    def test_unreadable_device_is_reported(self):
        denied = FakeDevice(0x1209, 0x0001, FakeUsbError("Access denied", -3, errno.EACCES))
        result, out, err = self.show([denied])
        self.assertEqual(result, 1)
        self.assertIn("No dongle found", out)
        self.assertIn("1209:0001 on bus 1 address 7", err)

    def test_report_next_to_a_found_dongle(self):
        denied = FakeDevice(0x1209, 0x0001, FakeUsbError("Access denied", -3, errno.EACCES))
        result, out, err = self.show([FakeDevice(0x0483, 0xA038), denied])
        self.assertEqual(result, 0)
        self.assertIn("versaloon  0483:a038", out)
        self.assertIn("1209:0001 on bus 1 address 7", err)

    def test_unknown_id(self):
        result, out, _ = self.show([FakeDevice(0x1234, 0x5678)], (0x1234, 0x5678))
        self.assertEqual(result, 0)
        self.assertIn("unknown  1234:5678", out)


class MainTest(unittest.TestCase):
    def test_active_needs_an_unknown_id(self):
        for argv in (["--active", "wch-link", "show"], ["--id", "0483:a038", "--active", "wch-link", "show"]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                mp.main(argv)


class WaitForTest(unittest.TestCase):
    def test_bootloader_wait_skips_the_serial_of_the_rom_dfu(self):
        dfu = FakeDevice(0x0483, 0xDF11, serial="ROM")
        self.assertTrue(mp.wait_for(fake_usb([dfu]), "bootloader", "SN1", 1.0))

    def test_other_waits_check_the_serial(self):
        device = FakeDevice(0x0483, 0xA038, serial="ROM")
        self.assertFalse(mp.wait_for(fake_usb([device]), "versaloon", "SN1", 0.0))


class CommandSwitchTest(unittest.TestCase):
    def setUp(self):
        self.waited = []
        self.wait_result = True
        original = mp.wait_for

        def wait_for(_usb, target, serial, _timeout_s):
            self.waited.append((target, serial))
            return self.wait_result

        mp.wait_for = wait_for
        self.addCleanup(setattr, mp, "wait_for", original)

    def switch(self, device, target, usb_id=None, active=None):
        args = argparse.Namespace(serial=None, personality=target, id=usb_id, active=active)
        stderr = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
            result = mp.command_switch(fake_usb([device]), args)
        self.stderr = stderr.getvalue()
        return result

    def test_ep0_request(self):
        device = FakeDevice(0x0483, 0xA038)
        self.assertEqual(self.switch(device, "wch-link"), 0)
        self.assertEqual(device.ctrl_transfers, [(0x40, 0x0E, 2, 0, None, mp.TIMEOUT_MS)])
        self.assertEqual(device.writes, [])
        self.assertEqual(self.waited, [("wch-link", "SN1")])

    def test_ep0_request_from_wch_link(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP WCH-Link")
        self.assertEqual(self.switch(device, "cmsis-dap"), 0)
        self.assertEqual(device.ctrl_transfers, [(0x40, 0x0E, 1, 0, None, mp.TIMEOUT_MS)])
        self.assertEqual(self.waited, [("cmsis-dap", "SN1")])

    def test_ep0_no_status_stage_waits(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", ctrl_error=FakeUsbError("No such device", -4, errno.ENODEV))
        self.assertEqual(self.switch(device, "versaloon"), 0)
        self.assertEqual(device.writes, [])
        self.assertEqual(self.waited, [("versaloon", "SN1")])

    def test_ep0_timeout_and_io_error_wait(self):
        for error in (FakeUsbError("Operation timed out", -7, errno.ETIMEDOUT), FakeUsbError("Input/Output Error", -1, errno.EIO)):
            self.waited = []
            device = FakeDevice(0x0483, 0xA038, ctrl_error=error)
            self.assertEqual(self.switch(device, "cmsis-dap"), 0)
            self.assertEqual(device.writes, [])
            self.assertEqual(self.waited, [("cmsis-dap", "SN1")])

    def test_ep0_other_errors_fail(self):
        errors = (
            FakeUsbError("Access denied", -3, errno.EACCES),
            FakeUsbError("Entity not found", -5, errno.ENOENT),
            FakeUsbError("Resource busy", -6, errno.EBUSY),
            NotImplementedError("Operation not supported"),
        )
        for error in errors:
            device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", ctrl_error=error, reply=[0x80, 0x00])
            self.assertEqual(self.switch(device, "versaloon"), 1)
            self.assertEqual(device.writes, [])
            self.assertEqual(self.waited, [])

    def test_stall_in_wch_link_is_a_refusal(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP WCH-Link", ctrl_error=stall())
        self.assertEqual(self.switch(device, "versaloon"), 1)
        self.assertEqual(device.writes, [])
        self.assertEqual(self.waited, [])
        self.assertIn("does not have the versaloon personality", self.stderr)
        self.assertEqual(self.switch(device, "bootloader"), 1)
        self.assertIn("no bootloader entry", self.stderr)

    def test_bootloader_not_found_gives_the_udev_hint(self):
        self.wait_result = False
        self.assertEqual(self.switch(FakeDevice(0x0483, 0xA038), "bootloader"), 1)
        self.assertIn("70-openmupp-bootloader.rules", self.stderr)

    def test_fallback_versaloon(self):
        device = FakeDevice(0x0483, 0xA038, ctrl_error=stall(), reply=[0x00])
        self.assertEqual(self.switch(device, "cmsis-dap"), 0)
        self.assertEqual(device.writes, [(mp.OUT_ENDPOINT, bytes([0x0E, 0x01]))])
        self.assertEqual(self.waited, [("cmsis-dap", "SN1")])

    def test_fallback_cmsis_dap(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", ctrl_error=stall(), reply=[0x80, 0x00])
        self.assertEqual(self.switch(device, "versaloon"), 0)
        self.assertEqual(device.writes, [(mp.OUT_ENDPOINT, bytes([0x80, 0x00]))])

    def test_fallback_backend_error_code_only(self):
        device = FakeDevice(0x0483, 0xA038, ctrl_error=FakeUsbError("Pipe error", -9), reply=[0x00])
        self.assertEqual(self.switch(device, "cmsis-dap"), 0)
        self.assertEqual(len(device.writes), 1)

    def test_fallback_refused(self):
        device = FakeDevice(0x0483, 0xA038, ctrl_error=stall(), reply=[0x01])
        self.assertEqual(self.switch(device, "cmsis-dap"), 1)
        self.assertEqual(self.waited, [])

    def test_fallback_no_reply_waits(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", ctrl_error=stall(), reply=None)
        self.assertEqual(self.switch(device, "versaloon"), 0)
        self.assertEqual(self.waited, [("versaloon", "SN1")])

    def test_bootloader_ep0_request(self):
        for vid, pid, product in ((0x0483, 0xA038, ""), (0x1209, 0x0001, "OpenMUPP CMSIS-DAP"), (0x1209, 0x0001, "OpenMUPP WCH-Link")):
            self.waited = []
            device = FakeDevice(vid, pid, product)
            self.assertEqual(self.switch(device, "bootloader"), 0)
            self.assertEqual(device.ctrl_transfers, [(0x40, 0x0E, 0xB0, 0, None, mp.TIMEOUT_MS)])
            self.assertEqual(device.writes, [])
            self.assertEqual(self.waited, [("bootloader", "SN1")])

    def test_bootloader_fallback_versaloon_sends_fw_update(self):
        device = FakeDevice(0x0483, 0xA038, ctrl_error=stall(), reply=[0x00])
        self.assertEqual(self.switch(device, "bootloader"), 0)
        self.assertEqual(device.writes, [(mp.OUT_ENDPOINT, bytes([0x0F, 0xAA]))])
        self.assertEqual(self.waited, [("bootloader", "SN1")])

    def test_bootloader_fallback_cmsis_dap_refused(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", ctrl_error=stall(), reply=[0xFF])
        self.assertEqual(self.switch(device, "bootloader"), 1)
        self.assertEqual(device.writes, [(mp.OUT_ENDPOINT, bytes([0x80, 0xB0]))])
        self.assertEqual(self.waited, [])

    def test_no_switch_from_the_bootloader(self):
        device = FakeDevice(0x0483, 0x5740, "Bootloader")
        self.assertEqual(self.switch(device, "versaloon"), 1)
        self.assertEqual(device.ctrl_transfers, [])
        self.assertEqual(self.switch(device, "bootloader"), 0)

    def test_unreadable_device_is_reported(self):
        device = FakeDevice(0x1209, 0x0001, FakeUsbError("Access denied", -3, errno.EACCES))
        self.assertEqual(self.switch(device, "bootloader"), 1)
        self.assertIn("1209:0001 on bus 1 address 7", self.stderr)
        self.assertIn("70-openmupp", self.stderr)
        self.assertIn("No dongle found", self.stderr)

    def test_usb_id_switches_without_the_product_string(self):
        device = FakeDevice(0x1209, 0x0001, "Some test device")
        self.assertEqual(self.switch(device, "versaloon", usb_id=(0x1209, 0x0001)), 0)
        self.assertEqual(device.ctrl_transfers, [(0x40, 0x0E, 0, 0, None, mp.TIMEOUT_MS)])

    def test_usb_id_without_access_gives_the_udev_hint(self):
        denied = FakeUsbError("Access denied", -3, errno.EACCES)
        device = FakeDevice(0x1209, 0x0001, denied, serial=denied, ctrl_error=denied)
        self.assertEqual(self.switch(device, "versaloon", usb_id=(0x1209, 0x0001)), 1)
        self.assertIn("70-openmupp", self.stderr)

    def test_usb_id_and_serial_report_the_unreadable_serial(self):
        denied = FakeUsbError("Access denied", -3, errno.EACCES)
        device = FakeDevice(0x1209, 0x0001, denied, serial=denied)
        args = argparse.Namespace(serial="SN1", id=(0x1209, 0x0001), active=None)
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            self.assertEqual(mp.command_show(fake_usb([device]), args), 1)
        self.assertIn("1209:0001 on bus 1 address 7", stderr.getvalue())
        self.assertIn("No dongle found", stdout.getvalue())

    def test_no_enumeration_reports_the_unreadable_target(self):
        self.wait_result = False
        denied = FakeUsbError("Access denied", -3, errno.EACCES)
        target = FakeDevice(0x1209, 0x0001, denied)
        usb = fake_usb([FakeDevice(0x0483, 0xA038), target])
        args = argparse.Namespace(serial=None, personality="wch-link", id=None, active=None)
        stderr = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
            mp.command_switch(usb, args)
        self.assertIn("1209:0001 on bus 1 address 7", stderr.getvalue())

    def test_unknown_id_uses_the_ep0_request(self):
        device = FakeDevice(0x1234, 0x5678)
        self.assertEqual(self.switch(device, "cmsis-dap", usb_id=(0x1234, 0x5678)), 0)
        self.assertEqual(device.writes, [])

    def test_unknown_id_stall_needs_active(self):
        device = FakeDevice(0x1234, 0x5678, ctrl_error=stall(), reply=[0x00])
        self.assertEqual(self.switch(device, "cmsis-dap", usb_id=(0x1234, 0x5678)), 1)
        self.assertIn("--active", self.stderr)
        self.assertEqual(device.writes, [])

    def test_unknown_id_with_active(self):
        device = FakeDevice(0x1234, 0x5678, ctrl_error=stall(), reply=[0x00])
        self.assertEqual(self.switch(device, "cmsis-dap", usb_id=(0x1234, 0x5678), active="versaloon"), 0)
        self.assertEqual(device.writes, [(mp.OUT_ENDPOINT, bytes([0x0E, 0x01]))])

    def test_no_enumeration(self):
        self.wait_result = False
        self.assertEqual(self.switch(FakeDevice(0x0483, 0xA038), "cmsis-dap"), 1)

    def test_active_personality(self):
        device = FakeDevice(0x0483, 0xA038)
        self.assertEqual(self.switch(device, "versaloon"), 0)
        self.assertEqual(device.ctrl_transfers, [])


class WaitForResetTest(unittest.TestCase):
    def test_waits_for_a_new_address(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP")
        self.assertIsNone(mp.wait_for_reset(fake_usb([device]), "SN1", None, (1, 7), 0.0))
        device.address = 8
        self.assertEqual(mp.wait_for_reset(fake_usb([device]), "SN1", None, (1, 7), 1.0), "cmsis-dap")

    def test_checks_the_serial(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", serial="SN2")
        device.address = 8
        self.assertIsNone(mp.wait_for_reset(fake_usb([device]), "SN1", None, (1, 7), 0.0))


class CommandResetTest(unittest.TestCase):
    def setUp(self):
        self.waited = []
        self.wait_result = "cmsis-dap"
        original = mp.wait_for_reset

        def wait_for_reset(_usb, serial, usb_id, location, _timeout_s):
            self.waited.append((serial, usb_id, location))
            return self.wait_result

        mp.wait_for_reset = wait_for_reset
        self.addCleanup(setattr, mp, "wait_for_reset", original)

    def reset(self, devices, usb_id=None):
        args = argparse.Namespace(serial=None, id=usb_id, active=None)
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = mp.command_reset(fake_usb(devices), args)
        self.stdout, self.stderr = stdout.getvalue(), stderr.getvalue()
        return result

    def test_sends_the_reset_code_and_waits(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP")
        self.assertEqual(self.reset([device]), 0)
        self.assertEqual(device.ctrl_transfers, [(0x40, 0x0E, 0xB1, 0, None, mp.TIMEOUT_MS)])
        self.assertEqual(device.writes, [])
        self.assertEqual(self.waited, [("SN1", None, (1, 7))])
        self.assertIn("cmsis-dap personality", self.stdout)

    def test_works_in_each_personality(self):
        for vid, pid, product in ((0x0483, 0xA038, ""), (0x1209, 0x0001, "OpenMUPP WCH-Link")):
            device = FakeDevice(vid, pid, product)
            self.assertEqual(self.reset([device]), 0)
            self.assertEqual(device.ctrl_transfers, [(0x40, 0x0E, 0xB1, 0, None, mp.TIMEOUT_MS)])

    def test_stall_is_old_firmware(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", ctrl_error=stall())
        self.assertEqual(self.reset([device]), 1)
        self.assertIn("before the reset code", self.stderr)
        self.assertEqual(self.waited, [])

    def test_no_status_stage_waits(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", ctrl_error=FakeUsbError("No such device", -4, errno.ENODEV))
        self.assertEqual(self.reset([device]), 0)
        self.assertEqual(len(self.waited), 1)

    def test_other_error_fails(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", ctrl_error=FakeUsbError("Access denied", -3, errno.EACCES))
        self.assertEqual(self.reset([device]), 1)
        self.assertIn("udev", self.stderr)
        self.assertEqual(self.waited, [])

    def test_no_return_fails(self):
        self.wait_result = None
        self.assertEqual(self.reset([FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP")]), 1)
        self.assertIn("did not enumerate again", self.stderr)

    def test_needs_one_dongle(self):
        self.assertEqual(self.reset([]), 1)
        two = [FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", serial=s) for s in ("SN1", "SN2")]
        self.assertEqual(self.reset(two), 1)
        self.assertIn("--serial", self.stderr)
        self.assertEqual(two[0].ctrl_transfers + two[1].ctrl_transfers, [])

    def test_no_reset_of_the_bootloader(self):
        device = FakeDevice(0x0483, 0x5740, "Bootloader")
        self.assertEqual(self.reset([device]), 1)
        self.assertEqual(device.ctrl_transfers, [])

    def test_unknown_id_waits_with_the_id(self):
        device = FakeDevice(0x1234, 0x5678)
        self.assertEqual(self.reset([device], usb_id=(0x1234, 0x5678)), 0)
        self.assertEqual(self.waited, [("SN1", (0x1234, 0x5678), (1, 7))])


class BridgeRequestTest(unittest.TestCase):
    def test_modes(self):
        self.assertEqual(mp.bridge_request("on"), bytes([0x81, 0x01]))
        self.assertEqual(mp.bridge_request("off"), bytes([0x81, 0x00]))
        self.assertEqual(mp.bridge_request("auto"), bytes([0x81, 0x02]))
        self.assertEqual(mp.bridge_request("stored"), bytes([0x81, 0x03]))

    def test_store(self):
        self.assertEqual(mp.bridge_request("off", store=True), bytes([0x81, 0x80]))
        self.assertEqual(mp.bridge_request("on", store=True), bytes([0x81, 0x81]))
        self.assertEqual(mp.bridge_request("auto", store=True), bytes([0x81, 0x82]))


class BridgeCheckReplyTest(unittest.TestCase):
    def test_done(self):
        mp.check_bridge_reply([0x81, 0x00])

    def test_no_bridge(self):
        with self.assertRaisesRegex(mp.ProbeError, "no RISC-V bridge"):
            mp.check_bridge_reply([0xFF])

    def test_refused(self):
        with self.assertRaisesRegex(mp.ProbeError, "status 255"):
            mp.check_bridge_reply([0x81, 0xFF])

    def test_other_response(self):
        for reply in ([], [0x80, 0x00], [0x81]):
            with self.assertRaises(mp.ProbeError):
                mp.check_bridge_reply(reply)


class BridgeCommandTest(unittest.TestCase):
    def run_main(self, argv, devices):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = mp.main(["bridge", *argv], fake_usb(devices))
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
        self.assertIn("No response", err)
        self.assertIn("not known", err)

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
        self.assertEqual(self.run_main(["on", "--serial", "B"], [a, b])[0], 0)
        self.assertEqual(a.writes, [])
        self.assertEqual(len(b.writes), 1)


    def test_serial_before_the_command(self):
        a = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", serial="A", reply=[0x81, 0x00])
        b = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", serial="B", reply=[0x81, 0x00])
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            self.assertEqual(mp.main(["--serial", "B", "bridge", "on"], fake_usb([a, b])), 0)
        self.assertEqual(a.writes, [])

    def test_id_needs_active_for_a_shared_id(self):
        device = FakeDevice(0x1209, 0x0001, "Some test device", reply=[0x81, 0x00])
        result, _, err = self.run_main(["on", "--id", "1209:0001"], [device])
        self.assertEqual(result, 1)
        self.assertIn("select it with --active", err)
        self.assertEqual(device.writes, [])
        self.assertEqual(self.run_main(["on", "--id", "1209:0001", "--active", "cmsis-dap"], [device])[0], 0)
        self.assertEqual(device.writes, [(0x03, bytes([0x81, 0x01]))])


class PowerCheckReplyTest(unittest.TestCase):
    def test_done(self):
        mp.check_power_reply([0x82, 0x00, 0xE4, 0x0C])

    def test_old_firmware(self):
        with self.assertRaisesRegex(mp.ProbeError, "v0.13.0"):
            mp.check_power_reply([0xFF])

    def test_refused_shows_the_voltage(self):
        with self.assertRaisesRegex(mp.ProbeError, "refused the switch-on.* 3300 mV"):
            mp.check_power_reply([0x82, 0x01, 0xE4, 0x0C])

    def test_no_switch(self):
        with self.assertRaisesRegex(mp.ProbeError, "no power switch"):
            mp.check_power_reply([0x82, 0xFF, 0x00, 0x00])

    def test_other_response(self):
        for reply in ([], [0x81, 0x00, 0x00, 0x00], [0x82, 0x00], [0x82, 0x02, 0x00, 0x00]):
            with self.assertRaises(mp.ProbeError):
                mp.check_power_reply(reply)


class VppCheckReplyTest(unittest.TestCase):
    def test_done(self):
        mp.check_vpp_reply([0x85, 0x00, 0xE0, 0x2E, 0xE5, 0x2E, 0xC8, 0x2E])

    def test_old_firmware(self):
        with self.assertRaisesRegex(mp.ProbeError, "ID_DAP_Vendor5"):
            mp.check_vpp_reply([0xFF])

    def test_errors_show_the_senses(self):
        for status, text in ((1, "refused"), (2, "did not show the level"), (3, "short")):
            with self.assertRaisesRegex(mp.ProbeError, text + ".*VPP_SENSE 4600 mV, VPP_SENSE2 0 mV"):
                mp.check_vpp_reply([0x85, status, 0x00, 0x00, 0xF8, 0x11, 0x00, 0x00])

    def test_other_response(self):
        for reply in ([], [0x84] + [0] * 7, [0x85, 0x00, 0x00], [0x85, 0x04] + [0] * 6):
            with self.assertRaises(mp.ProbeError):
                mp.check_vpp_reply(reply)


class VppCommandTest(unittest.TestCase):
    def run_main(self, argv, devices):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = mp.main(["vpp", *argv], fake_usb(devices))
        return result, stdout.getvalue(), stderr.getvalue()

    def test_on(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0x85, 0x00, 0xE0, 0x2E, 0xE5, 0x2E, 0xC8, 0x2E])
        result, out, _ = self.run_main(["on", "12000"], [device])
        self.assertEqual(result, 0)
        self.assertEqual(device.writes, [(0x03, bytes([0x85, 0x01, 0xE0, 0x2E]))])
        # The switch-on can take 2 s
        self.assertEqual(device.read_timeouts, [mp.VPP_TIMEOUT_MS])
        self.assertIn("VPP: on at 12000 mV", out)
        self.assertIn("VPP_SENSE:  12005 mV", out)
        self.assertIn("VPP_SENSE2: 11976 mV", out)

    def test_off_and_status(self):
        for operation, code in (("off", 0), ("status", 2)):
            device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0x85, 0x00, 0, 0, 0xF8, 0x11, 0, 0])
            result, out, _ = self.run_main([operation], [device])
            self.assertEqual(result, 0)
            self.assertEqual(device.writes, [(0x03, bytes([0x85, code]))])
            self.assertIn("VPP: off", out)
            self.assertIn("VPP_SENSE:  4600 mV", out)

    def test_refused(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0x85, 0x01, 0, 0, 0, 0, 0, 0])
        result, out, err = self.run_main(["on", "14000"], [device])
        self.assertEqual(result, 1)
        self.assertEqual(out, "")
        self.assertIn("refused VPP", err)

    def test_level_only_with_on(self):
        for argv in (["on"], ["off", "5000"], ["status", "5000"], ["on", "70000"]):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                mp.main(["vpp", *argv], fake_usb([]))


class PowerCommandTest(unittest.TestCase):
    def run_main(self, argv, devices):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = mp.main(["power", *argv], fake_usb(devices))
        return result, stdout.getvalue(), stderr.getvalue()

    def test_operations(self):
        for operation, code in (("off", 0), ("on", 1), ("status", 2)):
            device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0x82, 0x00, 0xE4, 0x0C])
            result, out, _ = self.run_main([operation], [device])
            self.assertEqual(result, 0)
            self.assertEqual(device.writes, [(0x03, bytes([0x82, code]))])
            self.assertIn("Target voltage: 3300 mV", out)

    def test_status_does_not_claim_a_switch(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0x82, 0x00, 0x00, 0x00])
        self.assertNotIn("target power is", self.run_main(["status"], [device])[1])

    def test_refused(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0x82, 0x01, 0xE4, 0x0C])
        result, out, err = self.run_main(["on"], [device])
        self.assertEqual(result, 1)
        self.assertEqual(out, "")
        self.assertIn("own supply", err)

    def test_interface_is_released_after_a_command(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0x82, 0x00, 0xE4, 0x0C])
        self.run_main(["on"], [device])
        self.assertEqual(device.interface_calls, ["claim", "release", "dispose"])

    def test_interface_is_released_without_a_reply(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=None)
        self.run_main(["on"], [device])
        self.assertEqual(device.interface_calls, ["claim", "release", "dispose"])

    def test_failed_claim_gives_the_udev_hint(self):
        denied = FakeUsbError("Access denied", -3, errno.EACCES)
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", claim_error=denied)
        result, _, err = self.run_main(["on"], [device])
        self.assertEqual(result, 1)
        self.assertEqual(device.writes, [])
        self.assertEqual(device.interface_calls, ["claim", "release", "dispose"])
        self.assertIn("Cannot send", err)
        self.assertIn("70-openmupp", err)

    def test_only_cmsis_dap(self):
        versaloon = FakeDevice(0x0483, 0xA038)
        self.assertEqual(self.run_main(["on"], [versaloon])[0], 1)
        self.assertEqual(versaloon.writes, [])

    def test_no_response(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=None)
        result, _, err = self.run_main(["off"], [device])
        self.assertEqual(result, 1)
        self.assertIn("No response", err)
        self.assertIn("not known", err)


class UartCheckReplyTest(unittest.TestCase):
    def test_done(self):
        mp.check_uart_reply([0x86, 0x00, 0x01])

    def test_no_command(self):
        with self.assertRaisesRegex(mp.ProbeError, "UART_TX_DIR"):
            mp.check_uart_reply([0xFF])

    def test_other_response(self):
        for reply in ([], [0x84, 0x00, 0x00], [0x86, 0x00], [0x86, 0xFF, 0x00], [0x86, 0x00, 0x02]):
            with self.assertRaises(mp.ProbeError):
                mp.check_uart_reply(reply)


class UartCommandTest(unittest.TestCase):
    def run_main(self, argv, devices):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = mp.main(["uart", *argv], fake_usb(devices))
        return result, stdout.getvalue(), stderr.getvalue()

    def test_modes(self):
        for mode, code, shown in (("normal", 0, "normal (two wires)"), ("single", 1, "single wire"), ("status", 2, "single wire")):
            reply = [0x86, 0x00, 0 if mode == "normal" else 1]
            device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=reply)
            result, out, _ = self.run_main([mode], [device])
            self.assertEqual(result, 0)
            self.assertEqual(device.writes, [(0x03, bytes([0x86, code]))])
            self.assertEqual(out, "UART mode: %s\n" % shown)

    def test_board_without_the_command(self):
        device = FakeDevice(0x1209, 0x0001, "OpenMUPP CMSIS-DAP", reply=[0xFF])
        result, out, err = self.run_main(["single"], [device])
        self.assertEqual(result, 1)
        self.assertEqual(out, "")
        self.assertIn("ID_DAP_Vendor6", err)


class IdReportLinesTest(unittest.TestCase):
    def test_known_board_and_adapter(self):
        lines = mp.id_report_lines(bytes([0x84, 0, 1, 1, 1, 5, 1]))
        self.assertEqual(lines, ["Board:    OpenMUPP v1, revision 1", "Adapter:  slot 5, pin-header adapter", "Connect:  allowed"])

    def test_no_adapter(self):
        lines = mp.id_report_lines(bytes([0x84, 0, 1, 1, 0, 0, 1]))
        self.assertEqual(lines[1], "Adapter:  no adapter")

    def test_free_slot(self):
        lines = mp.id_report_lines(bytes([0x84, 0, 1, 1, 3, 6, 1]))
        self.assertEqual(lines[1], "Adapter:  slot 6, unknown (free slot)")

    def test_adapter_fault_refuses(self):
        lines = mp.id_report_lines(bytes([0x84, 0, 1, 1, 2, 0, 0]))
        self.assertEqual(lines[1:], ["Adapter:  fault: pin shorted to GND", "Connect:  refused: adapter fault"])

    def test_board_revision_2_has_the_tx_pull_up(self):
        lines = mp.id_report_lines(bytes([0x84, 0, 1, 2, 1, 5, 1]))
        self.assertEqual(lines, ["Board:    OpenMUPP v1, revision 2 (pull-up on UART TX)", "Adapter:  slot 5, pin-header adapter", "Connect:  allowed"])

    def test_unknown_board_revision_refuses(self):
        lines = mp.id_report_lines(bytes([0x84, 0, 1, 3, 1, 4, 0]))
        self.assertEqual(lines[0], "Board:    unknown revision (slot 3, unknown)")
        self.assertEqual(lines[2], "Connect:  refused: unknown board revision")

    def test_adapter_fault_on_an_unknown_board_gives_the_adapter_fault(self):
        lines = mp.id_report_lines(bytes([0x84, 0, 1, 3, 2, 0, 0]))
        self.assertEqual(lines[2], "Connect:  refused: adapter fault")

    def test_board_fault_names_the_adc(self):
        lines = mp.id_report_lines(bytes([0x84, 0, 2, 0, 1, 5, 0]))
        self.assertEqual(lines[0], "Board:    unknown revision (fault: pin shorted to GND, or the ADC failed)")

    def test_failed_adc(self):
        lines = mp.id_report_lines(bytes([0x84, 0xFF, 1, 1, 0, 0, 0]))
        self.assertEqual(lines[1:], ["Adapter:  the ADC failed", "Connect:  refused: the ADC failed"])

    def test_no_id_report(self):
        lines = mp.id_report_lines(bytes([0xFF]))
        self.assertEqual(len(lines), 1)
        self.assertIn("no board ID", lines[0])


class CommandAdapterTest(unittest.TestCase):
    @staticmethod
    def replies(request):
        if request == bytes([0x00, 0x09]):
            return bytes([0x00, 8]) + b"v0.13.0\0"
        if request == bytes([0x84]):
            return bytes([0x84, 0, 1, 1, 1, 4, 1])
        return bytes([0xFF])

    def run_adapter(self, device):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = mp.main(["adapter"], usb=fake_usb([device]))
        return code, out.getvalue()

    def test_prints_version_board_and_adapter(self):
        device = FakeDevice(0x1209, 0x0001, product="OpenMUPP CMSIS-DAP", reply=self.replies)
        code, out = self.run_adapter(device)
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines(), ["Firmware: v0.13.0", "Board:    OpenMUPP v1, revision 1", "Adapter:  slot 4, Arm adapter", "Connect:  allowed"])
        self.assertEqual([data for _, data in device.writes], [bytes([0x00, 0x09]), bytes([0x84])])

    def test_old_firmware_prints_the_version(self):
        def replies(request):
            return bytes([0xFF]) if request == bytes([0x84]) else self.replies(request)

        device = FakeDevice(0x1209, 0x0001, product="OpenMUPP CMSIS-DAP", reply=replies)
        code, out = self.run_adapter(device)
        self.assertEqual(code, 0)
        lines = out.splitlines()
        self.assertEqual(lines[0], "Firmware: v0.13.0")
        self.assertIn("no board ID", lines[1])

    def test_short_id_reply_fails(self):
        def replies(request):
            return bytes([0x84, 0]) if request == bytes([0x84]) else self.replies(request)

        device = FakeDevice(0x1209, 0x0001, product="OpenMUPP CMSIS-DAP", reply=replies)
        code, _ = self.run_adapter(device)
        self.assertEqual(code, 1)

    def test_no_reply_fails(self):
        device = FakeDevice(0x1209, 0x0001, product="OpenMUPP CMSIS-DAP")
        code, _ = self.run_adapter(device)
        self.assertEqual(code, 1)

    def test_bad_info_reply_fails(self):
        device = FakeDevice(0x1209, 0x0001, product="OpenMUPP CMSIS-DAP", reply=bytes([0x84, 0]))
        code, _ = self.run_adapter(device)
        self.assertEqual(code, 1)

if __name__ == "__main__":
    unittest.main()
