"""Tests for the WCH-LinkE wire frame decoders. They use the captures in docs/wch-link-captures."""
import collections
import os
import unittest

import wch_link_frames_1wire as one
import wch_link_frames_2wire as two

CAPTURES = os.path.join(os.path.dirname(__file__), '..', 'docs', 'wch-link-captures')


def capture(name):
    return os.path.join(CAPTURES, name)


def two_wire(name, start_ms=0.0):
    skipped = []
    got = [(t, two.decode(f)) for t, f in two.frames(capture(name), start_ms, skipped)]
    return got, skipped


class TwoWire(unittest.TestCase):
    def test_host_phase_of_status_run(self):
        # The first accesses of the host: abstractcs write, data0 write, command write, abstractcs read
        got, _ = two_wire('v208-wlink-status-timing.csv', 60.1)
        self.assertEqual([(d['addr'], d['op'], d['data']) for _, d in got[:4]],
                         [(0x16, '10', 0x700), (0x04, '10', 0), (0x17, '11', 0x220301),
                          (0x16, '01', 0x08000002)])

    def test_every_frame_has_right_parity_and_none_is_lost(self):
        got, skipped = two_wire('v208-wlink-status-timing.csv')
        self.assertEqual(len(got), 147)
        self.assertTrue(all(d['parity_ok'] for _, d in got))
        # Only the 85-clock frames of the connect phase and a few short bursts are skipped.
        # A 53- or 54-clock part would be a real frame that the decoder lost.
        self.assertEqual(sorted(collections.Counter(c for _, c in skipped).items()),
                         [(2, 2), (3, 1), (85, 201), (99, 1)])

    def test_same_frames_at_all_three_speeds(self):
        # A frame with a stray clock before or after it must still decode
        for name in ('v208-wlink-status-timing.csv', 'v208-wlink-status-medium-timing.csv',
                     'v208-wlink-status-low-timing.csv'):
            got, skipped = two_wire(name)
            self.assertEqual(len(got), 147, name)
            self.assertTrue(all(d['parity_ok'] for _, d in got), name)
            self.assertFalse([c for _, c in skipped if c > 53 and c != 85 and c != 99], name)

    def test_frames_with_a_pause_after_the_start_clock_are_joined(self):
        got, _ = two_wire('v208-wlink-status-timing.csv', 45)
        # 46.046 ms: the start clock is the last clock of a longer burst
        first = got[0][1]
        self.assertEqual((first['addr'], first['data']), (0x7e, 0x5aa50400))

    def test_memory_reads_for_the_chip_information(self):
        # data1 holds the address, an access-memory command reads, data0 gives the value
        got, _ = two_wire('v208-wlink-status-timing.csv', 59.6)
        reads, address, waiting = [], None, False
        for _, d in got:
            if d['addr'] == 0x05 and d['op'] == '11':
                address = d['data']
            elif d['addr'] == 0x17 and d['data'] >> 24 == 2 and not d['data'] & 0x10000:  # memory read
                waiting = True
            elif d['addr'] == 0x04 and d['op'] == '01' and waiting:
                reads.append((address, d['data']))
                waiting = False
        self.assertEqual(reads[:7], [(0x1ffff7e0, 0xffff00a0), (0x1ffff7e8, 0x6213f0eb),
                                     (0x1ffff7ec, 0x2f30dc32), (0x1ffff7f0, 0xe339e339),
                                     (0x4002201c, 0x000000fc), (0x40022020, 0xffffffff),
                                     (0x1ffff802, 0x0000003f)])

    def test_no_swclk_edges_stops_with_a_message(self):
        with self.assertRaises(SystemExit):
            list(two.frames(capture('v003-wlink-status-timing.csv')))


class OneWire(unittest.TestCase):
    def test_host_phase_of_status_run(self):
        got = [one.decode(f) for _, f in one.frames(capture('v003-wlink-status-timing.csv'), 91.2)]
        self.assertEqual([(d['addr'], d['write'], d['data']) for d in got[:5]],
                         [(0x16, True, 0x700), (0x04, True, 0), (0x17, True, 0x220301),
                          (0x16, False, 0x08000002), (0x04, False, 0x40800014)])

    def test_connect_phase_after_the_break_pulse(self):
        got = [one.decode(f) for _, f in one.frames(capture('v003-wlink-status-timing.csv'), 76)]
        self.assertEqual([(d['addr'], d['write'], d['data']) for d in got[:2]],
                         [(0x7e, True, 0x5aa50400), (0x7d, True, 0x5aa50400)])
        self.assertIn((0x7f, False, 0x00300500), [(d['addr'], d['write'], d['data']) for d in got])

    def test_status_run_leaves_one_pulse(self):
        left = []
        list(one.frames(capture('v003-wlink-status-timing.csv'), 78, left))
        self.assertEqual([n for _, n in left], [1])

    def test_dump_burst_falls_out_of_step_and_is_reported(self):
        left = []
        got = list(one.frames(capture('v003-wlink-dump-timing.csv'), 89, left))
        self.assertEqual(len(got), 28)
        self.assertAlmostEqual(left[0][0], 0.0909519, places=5)
        self.assertEqual(left[0][1], 544)


class OneWireV006(unittest.TestCase):
    def frames(self):
        return [one.decode(f) for _, f in one.frames(capture('v006-wlink-status-timing.csv'), 76)]

    def test_connect_phase_after_the_two_wire_attempts(self):
        got = [(d['addr'], d['write'], d['data']) for d in self.frames()]
        self.assertEqual(got[:3], [(0x7e, True, 0x5aa50400), (0x7d, True, 0x5aa50400),
                                   (0x7c, False, 0x11038403)])
        self.assertIn((0x7f, False, 0x00600620), got)

    def test_protection_registers_are_read_with_the_abstract_command(self):
        got = [(d['addr'], d['write'], d['data']) for d in self.frames()]
        i = got.index((0x05, True, 0x4002201c))
        self.assertEqual(got[i + 1:i + 3], [(0x17, True, 0x02200000), (0x04, False, 0x03fffffc)])


if __name__ == '__main__':
    unittest.main()
