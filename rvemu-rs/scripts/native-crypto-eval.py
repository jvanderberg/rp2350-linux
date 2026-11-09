#!/usr/bin/env python3
"""Protocol-independent native crypto vectors in RV32 translator/interpreter lockstep.
No Linux or SSH is required. Uses existing pv-pages QEMU/assembler helpers.
"""
import importlib.util
import json
import re
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('pages',ROOT/'scripts/pv-pages-eval.py')
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)
OUT=ROOT/'target/native-crypto-eval';OUT.mkdir(parents=True,exist_ok=True);p.OUT=OUT

def call(args): subprocess.run(args,cwd=ROOT,check=True)
def transform(op,src,params):
    (OUT/'input').write_bytes(src);(OUT/'params').write_bytes(params)
    call([str(ROOT/'target/release/examples/transform'),str(op),str(OUT/'input'),str(OUT/'output'),str(OUT/'params')])
    return (OUT/'output').read_bytes()
def case(name,op,src,params,expected,err=0,cap=None):
    cap=len(expected) if cap is None else cap
    a=p.Asm()
    a.li(18,3);a.label("repeat")
    for reg,val in [(17,0x0a000001),(16,op),(10,0x300000),(11,cap),(12,0x200000),(13,len(src)),(14,0x400000),(15,len(params))]:a.li(reg,val)
    a.ecall();a.li(5,err);a.branch(10,5,'fail')
    if not err:a.li(5,len(expected));a.branch(11,5,'fail')
    # Expected bytes include unchanged sentinel bytes for negative tests.
    if expected:
        a.li(5,0x300000);a.li(6,0x500000);a.li(7,len(expected));a.label('check')
        a.emit(5<<15|4<<12|28<<7|3);a.emit(6<<15|4<<12|29<<7|3) # lbu
        a.branch(28,29,'fail');a.addi(5,5,1);a.addi(6,6,1);a.addi(7,7,-1);a.branch(7,0,'check')
    a.addi(18,18,-1);a.branch(18,0,'repeat')
    a.message('PV PASS\n');a.li(17,8);a.ecall()
    a.label('fail');a.message('PV FAIL\n');a.li(17,8);a.ecall()
    image=bytearray(0x510000);code=a.finish();image[:len(code)]=code
    for addr,data in [(0x200000,src),(0x300000,b'\xa5'*cap),(0x400000,params),(0x500000,expected)]:image[addr:addr+len(data)]=data
    row=p.run(name,image,2)
    log=(OUT/(name+'.log')).read_text()
    peaks=json.loads(re.search(r'native operation stack peaks Some\((\[.*?\])\)',log)[1])
    peak=peaks[op]
    assert 0 < peak <= 20*1024, (name, peak, "24 KiB native stack must retain at least 4 KiB headroom")
    row['native_stack_bytes']=peak
    row['operation']=op
    return row

if __name__=='__main__':
    call(['cargo','build','--release','-p','rvaccel','--example','transform'])
    # Enabling only rvcore must fail closed on firmware with no reserved workspace.
    call(['cargo','build','--release','-p','rvemu-virt','--target','riscv32imac-unknown-none-elf','--features','rvcore/pv-buffers'])
    a=p.Asm();a.li(17,0x10);a.li(16,3);a.li(10,0x0a000001);a.ecall()
    a.branch(10,0,'fail');a.branch(11,0,'fail')
    a.li(17,0x0a000001);a.li(16,6);a.ecall();a.li(5,-2);a.branch(10,5,'fail')
    a.message('PV PASS\n');a.li(17,8);a.ecall()
    a.label('fail');a.message('PV FAIL\n');a.li(17,8);a.ecall()
    unprovisioned=p.run('native-workspace-required',a.finish(),2)
    call(['cargo','build','--release','-p','rvemu-virt','--target','riscv32imac-unknown-none-elf','--features','pv-buffers'])
    seed=bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')
    pk=bytes.fromhex('d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')
    sig=bytes.fromhex('e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b')
    xsecret=bytes.fromhex('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a')
    xpub=bytes.fromhex('8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a')
    rows=[case('x25519-rfc7748',6,b'\x09'+bytes(31),xsecret,xpub),
          case('ed25519-rfc8032-public',7,b'',seed,pk),case('ed25519-rfc8032-sign',8,b'',seed,sig),
          case('ed25519-rfc8032-verify',9,b'',pk+sig,b''),
          case('x25519-low-order',6,bytes(32),xsecret,b'\xa5'*32,err=-1),
          case('ed25519-bad-signature',9,b'changed',pk+sig,b'\xa5'*32,err=-1)]
    kp=transform(10,b'',bytes([17])*32);enc=transform(11,kp[:1158],bytes([23])*32)
    rows += [case('sntrup761-keypair',10,b'',bytes([17])*32,kp),
             case('sntrup761-encap',11,kp[:1158],bytes([23])*32,enc)]
    for i in [0,1]:
        def fixture(name):return bytes.fromhex((ROOT/f'rvaccel/tests/data/sntrup761-kat{i}-{name}.hex').read_text())
        rows.append(case(f'sntrup761-ietf-decap-{i}',12,fixture('ct'),fixture('sk'),fixture('ss')))
    bad=bytes([enc[0]^1])+enc[1:1039];rejected=transform(12,bad,kp[1158:]);assert rejected!=enc[1039:]
    rows.append(case('sntrup761-implicit-rejection',12,bad,kp[1158:],rejected))
    # Exercise all operations, largest legal messages, error exits, and repeated
    # reuse of the same per-CPU workspace in both translator and interpreter.
    plain=bytes(i%251 for i in range(32752))
    params=bytes(range(32))+bytes(range(12))+bytes(1024)
    wire=transform(2,plain,params)
    rows += [case('aead-seal-max',2,plain,params,wire),case('aead-open-max',3,wire,params,plain)]
    bad=wire[:-1]+bytes([wire[-1]^1])
    rows.append(case('aead-bad-tag-max',3,bad,params,bytes(len(plain)),err=-1))
    packet=(32748).to_bytes(4,'big')+plain[:32748]
    sshparams=bytes(range(64))+(7).to_bytes(8,'big')
    sshwire=transform(4,packet,sshparams)
    rows += [case('ssh-seal-max',4,packet,sshparams,sshwire),case('ssh-open-max',5,sshwire,sshparams,packet)]
    compressible=bytes(i%31 for i in range(29770))
    packed=transform(1,compressible,b'')
    rows += [case('lz4-encode-max-bound',1,compressible,b'',packed,cap=32768),case('lz4-decode-large',0,packed,b'',compressible)]
    extension=32768-25
    block=b'\x1f\x00\x01\x00'+b'\xff'*(extension//255)+bytes([extension%255])+b'\x50'+bytes(5)
    rows.append(case('lz4-decode-max',0,block,b'',bytes(32768)))
    message=bytes(i%251 for i in range(32768));signature=transform(8,message,seed)
    rows += [case('ed25519-sign-max',8,message,seed,signature),case('ed25519-verify-max',9,message,pk+signature,b'')]
    assert {row['operation'] for row in rows} == set(range(13))
    rows.append(unprovisioned)
    rows.append(p.run('jittest-native-crypto' ,(ROOT/'jittest-guest/jittest.bin').read_bytes(),2,'signature 56c7e73f'))
    (OUT/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
