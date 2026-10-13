"""Tests for the WCH-LinkE wire frame decoders. They use the captures in docs/wch-link-captures."""
import os
import unittest

import wch_link_frames_1wire as one
import wch_link_frames_2wire as two

CAPTURES = os.path.join(os.path.dirname(__file__), '..', 'docs', 'wch-link-captures')


def capture(name):
    return os.path.join(CAPTURES, name)


class TwoWire(unittest.TestCase):
    def test_host_phase_of_status_run(self):
        # The first accesses of the host: abstractcs write, data0 write, command write, abstractcs read
        got = [two.decode(f) for _, f in two.frames(capture('v208-wlink-status-timing.csv'), 60.1)]
        self.assertEqual([(d['addr'], d['op'], d['data']) for d in got[:4]],
                         [(0x16, '10', 0x700), (0x04, '10', 0), (0x17, '11', 0x220301),
                          (0x16, '01', 0x08000002)])
        self.assertTrue(all(d['parity_ok'] for d in got))

    def test_connect_phase_frames_are_joined(self):
        # Frames with a pause after the start clock must decode, not drop out
        got = [two.decode(f) for _, f in two.frames(capture('v208-wlink-status-timing.csv'), 45)]
        self.assertGreater(len(got), 100)
        self.assertTrue(all(d['parity_ok'] for d in got))
        self.assertIn(0x2080053c, [d['data'] for d in got])

    def test_chip_id_words_match_the_usb_answer(self):
        # The 0x11 answer is the words at 0x1ffff7e0.. in this order
        data = [two.decode(f)['data'] for _, f in two.frames(capture('v208-wlink-status-timing.csv'), 59.6)]
        i = data.index(0xffff00a0)
        self.assertEqual(data[i:i + 1] + [d for d in data[i + 1:] if d in (0x6213f0eb, 0x2f30dc32, 0xe339e339)][:3],
                         [0xffff00a0, 0x6213f0eb, 0x2f30dc32, 0xe339e339])


class OneWire(unittest.TestCase):
    def test_host_phase_of_status_run(self):
        got = [one.decode(f) for _, f in one.frames(capture('v003-wlink-status-timing.csv'), 91.2)]
        self.assertEqual([(d['addr'], d['write'], d['data']) for d in got[:5]],
                         [(0x16, True, 0x700), (0x04, True, 0), (0x17, True, 0x220301),
                          (0x16, False, 0x08000002), (0x04, False, 0x40800014)])
        self.assertTrue(all(d['start'] == '1' for d in got))

    def test_misaligned_burst_is_reported(self):
        left = []
        list(one.frames(capture('v003-wlink-dump-timing.csv'), 90, left))
        self.assertTrue(left)  # the dump burst falls out of step; the decoder says so


if __name__ == '__main__':
    unittest.main()
