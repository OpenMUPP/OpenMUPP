"""Tests for the DTM scan decoder of tools/usb-pcap/dtm.py."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'usb-pcap'))
import dtm

CAPTURES = os.path.join(os.path.dirname(__file__), '..', 'docs', 'cmsis-dap-captures')


class ClocksTest(unittest.TestCase):
    def test_tdo_comes_from_the_reply_of_captured_entries_only(self):
        # Entry 1: 4 bits, TMS 0, capture, TDI 0b0011. Entry 2: 2 bits, TMS 1, no capture.
        request = bytes([0x14, 2, 0x84, 0x03, 0x42, 0x00])
        reply = bytes([0x14, 0x00, 0x05])
        self.assertEqual(dtm.clocks(request, reply),
                         [(0, 1, 1), (0, 1, 0), (0, 0, 1), (0, 0, 0), (1, 0, None), (1, 0, None)])

    def test_swj_sequence_drives_tdi_high(self):
        self.assertEqual(dtm.clocks(bytes([0x12, 3, 0x05]), bytes([0x12, 0x00])),
                         [(1, 1, None), (0, 1, None), (1, 1, None)])


class CaptureTest(unittest.TestCase):
    def test_openocd_finds_the_dtm_and_reads_dmcontrol(self):
        lines = list(dtm.scans(os.path.join(CAPTURES, 'openocd-ch32v208.pcapng'), 41, 3, 0x82))
        self.assertTrue(lines[0].endswith('15653101'))
        self.assertIn('DR ir=0x10 32b in=0x0 out=0x71', lines)
        dmi = [line for line in lines if line.startswith('DMI')]
        self.assertTrue(dmi[0].startswith('DMI rd a=0x10'))
        self.assertFalse([line for line in dmi if line.endswith('op=2')])

    def test_attach_at_boot_fails_the_first_access(self):
        lines = list(dtm.scans(os.path.join(CAPTURES, 'openocd-ch32v208-attach-at-boot.pcapng'), 38, 3, 0x82))
        dmi = [line for line in lines if line.startswith('DMI')]
        self.assertTrue(dmi[1].endswith('a=0x10 d=00000000 op=2'))


if __name__ == '__main__':
    unittest.main()
