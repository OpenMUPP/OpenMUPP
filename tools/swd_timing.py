#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measures the SWD half periods, the SWDIO setup time and the gaps in a Logic 2 CSV export.

Usage: swd_timing.py <digital.csv> <clock column> <data column> <nominal period in ns>

The CSV is a raw export of Logic 2, as for clock_rate.py. The columns are
the indexes of SWCLK and SWDIO among the channel columns (0 is the first
channel).

The script prints:

- The high and the low half periods of SWCLK, only in runs of consecutive
  bits (see clock_rate.py): the median and the shortest.
- The SWDIO setup time: the time from an SWDIO change while SWCLK is low to
  the next rising edge of SWCLK. The host changes SWDIO after the falling
  edge, and the target after the rising edge. Thus this time is the setup
  time of the host writes. A target that changes SWDIO late in the high half
  period can add values here.
- The count of SWDIO changes in the same CSV row as a rising edge of SWCLK.
  The CSV cannot tell a host write with a setup time below one sample from
  a target change just after the edge, so these changes are not setup
  times. At 500 MS/s the target changes (6 to 10 ns after the edge) are in
  a later row, so a count above 0 there shows a setup time below 2 ns.
- The gaps: the times between two rising edges that are longer than
  GAP_LIMIT times the nominal period, in groups by length. The short gaps are
  the gaps between two transfers in one USB packet, and the long gaps are
  the gaps between two packets.

See docs/clock-pacing.md.
"""

import csv
import statistics
import sys

from clock_rate import runs

GAP_LIMIT = 3.0
# The SWCLK state of an SWDIO change (see edges()).
LOW, HIGH, RISING = "low", "high", "rising"
# The upper limits of the gap groups, in seconds.
GAP_GROUPS = (1e-6, 2e-6, 5e-6, 10e-6, 20e-6, 50e-6, 100e-6, 1e-3, float("inf"))


def edges(rows, clock, data):
    """The SWCLK edges as (time, level) and the SWDIO changes as (time, SWCLK state).

    The state is HIGH or LOW, or RISING for a change in the same row as a rising edge.
    A change in the same row as a falling edge is LOW: the host writes after the falling edge.
    """
    clock_edges = []
    data_changes = []
    last_clock = last_data = None
    for row in rows:
        time = float(row[0])
        level_clock = row[1 + clock]
        level_data = row[1 + data]
        if last_clock is not None and level_clock != last_clock:
            clock_edges.append((time, level_clock == "1"))
        if last_data is not None and level_data != last_data:
            if level_clock == last_clock:
                state = HIGH if level_clock == "1" else LOW
            else:
                state = RISING if level_clock == "1" else LOW
            data_changes.append((time, state))
        last_clock, last_data = level_clock, level_data
    return clock_edges, data_changes


def half_periods(clock_edges, nominal):
    """The high and the low half periods inside runs of consecutive bits."""
    rising = [t for t, high in clock_edges if high]
    in_run = set()
    for run in runs(rising, nominal):
        in_run.update(run[:-1])
    high, low = [], []
    previous_rising = None
    for (a, a_high), (b, b_high) in zip(clock_edges, clock_edges[1:]):
        if a_high:
            previous_rising = a
            if a in in_run:
                high.append(b - a)
        # A low half period counts if its bit (from the rising edge before it) is in a run.
        elif b_high and previous_rising in in_run:
            low.append(b - a)
    return high, low


def setup_times(clock_edges, data_changes):
    """The time from each SWDIO change while SWCLK is low to the next rising edge."""
    rising = [t for t, high in clock_edges if high]
    result = []
    i = 0
    for time, state in data_changes:
        if state != LOW:
            continue
        while i < len(rising) and rising[i] <= time:
            i += 1
        if i == len(rising):
            break
        result.append(rising[i] - time)
    return result


def same_row_rising(data_changes):
    """The count of SWDIO changes in the same row as a rising edge of SWCLK."""
    return sum(1 for _, state in data_changes if state == RISING)


def gaps(clock_edges, nominal):
    """The periods between two rising edges that are longer than GAP_LIMIT times the nominal period."""
    rising = [t for t, high in clock_edges if high]
    return [b - a for a, b in zip(rising, rising[1:]) if b - a > GAP_LIMIT * nominal]


def gap_groups(values):
    """The count and the median of the gaps in each group of GAP_GROUPS."""
    result = []
    lower = 0.0
    for upper in GAP_GROUPS:
        group = [v for v in values if lower < v <= upper]
        if group:
            result.append((lower, upper, len(group), statistics.median(group)))
        lower = upper
    return result


def ns(value):
    return value * 1e9


def main(argv):
    if len(argv) != 5:
        sys.exit(__doc__)
    nominal = float(argv[4]) * 1e-9
    with open(argv[1], newline="") as f:
        reader = csv.reader(f)
        next(reader)
        clock_edges, data_changes = edges(reader, int(argv[2]), int(argv[3]))
    high, low = half_periods(clock_edges, nominal)
    if not high or not low:
        sys.exit("no run of consecutive bits")
    print("high half period: median %.0f ns, shortest %.0f ns (%d)" % (ns(statistics.median(high)), ns(min(high)), len(high)))
    print("low half period: median %.0f ns, shortest %.0f ns (%d)" % (ns(statistics.median(low)), ns(min(low)), len(low)))
    setup = setup_times(clock_edges, data_changes)
    if setup:
        print("SWDIO setup: median %.0f ns, shortest %.0f ns (%d)" % (ns(statistics.median(setup)), ns(min(setup)), len(setup)))
    print("SWDIO changes in the same sample as a rising edge: %d" % same_row_rising(data_changes))
    for lower, upper, count, median in gap_groups(gaps(clock_edges, nominal)):
        print("gaps %.0f to %.0f us: %d, median %.2f us" % (lower * 1e6, upper * 1e6, count, median * 1e6))


if __name__ == "__main__":
    main(sys.argv)
