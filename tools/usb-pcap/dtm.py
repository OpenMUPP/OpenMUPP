# SPDX-License-Identifier: GPL-3.0-or-later

"""Decode the RISC-V DTM scans of a usbmon pcapng, with the TDO bits of the replies.

Usage: dtm.py FILE.pcapng DEV [EP_OUT] [EP_IN]
EP_OUT is 3 and EP_IN is 0x82 by default (the CMSIS-DAP endpoints of the probe).
It pairs each DAP_SWJ_Sequence and DAP_JTAG_Sequence request with its reply, runs the
TMS bits through the TAP state machine, and prints one line for each scan:
  IR <bits> <value>
  DR ir=<ir> <bits> in=<tdi> out=<tdo>
  DMI <op> a=<address> d=<data> | prev: a=<address> d=<data> op=<status>
"prev" is the captured value: the result of the access before. Bits without the
TDO capture flag read as 0. Test-Logic-Reset selects IDCODE (0x01).
A scan longer than the register keeps its last bits, as the TAP does: the IR is the
last 5 bits, a DMI scan the last 41 bits.
Limit: it skips DAP_Transfer and DAP_TransferBlock. The firmware scans for these
commands too, so the TAP state and the IR after them are not known until the next TLR.
"""
import sys
from pcap import usb
from dap import NEXT

IR_LENGTH = 5
IR_DMI = 0x11
DMI_LENGTH = 41
OPS = {0: 'nop', 1: 'rd', 2: 'wr', 3: 'rsv'}

def bit(data, offset, i):
    return data[offset + i // 8] >> (i % 8) & 1

def clocks(request, reply):
    """The (tms, tdi, tdo) bits of one request. tdo is None without capture."""
    if request[0] == 0x12:
        return [(bit(request, 2, i), 1, None) for i in range(request[1] or 256)]
    out, p, o = [], 2, 2
    for _ in range(request[1]):
        info = request[p]; p += 1
        count = (info & 0x3f) or 64
        tms, capture = info >> 6 & 1, info >> 7
        for i in range(count):
            out.append((tms, bit(request, p, i), bit(reply, o, i) if capture else None))
        p += (count + 7) // 8
        o += (count + 7) // 8 if capture else 0
    return out

def scans(path, dev, ep_out, ep_in):
    requests, replies = [], []
    for p in usb(path):
        if p['dev'] != dev or not p['data']:
            continue
        if p['ep'] == ep_out and p['type'] == 'S':
            requests.append(p['data'])
        elif p['ep'] == ep_in and p['type'] == 'C':
            replies.append(p['data'])
    state, ir, shift = 'TLR', 0x01, None
    for request, reply in zip(requests, replies):
        if request[0] not in (0x12, 0x14):
            continue
        for tms, tdi, tdo in clocks(request, reply):
            if state in ('SHDR', 'SHIR'):
                shift = shift or (state, [], [])
                shift[1].append(tdi)
                shift[2].append(tdo or 0)
            nxt = NEXT[state][tms]
            if nxt in ('UPDR', 'UPIR', 'TLR') and shift:
                kind, ti, to = shift
                shift = None
                v = sum(b << i for i, b in enumerate(ti))
                w = sum(b << i for i, b in enumerate(to))
                if kind == 'SHIR':
                    if len(ti) >= IR_LENGTH:
                        ir = v >> (len(ti) - IR_LENGTH) & 0x1f
                    yield f"IR {len(ti)}b {v:#x}"
                elif ir == IR_DMI and len(ti) >= DMI_LENGTH:
                    v >>= len(ti) - DMI_LENGTH
                    w &= (1 << DMI_LENGTH) - 1
                    yield (f"DMI {OPS[v & 3]} a={v >> 34 & 0x7f:#04x} d={v >> 2 & 0xffffffff:08x}"
                           f" | prev: a={w >> 34 & 0x7f:#04x} d={w >> 2 & 0xffffffff:08x} op={w & 3}")
                else:
                    yield f"DR ir={ir:#04x} {len(ti)}b in={v:#x} out={w:#x}"
            if nxt == 'TLR' and state != 'TLR':
                yield 'TLR'
            state = nxt
            if state == 'TLR':
                ir = 0x01

if __name__ == '__main__':
    args = sys.argv[1:]
    dev = int(args[1])
    ep_out = int(args[2], 0) if len(args) > 2 else 3
    ep_in = int(args[3], 0) if len(args) > 3 else 0x82
    for line in scans(args[0], dev, ep_out, ep_in):
        print(line)
