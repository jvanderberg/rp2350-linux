#!/usr/bin/env python3
"""Synthetic RV32 page loops vs experimental SBI; no Linux image required.
Requires rustup target add riscv32imac-unknown-none-elf and QEMU.
Produces target/pv-pages-eval/{results.json,*.log,*.bin}. Host instruction
counts under QEMU -icount are NOT RP2350 cycles or Linux speedups.
"""
import json
import os
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'target/pv-pages-eval'
EXT = 0x0a000000
DST, SRC = 0x200000, 0x400000

class Asm:
    def __init__(self):
        self.words, self.labels, self.fixups = [], {}, []
    def emit(self, w): self.words.append(w & 0xffffffff)
    def label(self, s): self.labels[s] = 4 * len(self.words)
    def li(self, r, v):
        self.emit((((v + 0x800) >> 12) << 12) | r << 7 | 0x37)
        self.addi(r, r, v)
    def addi(self, d, s, n): self.emit((n & 4095) << 20 | s << 15 | d << 7 | 0x13)
    def lw(self, d, s, n=0): self.emit((n & 4095) << 20 | s << 15 | 2 << 12 | d << 7 | 3)
    def sw(self, s, d, n=0): self.emit((n >> 5) << 25 | s << 20 | d << 15 | 2 << 12 | (n & 31) << 7 | 0x23)
    def branch(self, a, b, label, f3=1):
        self.fixups.append((len(self.words), a, b, label, f3)); self.emit(0)
    def ecall(self): self.emit(0x73)
    def message(self, s):
        self.li(17, 1)
        for c in s.encode(): self.addi(10, 0, c); self.ecall()
    def finish(self):
        for i, a, b, label, f3 in self.fixups:
            off = self.labels[label] - i * 4
            assert -4096 <= off < 4096 and off % 2 == 0
            self.words[i] = ((off >> 12 & 1) << 31 | (off >> 5 & 63) << 25 |
                b << 20 | a << 15 | f3 << 12 | (off >> 1 & 15) << 8 | (off >> 11 & 1) << 7 | 0x63)
        return struct.pack('<' + 'I' * len(self.words), *self.words)

def guest(op, native, streaming, repeats=256):
    a = Asm()
    a.li(18, repeats); a.li(19, DST); a.li(20, SRC)
    a.label('page')
    a.addi(10, 19, 0); a.addi(11, 20, 0)
    if native:
        a.li(17, EXT); a.addi(16, 0, int(op == 'copy')); a.ecall()
        a.branch(10, 0, 'fail')
    else:
        a.addi(12, 0, 64)
        a.label('word')
        # 16-word unrolling avoids exaggerating baseline loop overhead.
        for i in range(16):
            if op == 'copy': a.lw(5, 11, i * 4)
            a.sw(5 if op == 'copy' else 0, 10, i * 4)
        a.addi(10, 10, 64)
        if op == 'copy': a.addi(11, 11, 64)
        a.addi(12, 12, -1); a.branch(12, 0, 'word')
    if streaming:
        a.li(6, 4096)
        a.emit(6 << 20 | 19 << 15 | 19 << 7 | 0x33)  # add s3,s3,t1
        a.emit(6 << 20 | 20 << 15 | 20 << 7 | 0x33)
    a.addi(18, 18, -1); a.branch(18, 0, 'page')
    # Sample every destination page, plus fully check the last one. These
    # identical checks are included in counts, making savings conservative.
    a.li(10, DST); a.li(12, repeats if streaming else 1)
    a.li(6, 4096); a.li(7, 0x12345678 if op == 'copy' else 0)
    a.label('check_pages'); a.lw(5, 10); a.branch(5, 7, 'fail')
    a.emit(6 << 20 | 10 << 15 | 10 << 7 | 0x33)
    a.addi(12, 12, -1); a.branch(12, 0, 'check_pages')
    a.li(10, DST + (repeats - 1 if streaming else 0) * 4096); a.li(12, 1024)
    a.label('check_words'); a.lw(5, 10); a.branch(5, 7, 'fail')
    a.addi(10, 10, 4); a.addi(12, 12, -1); a.branch(12, 0, 'check_words')
    a.message('PV PASS\n'); a.li(17, 8); a.ecall()
    a.label('fail'); a.message('PV FAIL\n'); a.li(17, 8); a.ecall()
    image = bytearray(SRC + 4096 * repeats)
    code = a.finish(); image[:len(code)] = code
    image[DST:DST+4096*repeats] = b'\xa5' * (4096*repeats)
    image[SRC:] = struct.pack('<I', 0x12345678) * (1024*repeats)
    return image

