#!/usr/bin/env python3
"""Compare identical LZ4/AEAD code translated vs native under QEMU.
Requires QEMU, Rust RV32 targets, and liblz4 for independent interoperability.
All keys/nonces are public synthetic test data. Repetition measures the same
operation; this benchmark is not an example of nonce management for live data.
"""
import ctypes as C
import ctypes.util
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('pages',ROOT/'scripts/pv-pages-eval.py')
pages=importlib.util.module_from_spec(spec); spec.loader.exec_module(pages)
OUT=ROOT/'target/pv-buffers-eval'; OUT.mkdir(parents=True,exist_ok=True); pages.OUT=OUT

def command(args, cwd=ROOT): subprocess.run(args,cwd=cwd,check=True)
def elf_image(path):
    data=path.read_bytes(); assert data[:5]==b'\x7fELF\x01'
    phoff=struct.unpack_from('<I',data,28)[0]
    entsize,num=struct.unpack_from('<HH',data,42)
    image=bytearray(0x500000+32768)
    for i in range(num):
        kind,off,va,pa,filesz,memsz,flags,align=struct.unpack_from('<8I',data,phoff+i*entsize)
        if kind==1:
            assert va+memsz<0x100000
            image[va:va+filesz]=data[off:off+filesz]
    return image

def elf_sizes(path):
    data=path.read_bytes()
    shoff=struct.unpack_from('<I',data,32)[0]
    entsize,num=struct.unpack_from('<HH',data,46)
    sizes=dict(executable=0,readonly=0,writable=0,bss=0)
    for i in range(num):
        _,kind,flags,_,_,size,_,_,_,_=struct.unpack_from('<10I',data,shoff+i*entsize)
        if flags&2:
            category='bss' if kind==8 else 'executable' if flags&4 else 'writable' if flags&1 else 'readonly'
            sizes[category]+=size
    return sizes

def transform(fid,src,params):
    (OUT/'input').write_bytes(src); (OUT/'params').write_bytes(params)
    command([str(ROOT/'target/release/examples/transform'),str(fid),str(OUT/'input'),str(OUT/'output'),str(OUT/'params')])
    return (OUT/'output').read_bytes()

def reference_lz4():
    name=ctypes.util.find_library('lz4')
    if not name: raise RuntimeError('liblz4 is required for independent LZ4 interoperability checks')
    lib=C.CDLL(name)
    for fn in (lib.LZ4_compress_default,lib.LZ4_decompress_safe):
        fn.argtypes=[C.c_void_p,C.c_void_p,C.c_int,C.c_int]; fn.restype=C.c_int
    return lib

def run_case(base,name,fid,src,expected,params=b'',cap=None,repeats=16,err=0):
    cap=cap if cap is not None else max(len(expected),1)
    rows=[]
    for native,mode,suffix,count in [(False,1,'translated',repeats),(True,1,'native',repeats),(True,2,'lockstep',1)]:
        image=base.copy()
        image[0x100000:0x100020]=struct.pack('<8I',fid,len(src),cap,len(params),count,int(native),len(expected),err & 0xffffffff)
        for addr,blob in [(0x200000,src),(0x400000,params),(0x500000,expected)]: image[addr:addr+len(blob)]=blob
        image[0x300000:0x300000+cap]=b'\xa5'*cap
        row=pages.run(name+'-'+suffix,image,mode,max_millions=200)
        row.update(fid=fid,input_bytes=len(src),output_bytes=len(expected),repeats=count)
        rows.append(row)
    return rows

