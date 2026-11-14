# SPDX-License-Identifier: GPL-3.0-or-later

"""Host tests for swd_timing.py.

Run: python3 -m unittest discover -s tools -p "test_*.py"
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swd_timing  # noqa: E402


def frame(start, bits, high_ns=100, low_ns=150, setup_ns=40):
    """Rows of a run: channel 0 is SWDIO, channel 1 is SWCLK. The host changes SWDIO setup_ns before each rising edge."""
    out = []
    t = start
    data = 0
    for bit in bits:
        # Low half period: SWDIO changes setup_ns before the rising edge.
        if bit != data:
            data = bit
            out.append((t + (low_ns - setup_ns) * 1e-9, data, 0))
        t += low_ns * 1e-9
        out.append((t, data, 1))
        t += high_ns * 1e-9
        out.append((t, data, 0))
    return out, t


def csv_rows(events):
    return [["%.9f" % t, str(d), str(c)] for t, d, c in [(0.0, 0, 0)] + events]


class SwdTimingTest(unittest.TestCase):
    def setUp(self):
        bits = [1, 0] * 20
        first, end = frame(1e-6, bits)
        second, _ = frame(end + 3e-6, bits)
        self.rows = csv_rows(first + second)

    def test_half_periods_and_setup(self):
        clock_edges, data_changes = swd_timing.edges(self.rows, 1, 0)
        high, low = swd_timing.half_periods(clock_edges, 250e-9)
        self.assertAlmostEqual(min(high), 100e-9, delta=1e-12)
        self.assertAlmostEqual(max(high), 100e-9, delta=1e-12)
        self.assertAlmostEqual(min(low), 150e-9, delta=1e-12)
        self.assertAlmostEqual(max(low), 150e-9, delta=1e-12)
        setup = swd_timing.setup_times(clock_edges, data_changes)
        self.assertEqual(len(setup), 80)
        self.assertAlmostEqual(min(setup), 40e-9, delta=1e-12)
        self.assertAlmostEqual(max(setup), 40e-9, delta=1e-12)

    def test_gap_between_runs(self):
        clock_edges, _ = swd_timing.edges(self.rows, 1, 0)
        found = swd_timing.gaps(clock_edges, 250e-9)
        self.assertEqual(len(found), 1)
        self.assertAlmostEqual(found[0], 3.25e-6, delta=1e-12)
        groups = swd_timing.gap_groups(found)
        self.assertEqual([(g[0], g[1], g[2]) for g in groups], [(2e-6, 5e-6, 1)])

    def test_change_while_clock_high_is_not_setup(self):
        rows = csv_rows([(1e-6, 0, 1), (1.05e-6, 1, 1), (1.1e-6, 1, 0), (1.2e-6, 1, 1)])
        clock_edges, data_changes = swd_timing.edges(rows, 1, 0)
        self.assertEqual(swd_timing.setup_times(clock_edges, data_changes), [])


if __name__ == "__main__":
    unittest.main()
