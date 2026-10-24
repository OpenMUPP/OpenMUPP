"""Host tests for vsl_personality.py. They need no pyusb and no hardware.

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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vsl_personality as vp  # noqa: E402


class PersonalityOfTest(unittest.TestCase):
    def test_known_ids(self):
        self.assertEqual(vp.personality_of(0x0483, 0xA038), "versaloon")
        self.assertEqual(vp.personality_of(0x0403, 0x9B3C), "cmsis-dap")
        self.assertEqual(vp.personality_of(0x1209, 0x0001), "wch-link")
        self.assertEqual(vp.personality_of(0x1A86, 0x8010), "wch-link")

    def test_bootloader_id(self):
        self.assertEqual(vp.personality_of(0x0483, 0x5740), "bootloader")

    def test_unknown_id(self):
        self.assertIsNone(vp.personality_of(0x0483, 0x5741))


class SwitchRequestTest(unittest.TestCase):
    def test_versaloon_sends_switch_personality(self):
        self.assertEqual(vp.switch_request("versaloon", "cmsis-dap"), bytes([0x0E, 0x01]))

    def test_cmsis_dap_sends_vendor0(self):
        self.assertEqual(vp.switch_request("cmsis-dap", "versaloon"), bytes([0x80, 0x00]))

    def test_code_of_wch_link(self):
        self.assertEqual(vp.switch_request("versaloon", "wch-link"), bytes([0x0E, 0x02]))
        self.assertEqual(vp.switch_request("cmsis-dap", "wch-link"), bytes([0x80, 0x02]))

    def test_bootloader_code(self):
        self.assertEqual(vp.switch_request("cmsis-dap", "bootloader"), bytes([0x80, 0xB0]))

    def test_versaloon_sends_fw_update_for_the_bootloader(self):
        self.assertEqual(vp.switch_request("versaloon", "bootloader"), bytes([0x0F, 0xAA]))

    def test_wch_link_has_no_command(self):
        with self.assertRaises(vp.PersonalityError):
            vp.switch_request("wch-link", "versaloon")


class CheckReplyTest(unittest.TestCase):
    def test_versaloon_accepted(self):
        vp.check_reply("versaloon", [0x00])

    def test_versaloon_refused(self):
        with self.assertRaisesRegex(vp.PersonalityError, "one personality"):
            vp.check_reply("versaloon", [0x01])

    def test_versaloon_wrong_size(self):
        with self.assertRaises(vp.PersonalityError):
            vp.check_reply("versaloon", [])

    def test_bootloader_refused(self):
        with self.assertRaisesRegex(vp.PersonalityError, "no bootloader entry"):
            vp.check_reply("versaloon", [0x01], "bootloader")
        with self.assertRaisesRegex(vp.PersonalityError, "no bootloader entry"):
            vp.check_reply("cmsis-dap", [0xFF], "bootloader")

    def test_cmsis_dap_accepted(self):
        vp.check_reply("cmsis-dap", [0x80, 0x00])

    def test_cmsis_dap_refused(self):
        with self.assertRaisesRegex(vp.PersonalityError, "refused"):
            vp.check_reply("cmsis-dap", [0x80, 0xFF])

    def test_cmsis_dap_invalid(self):
        with self.assertRaisesRegex(vp.PersonalityError, "one personality"):
            vp.check_reply("cmsis-dap", [0xFF])

    def test_cmsis_dap_other_response(self):
        with self.assertRaises(vp.PersonalityError):
            vp.check_reply("cmsis-dap", [0x00, 0x00])


class FakeUsbError(Exception):
    def __init__(self, strerror, error_code=None, errno_=None):
        super().__init__(strerror)
        self.backend_error_code = error_code
        self.errno = errno_


def stall():
    return FakeUsbError("Pipe error", -9, errno.EPIPE)


class FakeDevice:
    """A dongle. ctrl_error and reply select how it answers."""

    def __init__(self, vid, pid, product="", serial="SN1", ctrl_error=None, reply=None):
        self.idVendor = vid
        self.idProduct = pid
        self.iProduct = 2
        self.iSerialNumber = 3
        self.strings = {2: product, 3: serial}
        self.ctrl_error = ctrl_error
        self.reply = reply
        self.ctrl_transfers = []
        self.writes = []

    def ctrl_transfer(self, *args):
        self.ctrl_transfers.append(args)
        if self.ctrl_error is not None:
            raise self.ctrl_error

    def is_kernel_driver_active(self, _interface):
        return False

    def write(self, endpoint, data, _timeout):
        self.writes.append((endpoint, bytes(data)))

    def read(self, _endpoint, _size, _timeout):
        if self.reply is None:
            raise FakeUsbError("Operation timed out", -7, errno.ETIMEDOUT)
        return self.reply


def get_string(device, index):
    value = device.strings[index]
    if isinstance(value, Exception):
        raise value
    return value


def fake_usb(devices):
    """A module like pyusb that finds the given devices."""
    usb = types.SimpleNamespace()

    def find(find_all, idVendor, idProduct):
        return [d for d in devices if (d.idVendor, d.idProduct) == (idVendor, idProduct)]

    usb.core = types.SimpleNamespace(USBError=FakeUsbError, find=find)
    usb.util = types.SimpleNamespace(
        get_string=get_string,
        claim_interface=lambda device, interface: None,
        release_interface=lambda device, interface: None,
        dispose_resources=lambda device: None,
    )
    return usb


class FindDevicesTest(unittest.TestCase):
    def test_wch_link_needs_the_product_string(self):
        ours = FakeDevice(0x1209, 0x0001, "Versaloon WCH-Link")
        other = FakeDevice(0x1209, 0x0001, "Some test device")
        real = FakeDevice(0x1A86, 0x8010, "WCH-Link")
        ours_wch_id = FakeDevice(0x1A86, 0x8010, "Versaloon WCH-Link")
        found = vp.find_devices(fake_usb([ours, other, real, ours_wch_id]))
        self.assertEqual(found, [("wch-link", ours), ("wch-link", ours_wch_id)])

    def test_unreadable_product_string(self):
        for error in (FakeUsbError("Access denied", -3, errno.EACCES), NotImplementedError("not supported"), ValueError()):
            device = FakeDevice(0x1209, 0x0001, error)
            self.assertEqual(vp.find_devices(fake_usb([device])), [])

    def test_unreadable_serial(self):
        device = FakeDevice(0x0483, 0xA038, serial=NotImplementedError("not supported"))
        self.assertEqual(vp.find_devices(fake_usb([device])), [("versaloon", device)])
        self.assertEqual(vp.find_devices(fake_usb([device]), "A"), [])

    def test_bootloader_needs_the_product_string(self):
        ours = FakeDevice(0x0483, 0x5740, "Bootloader")
        other = FakeDevice(0x0483, 0x5740, "STM32 Virtual ComPort")
        self.assertEqual(vp.find_devices(fake_usb([ours, other])), [("bootloader", ours)])

    def test_serial(self):
        a = FakeDevice(0x0483, 0xA038, serial="A")
        b = FakeDevice(0x0403, 0x9B3C, serial="B")
        self.assertEqual(vp.find_devices(fake_usb([a, b]), "B"), [("cmsis-dap", b)])


class CommandSwitchTest(unittest.TestCase):
    def setUp(self):
        self.waited = []
        self.wait_result = True
        original = vp.wait_for

        def wait_for(_usb, target, serial, _timeout_s):
            self.waited.append((target, serial))
            return self.wait_result

        vp.wait_for = wait_for
        self.addCleanup(setattr, vp, "wait_for", original)

    def switch(self, device, target):
        args = argparse.Namespace(serial=None, personality=target)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return vp.command_switch(fake_usb([device]), args)

    def test_ep0_request(self):
        device = FakeDevice(0x0483, 0xA038)
        self.assertEqual(self.switch(device, "wch-link"), 0)
        self.assertEqual(device.ctrl_transfers, [(0x40, 0x0E, 2, 0, None, vp.TIMEOUT_MS)])
        self.assertEqual(device.writes, [])
        self.assertEqual(self.waited, [("wch-link", "SN1")])

    def test_ep0_request_from_wch_link(self):
        device = FakeDevice(0x1209, 0x0001, "Versaloon WCH-Link")
        self.assertEqual(self.switch(device, "cmsis-dap"), 0)
        self.assertEqual(device.ctrl_transfers, [(0x40, 0x0E, 1, 0, None, vp.TIMEOUT_MS)])
        self.assertEqual(self.waited, [("cmsis-dap", "SN1")])

    def test_ep0_no_status_stage_waits(self):
        device = FakeDevice(0x0403, 0x9B3C, ctrl_error=FakeUsbError("No such device", -4, errno.ENODEV))
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
            device = FakeDevice(0x0403, 0x9B3C, ctrl_error=error, reply=[0x80, 0x00])
            self.assertEqual(self.switch(device, "versaloon"), 1)
            self.assertEqual(device.writes, [])
            self.assertEqual(self.waited, [])

    def test_stall_in_wch_link_is_a_refusal(self):
        device = FakeDevice(0x1209, 0x0001, "Versaloon WCH-Link", ctrl_error=stall())
        self.assertEqual(self.switch(device, "versaloon"), 1)
        self.assertEqual(device.writes, [])
        self.assertEqual(self.waited, [])

    def test_fallback_versaloon(self):
        device = FakeDevice(0x0483, 0xA038, ctrl_error=stall(), reply=[0x00])
        self.assertEqual(self.switch(device, "cmsis-dap"), 0)
        self.assertEqual(device.writes, [(vp.OUT_ENDPOINT, bytes([0x0E, 0x01]))])
        self.assertEqual(self.waited, [("cmsis-dap", "SN1")])

    def test_fallback_cmsis_dap(self):
        device = FakeDevice(0x0403, 0x9B3C, ctrl_error=stall(), reply=[0x80, 0x00])
        self.assertEqual(self.switch(device, "versaloon"), 0)
        self.assertEqual(device.writes, [(vp.OUT_ENDPOINT, bytes([0x80, 0x00]))])

    def test_fallback_backend_error_code_only(self):
        device = FakeDevice(0x0483, 0xA038, ctrl_error=FakeUsbError("Pipe error", -9), reply=[0x00])
        self.assertEqual(self.switch(device, "cmsis-dap"), 0)
        self.assertEqual(len(device.writes), 1)

    def test_fallback_refused(self):
        device = FakeDevice(0x0483, 0xA038, ctrl_error=stall(), reply=[0x01])
        self.assertEqual(self.switch(device, "cmsis-dap"), 1)
        self.assertEqual(self.waited, [])

    def test_fallback_no_reply_waits(self):
        device = FakeDevice(0x0403, 0x9B3C, ctrl_error=stall(), reply=None)
        self.assertEqual(self.switch(device, "versaloon"), 0)
        self.assertEqual(self.waited, [("versaloon", "SN1")])

    def test_bootloader_ep0_request(self):
        for vid, pid, product in ((0x0483, 0xA038, ""), (0x0403, 0x9B3C, ""), (0x1209, 0x0001, "Versaloon WCH-Link")):
            self.waited = []
            device = FakeDevice(vid, pid, product)
            self.assertEqual(self.switch(device, "bootloader"), 0)
            self.assertEqual(device.ctrl_transfers, [(0x40, 0x0E, 0xB0, 0, None, vp.TIMEOUT_MS)])
            self.assertEqual(device.writes, [])
            self.assertEqual(self.waited, [("bootloader", "SN1")])

    def test_bootloader_fallback_versaloon_sends_fw_update(self):
        device = FakeDevice(0x0483, 0xA038, ctrl_error=stall(), reply=[0x00])
        self.assertEqual(self.switch(device, "bootloader"), 0)
        self.assertEqual(device.writes, [(vp.OUT_ENDPOINT, bytes([0x0F, 0xAA]))])
        self.assertEqual(self.waited, [("bootloader", "SN1")])

    def test_bootloader_fallback_cmsis_dap_refused(self):
        device = FakeDevice(0x0403, 0x9B3C, ctrl_error=stall(), reply=[0xFF])
        self.assertEqual(self.switch(device, "bootloader"), 1)
        self.assertEqual(device.writes, [(vp.OUT_ENDPOINT, bytes([0x80, 0xB0]))])
        self.assertEqual(self.waited, [])

    def test_no_switch_from_the_bootloader(self):
        device = FakeDevice(0x0483, 0x5740, "Bootloader")
        self.assertEqual(self.switch(device, "versaloon"), 1)
        self.assertEqual(device.ctrl_transfers, [])
        self.assertEqual(self.switch(device, "bootloader"), 0)

    def test_no_enumeration(self):
        self.wait_result = False
        self.assertEqual(self.switch(FakeDevice(0x0483, 0xA038), "cmsis-dap"), 1)

    def test_active_personality(self):
        device = FakeDevice(0x0483, 0xA038)
        self.assertEqual(self.switch(device, "versaloon"), 0)
        self.assertEqual(device.ctrl_transfers, [])


if __name__ == "__main__":
    unittest.main()