def smc_case(lib, fail=False):
    a=pages.Asm()
    for target in (0x200000,0x201000):
        a.li(5,target); a.emit(5<<15|1<<7|0x67)
        a.addi(6,0,42); a.branch(12,6,'fail')
    replacement=bytearray(8192)
    for offset in (0,4096):
        replacement[offset:offset+8]=struct.pack('<II',99<<20|12<<7|0x13,0x8067)
    if fail:
        # Copy 8192 literal bytes, then fail on an invalid zero match offset.
        length=8192-15
        packed=b'\xf0'+b'\xff'*(length//255)+bytes([length%255])+replacement+b'\0\0'+b'\0'*8
    else:
        buf=C.create_string_buffer(16384)
        n=lib.LZ4_compress_default(bytes(replacement),buf,8192,len(buf)); assert n>0
        packed=buf.raw[:n]
    a.li(17,0x0a000001);a.addi(16,0,0)
    for r,v in [(10,0x200000),(11,12288),(12,0x400000),(13,len(packed)),(14,0),(15,0)]: a.li(r,v)
    a.ecall();a.addi(6,0,-1 if fail else 0);a.branch(10,6,'fail')
    for target in (0x200000,0x201000):
        a.li(5,target);a.emit(5<<15|1<<7|0x67)
        a.addi(6,0,99);a.branch(12,6,'fail')
    a.message('PV PASS\n');a.li(17,8);a.ecall()
    a.label('fail');a.message('PV FAIL\n');a.li(17,8);a.ecall()
    image=bytearray(0x500000)
    code=a.finish();image[:len(code)]=code
    for target in (0x200000,0x201000):image[target:target+8]=struct.pack('<II',42<<20|12<<7|0x13,0x8067)
    image[0x400000:0x400000+len(packed)]=packed
    return pages.run('lz4-multipage-smc-'+('failure' if fail else 'success'),image,2)

if __name__=='__main__':
    command(['cargo','build','--release','-p','rvaccel','--example','transform'])
    command(['cargo','build','--release'], ROOT/'pvbench-guest')
    env={**os.environ,'RVEMU_CODE_KB':'326','RVEMU_REGIONS':'8','RVEMU_NBUCKETS':'1024','RVEMU_NJC':'1024'}
    elf=ROOT/'target/riscv32imac-unknown-none-elf/release/rvemu-virt'
    subprocess.run(['cargo','build','--release','-p','rvemu-virt','--target','riscv32imac-unknown-none-elf',
        '--features','rvcore/pv-pages'],cwd=ROOT,env=env,check=True)
    footprint={'pages_only':elf_sizes(elf)}
    subprocess.run(['cargo','build','--release','-p','rvemu-virt','--target','riscv32imac-unknown-none-elf',
        '--features','pv-buffers,rvcore/pv-pages'],cwd=ROOT,env=env,check=True)
    footprint['pages_and_buffers']=elf_sizes(elf)
    (OUT/'footprint.json').write_text(json.dumps(footprint,indent=2)+'\n')
    base=elf_image(ROOT/'pvbench-guest/target/riscv32im-unknown-none-elf/release/pvbench-guest')
    lib=reference_lz4(); rows=[]
    for n in (4096,16384):
        seed=12345678; random=bytearray()
        for _ in range(n):
            seed ^= (seed<<13)&0xffffffff; seed ^= seed>>17; seed ^= (seed<<5)&0xffffffff
            random.append(seed&255)
        for kind,plain in [('repeat',(b'kernel data page '*((n+15)//16))[:n]),('random',bytes(random))]:
            assert len(plain)==n
            compressed=C.create_string_buffer(32768)
            k=lib.LZ4_compress_default(plain,compressed,len(plain),len(compressed)); assert k>0
            rows+=run_case(base,f'lz4-decode-{kind}-{n}',0,compressed.raw[:k],plain)
            encoded=transform(1,plain,b'')
            decoded=C.create_string_buffer(n)
            assert lib.LZ4_decompress_safe(encoded,decoded,len(encoded),n)==n
            assert decoded.raw==plain
            rows+=run_case(base,f'lz4-encode-{kind}-{n}',1,plain,encoded,cap=32768)
    params=bytes(range(32))+bytes(range(12))+b'authenticated header'
    for n in (64,1024,16384):
        plain=bytes(i%251 for i in range(n))
        ciphertext=transform(2,plain,params)
        rows+=run_case(base,f'aead-seal-{n}',2,plain,ciphertext,params)
        rows+=run_case(base,f'aead-open-{n}',3,ciphertext,plain,params)
    ssh_params=bytes(range(64))+(7).to_bytes(8,'big')
    ssh_plain=(16380).to_bytes(4,'big')+bytes(i%251 for i in range(16380))
    ssh_wire=transform(4,ssh_plain,ssh_params)
    rows+=run_case(base,'ssh-seal-16384',4,ssh_plain,ssh_wire,ssh_params)
    rows+=run_case(base,'ssh-open-16384',5,ssh_wire,ssh_plain,ssh_params)
    bad=bytearray(ciphertext); bad[-1]^=1
    rows+=run_case(base,'aead-bad-tag',3,bytes(bad),bytes(len(plain)),params,err=-1,repeats=1)
    # Invalid LZ4 must fail (only status tested; partial output is unspecified).
    rows+=run_case(base,'lz4-truncated',0,b'\xf0',b'',cap=4096,err=-1,repeats=1)
    rows.extend([smc_case(lib),smc_case(lib,True)])
    rows.append(pages.run('jittest-buffers-regression',(ROOT/'jittest-guest/jittest.bin').read_bytes(),2,'signature 56c7e73f'))
    (OUT/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
