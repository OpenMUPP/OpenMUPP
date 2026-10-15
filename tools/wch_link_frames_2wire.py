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
MAX_STRAY = 2  # a burst of 55 or 56 clocks holds one frame and stray clocks
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
    # some frames have a long pause after the start clock. The start clock is
    # the last clock of the burst before it, or a burst of its own.
    joined = []
    for b in out:
        if joined and len(b) == FRAME_BITS - 1 and b[0] - joined[-1][-1] < START_GAP:
            b = [joined[-1].pop()] + b
            if not joined[-1]:
                joined.pop()
        joined.append(b)
    return joined


def frames(path, start_ms=0.0, skipped=None):
    """Yield (time, 54 bits). Add (time, clocks) to `skipped` for a burst that is not a multiple of 54."""
    rows, times, rising = load(path)
    for burst in bursts(rising):
        if burst[0] * 1e3 < start_ms:
            continue
        # SWDIO is stable at the rising edge: sample just before it
        bits = ''.join(str(rows[bisect.bisect_right(times, t - 5e-9) - 1][1]) for t in burst)
        if FRAME_BITS < len(bits) <= FRAME_BITS + MAX_STRAY:
            # a frame with 1 or 2 stray clocks before or after it: keep the part with right parity
            for k in range(len(bits) - FRAME_BITS + 1):
                if valid(bits[k:k + FRAME_BITS]):
                    yield burst[k], bits[k:k + FRAME_BITS]
                    break
            else:
                if skipped is not None:
                    skipped.append((burst[0], len(bits)))
            continue
        if len(bits) % FRAME_BITS:
            if skipped is not None:
                skipped.append((burst[0], len(bits)))
            continue
        for k in range(0, len(bits), FRAME_BITS):
            yield burst[k], bits[k:k + FRAME_BITS]


def valid(f):
    return f[0] == '1' and f[47:49] == ('11' if f[15:47].count('1') % 2 else '00')


def decode(f):
    return dict(start=f[0], addr=int(f[1:8], 2), op=f[8:10], ack=f[10:15],
                data=int(f[15:47], 2), parity=f[47:49], tail=f[49:],
                parity_ok=f[47:49] == ('11' if f[15:47].count('1') % 2 else '00'))


if __name__ == '__main__':
    skipped = []
    for t, f in frames(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.0, skipped):
        d = decode(f)
        print(f"{t * 1e3:9.3f} ms A={d['addr']:02x} op={d['op']} ack={d['ack']} D={d['data']:08x} "
              f"par={d['parity']}{'' if d['parity_ok'] else ' BAD'} tail={d['tail']}")
    if skipped:
        print(f"skipped {len(skipped)} bursts that are not a multiple of {FRAME_BITS} clocks "
              f"(not decoded)", file=sys.stderr)
