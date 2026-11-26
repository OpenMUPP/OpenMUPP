# SPDX-License-Identifier: GPL-3.0-or-later

"""Decode the 41-pulse frames of the WCH-LinkE one-wire interface (CH32V003).

Input: a Saleae CSV export (digital, one row for each change) with
channel 0 = SWIO. See docs/wch-link-protocol.md.
Usage: python3 wch_link_frames_1wire.py capture.csv [first-frame-time-in-ms]

Only the host phase of a status run is verified (see the document). The
decoder checks the start bit of each frame. It stops at the first bad
start bit of a burst and reports the number of pulses it did not decode.
"""
import csv
import sys

FRAME_PULSES = 41
IDLE = 20e-6        # a high level longer than this ends a burst of frames
LONG_LOW = 0.5e-6   # a low pulse longer than this is a 0 bit (short pulse: 1 bit)
BREAK = 1.5e-6      # a low pulse longer than this is not a bit


def pulses(path):
    with open(path) as f:
        rows = [(float(r[0]), int(r[1])) for r in list(csv.reader(f))[1:]]
    edges = [(rows[i][0], rows[i][1]) for i in range(1, len(rows)) if rows[i][1] != rows[i - 1][1]]
    if len(edges) < 2:
        sys.exit(f'{path}: channel 0 has no edges')
    return [(t, edges[i + 1][0] - t, lvl) for i, (t, lvl) in enumerate(edges[:-1])]


def bursts(path, start_ms=0.0):
    """Yield lists of (time, bit). A long high level or a break pulse ends a list."""
    burst = []
    for t, d, lvl in pulses(path):
        if t * 1e3 < start_ms:
            continue
        if lvl == 1 and d > IDLE or lvl == 0 and d > BREAK:
            if burst:
                yield burst
            burst = []
        elif lvl == 0:
            burst.append((t, '0' if d > LONG_LOW else '1'))
    if burst:
        yield burst


def frames(path, start_ms=0.0, undecoded=None):
    """Yield (time, 41 bits). Add (time, pulses left) to `undecoded` for the rest of a burst."""
    for burst in bursts(path, start_ms):
        k = 0
        while k + FRAME_PULSES <= len(burst) and burst[k][1] == '1':
            yield burst[k][0], ''.join(b for _, b in burst[k:k + FRAME_PULSES])
            k += FRAME_PULSES
        if k < len(burst) and undecoded is not None:
            undecoded.append((burst[k][0], len(burst) - k))


def decode(f):
    return dict(start=f[0], addr=int(f[1:8], 2), write=f[8] == '1', data=int(f[9:], 2))


if __name__ == '__main__':
    left = []
    for t, f in frames(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.0, left):
        d = decode(f)
        print(f"{t * 1e3:9.3f} ms A={d['addr']:02x} {'W' if d['write'] else 'R'} D={d['data']:08x}")
    if left:
        print(f"not decoded: {len(left)} bursts, {sum(n for _, n in left)} pulses "
              f"(first at {left[0][0] * 1e3:.3f} ms)", file=sys.stderr)
