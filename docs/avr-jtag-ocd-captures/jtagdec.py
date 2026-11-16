"""Decode JTAG from a Saleae digital CSV. Channels: 0 TDO, 1 TCK, 2 RESET, 3 TDI, 4 TMS.
Prints IR/DR scans (LSB-first values), idle-clock counts and RESET edges."""
import csv, sys
TDO, TCK, RST, TDI, TMS = 0, 1, 2, 3, 4
NEXT = {  # state: (tms0, tms1)
 'RESET': ('IDLE', 'RESET'), 'IDLE': ('IDLE', 'SELDR'),
 'SELDR': ('CAPDR', 'SELIR'), 'CAPDR': ('SHDR', 'EX1DR'), 'SHDR': ('SHDR', 'EX1DR'),
 'EX1DR': ('PAUSEDR', 'UPDR'), 'PAUSEDR': ('PAUSEDR', 'EX2DR'), 'EX2DR': ('SHDR', 'UPDR'),
 'UPDR': ('IDLE', 'SELDR'),
 'SELIR': ('CAPIR', 'RESET'), 'CAPIR': ('SHIR', 'EX1IR'), 'SHIR': ('SHIR', 'EX1IR'),
 'EX1IR': ('PAUSEIR', 'UPIR'), 'PAUSEIR': ('PAUSEIR', 'EX2IR'), 'EX2IR': ('SHIR', 'UPIR'),
 'UPIR': ('IDLE', 'SELDR')}
rows = csv.reader(open(sys.argv[1])); next(rows)
st, prev, tin, tout, idle, tms1 = 'RESET', None, [], [], 0, 0
def val(b): return sum(v << i for i, v in enumerate(b))
def flush_idle():
    global idle, tms1
    if idle: print(f'{"":14}  idle x{idle}')
    if tms1: print(f'{"":14}  tms1 x{tms1} (reset path)')
    idle = tms1 = 0
for r in rows:
    t, s = float(r[0]), [int(x) for x in r[1:]]
    if prev is not None and s[RST] != prev[RST]:
        flush_idle(); print(f'{t:14.6f}  RESET -> {s[RST]}')
    if prev is not None and prev[TCK] == 0 and s[TCK] == 1:  # rising edge: sample
        if st in ('SHDR', 'SHIR'):
            tin.append(s[TDI]); tout.append(s[TDO])
        elif st == 'IDLE' and s[TMS] == 0: idle += 1
        elif st == 'RESET' and s[TMS] == 1: tms1 += 1
        nst = NEXT[st][s[TMS]]
        if st in ('SHDR', 'SHIR') and nst not in ('SHDR', 'SHIR'):
            flush_idle(); n = len(tin)
            kind = 'IR' if st == 'SHIR' else 'DR'
            print(f'{t:14.6f}  {kind}[{n:2}] in=0x{val(tin):0{(n+3)//4}X} out=0x{val(tout):0{(n+3)//4}X}')
            tin, tout = [], []
        st = nst
    prev = s
flush_idle()
