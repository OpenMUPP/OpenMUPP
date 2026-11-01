# SPDX-License-Identifier: GPL-3.0-or-later

"""Host tests for clock_rate.py.

Run: python3 -m unittest discover -s tools -p "test_*.py"
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import clock_rate  # noqa: E402


def rows(edges):
    """CSV rows of two channels: channel 1 is the clock, channel 0 stays low."""
    out = []
    level = 0
    for t in edges:
        out.append(["%.9f" % t, "0", str(level)])
        level ^= 1
    return out


class ClockRateTest(unittest.TestCase):
    def test_rising_edges_of_one_channel(self):
        data = [["0.0", "0", "0"], ["1.0", "1", "0"], ["2.0", "1", "1"], ["3.0", "0", "0"], ["4.0", "0", "1"]]
        self.assertEqual(clock_rate.rising_edges(data, 1), [2.0, 4.0])
        self.assertEqual(clock_rate.rising_edges(data, 0), [1.0])

    def test_gaps_are_not_part_of_the_rate(self):
        # Two runs of 40 bits at 1 us, with a gap of 50 us between them
        edges = [i * 1e-6 for i in range(41)] + [90e-6 + i * 1e-6 for i in range(41)]
        mean, shortest, longest, bits = clock_rate.measure(edges, 1e-6)
        self.assertAlmostEqual(mean, 1e-6)
        self.assertAlmostEqual(shortest, 1e-6)
        self.assertAlmostEqual(longest, 1e-6)
        self.assertEqual(bits, 80)

    def test_short_runs_do_not_count(self):
        edges = [i * 1e-6 for i in range(10)] + [100e-6 + i * 1e-6 for i in range(41)]
        self.assertEqual(clock_rate.measure(edges, 1e-6)[3], 40)

    def test_no_run_is_an_error(self):
        with self.assertRaises(ValueError):
            clock_rate.measure([i * 2e-6 for i in range(100)], 1e-6)

    def test_median_includes_the_gaps(self):
        edges = [0.0, 1e-6, 2e-6, 3e-6, 50e-6]
        self.assertAlmostEqual(clock_rate.median_period(edges), 1e-6)

    def test_median_needs_two_edges(self):
        with self.assertRaises(ValueError):
            clock_rate.median_period([1.0])

    def test_edges_from_rows(self):
        # Two edges per period of 1 us: high and low for 0.5 us each
        data = rows([i * 0.5e-6 for i in range(100)])
        edges = clock_rate.rising_edges(data, 1)
        self.assertAlmostEqual(clock_rate.measure(edges, 1e-6)[0], 1e-6)


if __name__ == "__main__":
    unittest.main()
