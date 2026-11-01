# SPDX-License-Identifier: GPL-3.0-or-later

"""Decode the CMSIS-DAP request stream (bulk OUT) of a usbmon pcapng.

Usage: dap.py FILE.pcapng DEV [EP_OUT] [--scans | --trace]
Prints one line per command. TMS and TDI bit strings are in time order, first bit first.
With --scans, it runs the TMS bits through the JTAG TAP state machine and prints
the TLR entries and the IR and DR scans (bit count and TDI value, first bit = LSB).
With --trace, it prints each request as "> hex", for the host tests (tests/replay).
"""
import sys
from pcap import usb

def bits(data, n):
    return ''.join(str(data[i // 8] >> (i % 8) & 1) for i in range(n))

def jtag_seq(b):
    n, p, out = b[1], 2, []
    for _ in range(n):
        info = b[p]; p += 1
        c = (info & 0x3f) or 64
        tms, cap = info >> 6 & 1, info >> 7
        tdi = b[p:p + (c + 7) // 8]; p += (c + 7) // 8
        out.append(f"{c}b tms={tms}{' cap' if cap else ''} tdi={bits(tdi, c)}")
    return '; '.join(out)

# next state of the TAP for TMS = 0 and 1
NEXT = {
    'TLR': ('RTI', 'TLR'), 'RTI': ('RTI', 'SELDR'),
    'SELDR': ('CAPDR', 'SELIR'), 'CAPDR': ('SHDR', 'EX1DR'), 'SHDR': ('SHDR', 'EX1DR'),
    'EX1DR': ('PADR', 'UPDR'), 'PADR': ('PADR', 'EX2DR'), 'EX2DR': ('SHDR', 'UPDR'),
    'UPDR': ('RTI', 'SELDR'),
    'SELIR': ('CAPIR', 'TLR'), 'CAPIR': ('SHIR', 'EX1IR'), 'SHIR': ('SHIR', 'EX1IR'),
    'EX1IR': ('PAIR', 'UPIR'), 'PAIR': ('PAIR', 'EX2IR'), 'EX2IR': ('SHIR', 'UPIR'),
    'UPIR': ('RTI', 'SELDR'),
}

class Tap:
    def __init__(self):
        self.state, self.shift, self.idle = 'TLR', [], 0
    def clock(self, tms, tdi):
        """Clock one TCK. Return an event string when a scan or a TLR entry ends."""
        ev = None
        if self.state in ('SHDR', 'SHIR'):
            self.shift.append(tdi)
        nxt = NEXT[self.state][tms]
        # Limit: a scan with a Pause state inside is reported as two scans.
        if self.state in ('SHDR', 'SHIR') and nxt.startswith('EX1'):
            v = sum(b << i for i, b in enumerate(self.shift))
            ev = f"{'IR' if self.state == 'SHIR' else 'DR'} scan {len(self.shift)}b tdi={v:#x}"
            self.shift = []
        if self.state == 'RTI' and nxt == 'RTI':
            self.idle += 1
        elif nxt in ('UPDR', 'UPIR', 'SELDR', 'TLR') and self.idle:
            ev = (ev + ' ' if ev else '') + f"[{self.idle} idle]"
            self.idle = 0
        if nxt == 'TLR' and self.state != 'TLR':
            ev = (ev + ' ' if ev else '') + 'TLR'
        self.state = nxt
        return ev

def tap_events(tap, b):
    """Feed one DAP command to the TAP. Return the events it causes."""
    evs = []
    def feed(tms, tdi):
        e = tap.clock(tms, tdi)
        if e: evs.append(e)
    if b[0] == 0x12:
        for i in range(b[1] or 256): feed(b[2 + i // 8] >> (i % 8) & 1, 1)
    elif b[0] == 0x14:
        p = 2
        for _ in range(b[1]):
            info = b[p]; p += 1
            c = (info & 0x3f) or 64
            for i in range(c): feed(info >> 6 & 1, b[p + i // 8] >> (i % 8) & 1)
            p += (c + 7) // 8
    return evs

def decode(b):
    c = b[0]
    if c == 0x00: return f"Info id={b[1]:#04x}"
    if c == 0x02: return f"Connect port={b[1]}"
    if c == 0x03: return "Disconnect"
    if c == 0x04: return f"TransferConfigure idle={b[1]} wait={int.from_bytes(b[2:4], 'little')} match={int.from_bytes(b[4:6], 'little')}"
    if c == 0x10: return f"SWJ_Pins out={b[1]:#04x} select={b[2]:#04x} wait={int.from_bytes(b[3:7], 'little')}"
    if c == 0x11: return f"SWJ_Clock {int.from_bytes(b[1:5], 'little')} Hz"
    if c == 0x12:
        n = b[1] or 256
        return f"SWJ_Sequence {n}b tms={bits(b[2:], n)}"
    if c == 0x14: return "JTAG_Sequence " + jtag_seq(b)
    if c == 0x15: return f"JTAG_Configure count={b[1]} ir_len={list(b[2:2 + b[1]])}"
    if c == 0x16: return f"JTAG_IDCODE index={b[1]}"
    if c == 0x01: return f"HostStatus type={b[1]} status={b[2]}"
    return f"cmd {c:#04x} {b.hex()}"

if __name__ == '__main__':
    scans = '--scans' in sys.argv
    trace = '--trace' in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    dev = int(args[1]); ep = int(args[2], 0) if len(args) > 2 else 3
    t0, tap = None, Tap()
    for p in usb(args[0]):
        if p['dev'] != dev or p['ep'] != ep or p['type'] != 'S' or not p['data']: continue
        t0 = t0 or p['ts']
        if trace:
            print(f"> {p['data'].hex()}")
        elif scans:
            for e in tap_events(tap, p['data']): print(f"{p['ts'] - t0:9.6f} {e}")
        else:
            print(f"{p['ts'] - t0:9.6f} {decode(p['data'])}")
