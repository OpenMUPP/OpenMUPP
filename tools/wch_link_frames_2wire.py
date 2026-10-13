"""Decode the 54-clock frames of the WCH-LinkE two-wire interface.

Input: a Saleae CSV export (digital, one row for each change) with
channel 0 = SWDIO and channel 1 = SWCLK. See docs/wch-link-protocol.md.
Usage: python3 wch_link_frames_2wire.py capture.csv [first-frame-time-in-ms]

The 85-clock frames of the connect phase are not decoded. The decoder
skips them and counts them.
"""
import bisect
import csv
import sys

FRAME_BITS = 54
FRAME_GAP = 5e-6  # clocks further apart than this belong to different bursts
START_GAP = 20e-6  # a lone start clock and the rest of its frame are this close


def load(path):
    with open(path) as f:
        rows = [(float(r[0]), int(r[1]), int(r[2])) for r in list(csv.reader(f))[1:]]
    if not any(r[2] for r in rows) or all(r[2] for r in rows):
        sys.exit(f'{path}: channel 1 (SWCLK) has no edges; this is not a two-wire capture')
    times = [r[0] for r in rows]
    rising = [rows[i][0] for i in range(1, len(rows)) if rows[i][2] == 1 and rows[i - 1][2] == 0]
    return rows, times, rising


def bursts(rising):
    out, cur = [], [rising[0]]
    for t in rising[1:]:
        if t - cur[-1] > FRAME_GAP:
            out.append(cur)
            cur = [t]
        else:
            cur.append(t)
    out.append(cur)
    # some frames have a long pause after the start clock: join the two parts
    joined = []
    for b in out:
        if joined and len(joined[-1]) == 1 and b[0] - joined[-1][0] < START_GAP:
            joined[-1] = joined[-1] + b
        else:
            joined.append(b)
    return joined


skipped = []  # (time, clocks) of bursts that are not a multiple of 54 clocks


def frames(path, start_ms=0.0):
    rows, times, rising = load(path)
    for burst in bursts(rising):
        if burst[0] * 1e3 < start_ms:
            continue
        # SWDIO is stable at the rising edge: sample just before it
        bits = ''.join(str(rows[bisect.bisect_right(times, t - 5e-9) - 1][1]) for t in burst)
        if len(bits) % FRAME_BITS:
            skipped.append((burst[0], len(bits)))
            continue
        for k in range(0, len(bits), FRAME_BITS):
            yield burst[k], bits[k:k + FRAME_BITS]


def decode(f):
    return dict(start=f[0], addr=int(f[1:8], 2), op=f[8:10], ack=f[10:15],
                data=int(f[15:47], 2), parity=f[47:49], tail=f[49:],
                parity_ok=f[47:49] == ('11' if f[15:47].count('1') % 2 else '00'))


if __name__ == '__main__':
    for t, f in frames(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.0):
        d = decode(f)
        print(f"{t * 1e3:9.3f} ms A={d['addr']:02x} op={d['op']} ack={d['ack']} D={d['data']:08x} "
              f"par={d['parity']}{'' if d['parity_ok'] else ' BAD'} tail={d['tail']}")
    if skipped:
        print(f"skipped {len(skipped)} bursts that are not a multiple of {FRAME_BITS} clocks "
              f"(not decoded)", file=sys.stderr)
