import struct, sys
def blocks(path):
    # Limit: timestamps assume the default if_tsresol (microseconds), as usbmon captures have.
    with open(path,'rb') as fh: f=fh.read()
    off=0; le='<'; ifaces=[]
    while off+12<=len(f):
        bt,bl=struct.unpack_from(le+'II',f,off)
        if bt==0x0A0D0D0A:
            bom=f[off+8:off+12]; le='<' if bom==b'\x4d\x3c\x2b\x1a' else '>'
            bt,bl=struct.unpack_from(le+'II',f,off)
        if bl<12 or off+bl>len(f): return  # truncated or corrupt block: stop instead of looping
        body=f[off+8:off+bl-4]
        if bt==1: ifaces.append(struct.unpack_from(le+'H',body,0)[0])
        elif bt==6:
            iid,th,tl,cap,orig=struct.unpack_from(le+'IIIII',body,0)
            yield ifaces[iid], ((th<<32)|tl)/1e6, body[20:20+cap]
        off+=bl
def usb(path):
    for lt,ts,d in blocks(path):
        if lt in (189,220):
            hl=48 if lt==189 else 64
            uid,typ,xfer,ep,dev,bus,fs,fd,sec,usec,st,ln,lc=struct.unpack_from('<QBBBBHbbqiiII',d,0)
            yield dict(ts=ts,type=chr(typ),xfer=xfer,ep=ep,dev=dev,bus=bus,status=st,len=ln,data=d[hl:hl+lc],setup=d[40:48],fs=fs)
        elif lt==249:
            hl,irp,st,fn,info,bus,dev,ep,xfer,dl=struct.unpack_from('<HQIHBHHBBI',d,0)
            yield dict(ts=ts,type='C' if info&1 else 'S',xfer=xfer,ep=ep,dev=dev,bus=bus,status=st,len=dl,data=d[hl:],setup=b'',fs=0)
        else:
            print('linktype',lt); return
if __name__=='__main__':
    import collections
    c=collections.Counter()
    for p in usb(sys.argv[1]): c[(p['bus'],p['dev'],p['ep'],p['xfer'])]+=1
    print(c)
