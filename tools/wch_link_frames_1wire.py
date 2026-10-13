"""Decode the 41-pulse frames of the WCH-LinkE one-wire interface (CH32V003).

Input: a Saleae CSV export (digital, one row for each change) with
channel 0 = SWIO. See docs/wch-link-protocol.md.
Usage: python3 decode_frames_1wire.py capture.csv [first-frame-time-in-ms]
"""
import csv
import sys

FRAME_PULSES = 41
IDLE = 20e-6        # a high level longer than this ends a burst of frames
LONG_LOW = 0.5e-6   # a low pulse longer than this is a 0 bit (short pulse: 1 bit)


def pulses(path):
    rows = [(float(r[0]), int(r[1])) for r in list(csv.reader(open(path)))[1:]]
    edges = [(rows[i][0], rows[i][1]) for i in range(1, len(rows)) if rows[i][1] != rows[i - 1][1]]
    return [(t, edges[i + 1][0] - t, lvl) for i, (t, lvl) in enumerate(edges[:-1])]


def frames(path, start_ms=0.0):
    burst = []
    for t, d, lvl in pulses(path):
        if t * 1e3 < start_ms:
            continue
        if lvl == 1 and d > IDLE:
            yield from split(burst)
            burst = []
        elif lvl == 0:
            burst.append((t, '0' if d > LONG_LOW else '1'))
    yield from split(burst)


def split(burst):
    for k in range(0, len(burst) - FRAME_PULSES + 1, FRAME_PULSES):
        chunk = burst[k:k + FRAME_PULSES]
        yield chunk[0][0], ''.join(b for _, b in chunk)


def decode(f):
    return dict(start=f[0], addr=int(f[1:8], 2), write=f[8] == '1', data=int(f[9:], 2))


if __name__ == '__main__':
    for t, f in frames(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.0):
        d = decode(f)
        print(f"{t * 1e3:9.3f} ms A={d['addr']:02x} {'W' if d['write'] else 'R'} D={d['data']:08x}")
