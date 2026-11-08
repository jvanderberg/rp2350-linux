#!/usr/bin/env python3
"""List every reference from core1's SRAM code into XIP flash.

    scripts/check-core1.py [ELF]      (default: rvemu-pico-shell, release)

core1 runs only what core1.x links into `.core1_text` (SRAM). A direct call
or jump from there into flash (0x10000000-0x13ffffff), or a pc-relative or
absolute address formed there that points into flash (rodata), means core1
fetches through the XIP cache it shares with the guest. Each reference is
printed with the function it is in and what it reaches, grouped by target.
Flash's uncached alias (0x14000000) and SRAM are fine and not listed.

Cold paths (panics, formatting for RTT, the radio firmware upload, BOOTSEL)
may stay in flash; anything a poll pass runs must not. Exit status 1 if any
reference is not to a target matched by COLD below.
"""
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ELF = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "target/riscv32imac-unknown-none-elf/release/rvemu-pico-shell"
SYSROOT = subprocess.check_output(["rustc", "--print", "sysroot"], text=True).strip()
HOST = next(line[6:] for line in subprocess.check_output(["rustc", "-vV"], text=True).splitlines() if line.startswith("host: "))
OBJDUMP = Path(SYSROOT) / "lib/rustlib" / HOST / "bin/llvm-objdump"
if not OBJDUMP.is_file():
    sys.exit("Missing llvm-objdump. Run: rustup component add llvm-tools-preview")
NM = OBJDUMP.with_name("llvm-nm")

# Targets that only run on rare or one-time paths (BYTES: the radio firmware,
# read once at bring-up). A reference made only from cold functions is cold
# too.
COLD = re.compile(
    r"panic|unwrap_failed|expect_failed|fmt|rtt_target|_print|Debug|Display|Formatter"
    r"|reboot|bootsel|slice_error|str_index|index_order|slice_end_index|slice_start_index"
    r"|len_mismatch|assert_failed|unreachable|from_utf8|escape_debug|Lanon|::fail|index_fail"
    r"|write_blocking|::BYTES"
)


def flash(a):
    # the firmware's part of flash (the guest's parts are read through the
    # uncached alias or by core0)
    return 0x1000_0000 <= a < 0x1010_0000


def main():
    syms = []
    for line in subprocess.run([NM, "-C", "-n", ELF], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split(None, 2)
        if len(p) == 3:
            syms.append((int(p[0], 16), p[2]))

    def name_at(a):
        best = None
        for v, n in syms:
            if v > a:
                break
            best = n
        return best or "?"

    dis = subprocess.run([OBJDUMP, "-d", "-C", "--no-show-raw-insn", "-j", ".core1_text", ELF],
                         capture_output=True, text=True, check=True).stdout
    refs = defaultdict(set)  # target -> {function}
    func = "?"
    regs = {}  # register -> value formed by auipc/lui
    ins = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
    for line in dis.splitlines():
        m = re.match(r"^[0-9a-f]+ <(.*)>:$", line)
        if m:
            func, regs = m.group(1), {}
            continue
        m = ins.match(line)
        if not m:
            continue
        pc, op, args = int(m.group(1), 16), m.group(2), m.group(3)
        a = [x.strip() for x in args.split("<")[0].split(",")]
        t = re.search(r"<(.*)>", args)
        if op in ("jal", "j", "call", "tail", "jalr") and t:
            # llvm-objdump resolves jal and auipc+jalr targets
            tm = re.search(r"(0x[0-9a-f]+)", args.split("<")[0][::-1][::-1])
            tgt = t.group(1).split("+")[0]
            addr = next((v for v, n in syms if n == tgt), None)
            if addr is not None and flash(addr):
                refs[f"call {tgt}"].add(func)
            continue
        if op == "auipc" and len(a) == 2:
            regs[a[0]] = (pc + (int(a[1], 0) << 12)) & 0xFFFF_FFFF
            continue
        if op == "lui" and len(a) == 2:
            regs[a[0]] = (int(a[1], 0) << 12) & 0xFFFF_FFFF
            continue
        if op == "addi" and len(a) == 3 and a[1] in regs:
            v = (regs[a[1]] + int(a[2], 0)) & 0xFFFF_FFFF
            if flash(v):
                refs[f"addr {name_at(v)} ({v:#x})"].add(func)
            regs.pop(a[0], None)
            continue
        mm = re.match(r"(-?(?:0x)?[0-9a-f]+)\((\w+)\)", a[-1]) if a else None
        if mm and mm.group(2) in regs:
            v = (regs[mm.group(2)] + int(mm.group(1), 0)) & 0xFFFF_FFFF
            if flash(v):
                refs[f"{op} {name_at(v)} ({v:#x})"].add(func)
        if a:
            regs.pop(a[0], None)

    hot = 0
    for tgt in sorted(refs):
        cold = bool(COLD.search(tgt)) or all(COLD.search(f) for f in refs[tgt])
        hot += not cold
        print(f"{'cold' if cold else 'HOT '}  {tgt[:110]}")
        for f in sorted(refs[tgt])[: 4 if not cold else 1]:
            print(f"        from {f[:120]}")
    print(f"check-core1: {len(refs)} flash targets, {hot} not known to be cold")
    sys.exit(1 if hot else 0)


main()
