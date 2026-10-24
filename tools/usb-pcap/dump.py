import sys
from pcap import usb
dev=int(sys.argv[2]); lim=int(sys.argv[3]) if len(sys.argv)>3 else 10**9
t0=None;n=0
for p in usb(sys.argv[1]):
    if p['dev']!=dev: continue
    if p['ep']==0x84 and not p['data'] and '-a' not in sys.argv: continue
    t0=t0 or p['ts']
    s=p['setup'].hex() if p['xfer']==2 and p['type']=='S' else ''
    print(f"{p['ts']-t0:10.6f} {p['type']} ep{p['ep']:02x} x{p['xfer']} st={p['status']} len={p['len']} {s} {p['data'][:64].hex()}")
    n+=1
    if n>=lim: break
