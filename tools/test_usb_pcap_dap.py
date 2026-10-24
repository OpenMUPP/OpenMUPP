"""Tests for the TAP state machine of tools/usb-pcap/dap.py."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'usb-pcap'))
import dap


def run(*cmds):
    tap, evs = dap.Tap(), []
    for c in cmds:
        evs += dap.tap_events(tap, c)
    return evs


def scans(evs):
    return [e for e in evs if e.startswith(('IR', 'DR'))]


def seq(*entries):
    """DAP_JTAG_Sequence from (tms, tdi bit string) entries; the first bit is the LSB."""
    out = bytes([0x14, len(entries)])
    for tms, tdi in entries:
        out += bytes([len(tdi) | tms << 6]) + int(tdi[::-1], 2).to_bytes((len(tdi) + 7) // 8, 'little')
    return out


class TapTest(unittest.TestCase):
    def test_ir_then_dr_through_update_ir(self):
        # RTI -> SelDR -> SelIR -> CapIR -> ShIR (5b, last with TMS=1) -> UpIR -> SelDR -> CapDR -> ShDR
        ir = seq((0, '0'), (1, '0'), (1, '0'), (0, '0'), (0, '0'), (0, '0000'), (1, '1'))
        dr = seq((1, '0'), (1, '0'), (0, '0'), (0, '0'), (0, '1010'), (1, '1'))  # Exit1-IR -> Update-IR -> SelDR -> CapDR -> ShDR
        evs = run(seq((1, '11111'), (0, '0')), ir, dr)
        self.assertEqual(scans(evs), ['IR scan 5b tdi=0x10', 'DR scan 5b tdi=0x15'])

    def test_scan_split_over_two_commands(self):
        evs = run(seq((1, '11111'), (0, '0'), (1, '0'), (0, '0'), (0, '0'), (0, '1111')),
                  seq((0, '11'), (1, '1')))
        self.assertEqual(scans(evs), ['DR scan 7b tdi=0x7f'])

    def test_exit_with_multi_bit_tms_entry(self):
        evs = run(seq((1, '11111'), (0, '0'), (1, '0'), (0, '0'), (0, '0'), (0, '000')),
                  seq((1, '11111')))
        self.assertEqual(scans(evs), ['DR scan 4b tdi=0x8'])

    def test_swj_sequence_moves_tap(self):
        tap = dap.Tap()
        dap.tap_events(tap, bytes([0x12, 8, 0xff]))
        dap.tap_events(tap, bytes([0x12, 5, 0b00110]))  # TMS 0,1,1,0,0: RTI, SelDR, SelIR, CapIR, ShIR
        self.assertEqual(tap.state, 'SHIR')

    def test_captures_decode(self):
        d = os.path.join(os.path.dirname(__file__), '..', 'docs', 'cmsis-dap-captures')
        from pcap import usb
        tap, evs = dap.Tap(), []
        for p in usb(os.path.join(d, 'openocd-jtag-no-target.pcapng')):
            if p['dev'] == 35 and p['ep'] == 3 and p['type'] == 'S' and p['data']:
                evs += dap.tap_events(tap, p['data'])
        self.assertIn('DR scan 672b tdi=0x' + 'f' * 168, evs)
        self.assertEqual([e for e in evs if e.startswith('IR')],
                         ['IR scan 7b tdi=0x7f', 'IR scan 5b tdi=0x10', 'IR scan 5b tdi=0x11'])


if __name__ == '__main__':
    unittest.main()
