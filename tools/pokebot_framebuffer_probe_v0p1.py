#!/usr/bin/env python3
"""Pull one Pokebot-CFW top-screen snapshot and save it as PNG."""
from __future__ import annotations
import argparse, binascii, secrets, socket, struct, sys, time, zlib
from pathlib import Path

PORT=4952
REQ_MAGIC=0x5242524F
RESP_MAGIC=0x5342524F
VERSION=1
BEGIN,READ,END=11,12,13
OK,BAD_COMMAND=0,3
REQ=struct.Struct('<IHHIII')
RESP=struct.Struct('<IHHIIiI')
INFO=struct.Struct('<IIIIIIII')


def fnv1a32(data):
    h=0x811C9DC5
    for b in data:
        h^=b
        h=(h*0x01000193)&0xFFFFFFFF
    return h


def chunk(kind,data):
    p=kind+data
    return struct.pack('>I',len(data))+p+struct.pack('>I',binascii.crc32(p)&0xFFFFFFFF)


def write_png(path,bgr,w,h,bottom_up=True):
    rows=range(h-1,-1,-1) if bottom_up else range(h)
    scan=bytearray(); rb=w*3
    for y in rows:
        row=bgr[y*rb:(y+1)*rb]; scan.append(0)
        for x in range(0,rb,3):
            b,g,r=row[x:x+3]; scan.extend((r,g,b))
    ihdr=struct.pack('>IIBBBBB',w,h,8,2,0,0,0)
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',ihdr)+chunk(b'IDAT',zlib.compress(bytes(scan),6))+chunk(b'IEND',b''))


class Bridge:
    def __init__(self,host,port=PORT,timeout=1.0):
        self.remote=(host,port); self.s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); self.s.settimeout(timeout)
        self.rid=secrets.randbelow(0x7FFFFFFE)+1
    def next(self):
        self.rid=(self.rid+1)&0x7FFFFFFF or 1; return self.rid
    def request(self,cmd,arg=0,aux=0,retries=4,rid=None):
        rid=rid or self.next(); pkt=REQ.pack(REQ_MAGIC,VERSION,cmd,rid,arg&0xFFFFFFFF,aux&0xFFFFFFFF)
        for _ in range(retries):
            self.s.sendto(pkt,self.remote)
            try:
                while True:
                    raw,_=self.s.recvfrom(4096)
                    if len(raw)<RESP.size: continue
                    magic,ver,status,got,echo,res,n=RESP.unpack_from(raw)
                    if magic!=RESP_MAGIC or ver!=VERSION or got!=rid: continue
                    payload=raw[RESP.size:]
                    if len(payload)==n: return status,res,payload
            except socket.timeout: pass
        raise RuntimeError(f'UDP timeout command={cmd}')
    def close(self): self.s.close()


def self_test():
    raw=bytes([255,0,0,255,255,255,0,0,255,0,255,0])
    if fnv1a32(raw)!=0x03D1B4C1: raise RuntimeError('FNV self-test failed')
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        p=Path(td)/'x.png'; write_png(p,raw,2,2,True)
        if not p.read_bytes().startswith(b'\x89PNG'): raise RuntimeError('PNG self-test failed')
    print('framebuffer probe self-test: PASS')


def capture(host,out,port,timeout):
    b=Bridge(host,port,timeout); capture_id=None; start=time.monotonic(); begin_id=b.next()
    try:
        status,res,payload=b.request(BEGIN,rid=begin_id)
        if status==BAD_COMMAND: raise RuntimeError('This boot.firm does not support framebuffer commands 11-13')
        if status!=OK: raise RuntimeError(f'FRAMEBUFFER_BEGIN status={status} result=0x{res&0xFFFFFFFF:08X}')
        if len(payload)!=INFO.size: raise RuntimeError(f'bad info length {len(payload)}')
        capture_id,w,h,pix,total,max_chunk,flags,expected=INFO.unpack(payload)
        if pix!=1 or total!=w*h*3: raise RuntimeError(f'unsupported geometry/format: {w}x{h} format={pix} total={total}')
        print(f'Snapshot #{capture_id}: {w}x{h}, {total:,} bytes, checksum=0x{expected:08X}')
        data=bytearray(); off=0; report=10
        while off<total:
            status,res,p=b.request(READ,capture_id,off)
            if status!=OK: raise RuntimeError(f'FRAMEBUFFER_READ offset={off} status={status} result=0x{res&0xFFFFFFFF:08X}')
            need=min(max_chunk,total-off)
            if len(p)!=need: raise RuntimeError(f'short chunk at {off}: {len(p)} != {need}')
            data.extend(p); off+=len(p); pct=off*100//total
            if pct>=report: print(f'  transfer {pct}%'); report+=10
        got=fnv1a32(data)
        if got!=expected: raise RuntimeError(f'checksum mismatch CFW=0x{expected:08X} PC=0x{got:08X}')
        out=Path(out).resolve(); write_png(out,bytes(data),w,h,bool(flags&1))
        print(f'PASS: {out} ({time.monotonic()-start:.2f}s)')
    finally:
        if capture_id:
            try: b.request(END,capture_id,0,retries=2)
            except Exception as e: print(f'warning: END failed: {e}',file=sys.stderr)
        b.close()


def main():
    ap=argparse.ArgumentParser(); ap.add_argument('host',nargs='?'); ap.add_argument('output',nargs='?',default='pokebot_top_screen.png')
    ap.add_argument('--port',type=int,default=PORT); ap.add_argument('--timeout',type=float,default=1.0); ap.add_argument('--self-test',action='store_true')
    a=ap.parse_args()
    if a.self_test: self_test(); return 0
    if not a.host: ap.error('host required')
    try: capture(a.host,a.output,a.port,a.timeout); return 0
    except Exception as e: print(f'FAIL: {type(e).__name__}: {e}',file=sys.stderr); return 1

if __name__=='__main__': raise SystemExit(main())
