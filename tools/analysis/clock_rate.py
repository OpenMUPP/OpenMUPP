#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measures the clock rate of a bit-bang engine in a Logic 2 CSV export.

Usage: clock_rate.py <digital.csv> <column> <nominal period in ns>

The CSV is a raw export of Logic 2: a time column, then one column for each
channel, and one row for each change on any channel. <column> is the index
of the clock channel among the channel columns (0 is the first channel).

The rate counts only runs of consecutive bits: at least MIN_RUN periods
that are each shorter than RUN_LIMIT times the nominal period. The gaps
between transfers and the longer phases of a frame are then not part of the
rate. It also prints the median and the shortest of all periods, also
when there is no run
(for example when the code makes each period longer than RUN_LIMIT times
the nominal period). See docs/clock-pacing.md.
"""

import csv
import statistics
import sys

MIN_RUN = 30
RUN_LIMIT = 1.3


def rising_edges(rows, column):
    """The times of the rising edges of one channel."""
    edges = []
    last = None
    for row in rows:
        level = row[1 + column]
        if last is not None and level != last and level == "1":
            edges.append(float(row[0]))
        last = level
    return edges


def runs(edges, nominal):
    """Runs of consecutive bits: lists of edge times."""
    found = []
    current = edges[:1]
    for a, b in zip(edges, edges[1:]):
        if b - a < RUN_LIMIT * nominal:
            current.append(b)
        else:
            if len(current) > MIN_RUN:
                found.append(current)
            current = [b]
    if len(current) > MIN_RUN:
        found.append(current)
    return found


def measure(edges, nominal):
    """The mean period of the runs, the shortest and the longest period, and the bit count."""
    found = runs(edges, nominal)
    if not found:
        raise ValueError("no run of %d bits below %.1f times the nominal period" % (MIN_RUN, RUN_LIMIT))
    periods = [b - a for run in found for a, b in zip(run, run[1:])]
    total = sum(run[-1] - run[0] for run in found)
    return total / len(periods), min(periods), max(periods), len(periods)


def median_period(edges):
    """The median of all periods between rising edges, gaps included."""
    if len(edges) < 2:
        raise ValueError("fewer than two rising edges")
    return statistics.median(b - a for a, b in zip(edges, edges[1:]))


def main(argv):
    if len(argv) != 4:
        sys.exit(__doc__)
    nominal = float(argv[3]) * 1e-9
    with open(argv[1], newline="") as f:
        reader = csv.reader(f)
        next(reader)
        edges = rising_edges(reader, int(argv[2]))
    try:
        median = median_period(edges)
    except ValueError as error:
        sys.exit(str(error))
    shortest_all = min(b - a for a, b in zip(edges, edges[1:]))
    print("all periods: median %.1f ns = %.1f kHz, shortest %.0f ns" % (median * 1e9, 1e-3 / median, shortest_all * 1e9))
    try:
        mean, shortest, longest, bits = measure(edges, nominal)
    except ValueError as error:
        sys.exit(str(error))
    print("%d bits: %.1f kHz (%+.2f %%), period %.1f ns, shortest %.0f ns, longest %.0f ns" % (
        bits, 1e-3 / mean, 100 * (nominal / mean - 1), mean * 1e9, shortest * 1e9, longest * 1e9))


if __name__ == "__main__":
    main(sys.argv)