def smc_guest():
    a = Asm()
    a.li(5, DST); a.emit(5 << 15 | 1 << 7 | 0x67)  # jalr ra,t0
    a.addi(6, 0, 42); a.branch(12, 6, 'fail')
    a.li(10, DST); a.li(11, SRC); a.li(17, EXT); a.addi(16, 0, 1); a.ecall()
    a.branch(10, 0, 'fail')
    a.li(5, DST); a.emit(5 << 15 | 1 << 7 | 0x67)
    a.addi(6, 0, 99); a.branch(12, 6, 'fail')
    a.message('PV PASS\n'); a.li(17, 8); a.ecall()
    a.label('fail'); a.message('PV FAIL\n'); a.li(17, 8); a.ecall()
    image = bytearray(SRC + 4096)
    code = a.finish(); image[:len(code)] = code
    for addr, value in ((DST, 42), (SRC, 99)):
        image[addr:addr+8] = struct.pack('<II', value << 20 | 12 << 7 | 0x13, 0x8067)
    return image

def run(name, image, mode, expected="PV PASS", max_millions=10):
    binary = OUT / (name + '.bin'); binary.write_bytes(image)
    info = OUT / 'info.bin'
    info.write_bytes(struct.pack('<IIIII60sI', 0x4d455652, mode, 8 << 20, max_millions, 1, b'', 0))
    command = ['qemu-system-riscv32', '-machine', 'virt', '-bios', 'none', '-m', '256M',
        '-nographic', '-monitor', 'none', '-serial', 'stdio', '-icount', 'shift=0,align=off,sleep=off',
        '-kernel', str(ROOT/'target/riscv32imac-unknown-none-elf/release/rvemu-virt'),
        '-device', f'loader,file={binary},addr=0x81000000,force-raw=on',
        '-device', f'loader,file={info},addr=0x80f00000,force-raw=on']
    p = subprocess.run(command, capture_output=True, text=True, timeout=60, check=True)
    log = p.stdout + p.stderr; (OUT / (name + '.log')).write_text(log)
    assert expected in log and 'PV FAIL' not in log and 'SELFCHECK FAIL' not in log and 'Some(Shutdown)' in log, log
    if mode == 2: assert 'all matched' in log, log
    result = dict(name=name, mode=mode,
        guest_instructions=int(re.search(r'guest instructions (\d+)', log)[1]),
        host_instructions=int(re.search(r'host instructions (\d+)', log)[1]))
    print(json.dumps(result), flush=True)
    return result

if __name__ == '__main__':
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cargo', 'build', '--release', '-p', 'rvemu-virt', '--target',
        'riscv32imac-unknown-none-elf', '--features', 'rvcore/pv-pages'], cwd=ROOT, check=True,
        env={**os.environ, 'RVEMU_CODE_KB':'326', 'RVEMU_REGIONS':'8', 'RVEMU_NBUCKETS':'1024', 'RVEMU_NJC':'1024'})
    rows = []
    for op in ('zero', 'copy'):
        for stream in (False, True):
            for native in (False, True):
                name = f'{op}-{"stream" if stream else "hot"}-{"native" if native else "loop"}'
                rows.append(run(name, guest(op,native,stream), 1))
        rows.append(run(op+'-lockstep', guest(op,True,True,4), 2))
    rows.append(run('copy-executable-page-lockstep', smc_guest(), 2))
    rows.append(run('jittest-regression-lockstep', (ROOT/'jittest-guest/jittest.bin').read_bytes(), 2, 'signature 56c7e73f'))
    (OUT/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
