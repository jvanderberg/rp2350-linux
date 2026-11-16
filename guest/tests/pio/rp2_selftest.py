# MicroPython's rp2 on a real block (the board, or the Mac emulator's -G),
# as pio-selftest.c checks libpio: one block drives, PIO2 (state machines
# 8-11, at GPIOBASE 0) reads the pad back. No wiring; uses GP20, GP21, GP33.
#
#   micropython rp2_selftest.py
#
# Prints PASS/FAIL per check and "rp2-selftest: N of M passed".
from array import array
import rp2

checks = 0
fails = 0


def check(ok, what, detail=""):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
    print("PASS" if ok else "FAIL", what + (": " + detail if detail else ""))


@rp2.asm_pio()
def idle():
    nop()


# rp2 claims the pins a program declares (set_init, out_init), as the rp2
# port gives them to PIO
@rp2.asm_pio(set_init=rp2.PIO.OUT_LOW)
def setter():
    nop()


@rp2.asm_pio(out_init=rp2.PIO.OUT_LOW)
def outer():
    nop()


def pad(sm):
    """GPIO in_base's level, as the state machine sees it."""
    while sm.rx_fifo():
        sm.get()
    sm.exec("in_(pins, 1)")
    sm.exec("push(noblock)")
    return 1 if sm.rx_fifo() and sm.get() else 0


reader = rp2.StateMachine(8, idle, in_base=21)

# PIO1, on GPIOBASE 16 (its jmp pin is GP33), drives GP21 by SET
sm = rp2.StateMachine(4, setter, set_base=21, jmp_pin=33)
sm.exec("set(pindirs, 1)")
sm.exec("set(pins, 1)")
hi = pad(reader)
sm.exec("set(pins, 0)")
lo = pad(reader)
check(hi == 1 and lo == 0, "StateMachine(4) at GPIOBASE 16 drives GP21", "read %d then %d" % (hi, lo))

# MOV PINDIRS (RP2350): the documentation pass found rp2 assembling it as
# MOV EXEC, which ran ~null as an instruction instead
sm.exec("set(pindirs, 0)")
sm2 = rp2.StateMachine(5, outer, out_base=21, jmp_pin=33)
# out_init made GP21 an output: make it an input again (by OUT, which the
# bug didn't touch), so only a working MOV PINDIRS can make it an output
sm2.exec("mov(osr, null)")
sm2.exec("out(pindirs, 1)")
sm2.exec("mov(pindirs, invert(null))")
sm2.exec("mov(pins, invert(null))")
hi = pad(reader)
sm2.exec("mov(pins, null)")
lo = pad(reader)
check(hi == 1 and lo == 0, "mov(pindirs, invert(null)) makes GP21 an output", "read %d then %d" % (hi, lo))

# a machine can't move GPIOBASE under another one's pins
try:
    rp2.StateMachine(6, setter, set_base=3)
    moved = True
except ValueError:
    moved = False
check(not moved, "StateMachine(6) on GP3 can't move PIO1 off GPIOBASE 16")

# DMA through a state machine: each word back inverted
@rp2.asm_pio()
def echo():
    wrap_target()
    pull()
    mov(isr, invert(osr))
    push()
    wrap()


e = rp2.StateMachine(9, echo)
e.active(1)
src = array("I", [(0x9E3779B9 * (i + 1)) & 0xFFFFFFFF for i in range(256)])
dst = array("I", bytes(4 * 256))
rx = rp2.DMA()
tx = rp2.DMA()
rx.config(read=e, write=dst, count=256, ctrl=rx.pack_ctrl(size=2), trigger=True)
tx.config(read=src, write=e, count=256, ctrl=tx.pack_ctrl(size=2), trigger=True)
tx.wait(2000)
rx.wait(2000)
wrong = sum(1 for i in range(256) if dst[i] != ~src[i] & 0xFFFFFFFF)
check(wrong == 0, "rp2.DMA echo through StateMachine(9)", "%d of 256 wrong" % wrong)
rx.close()
tx.close()
e.active(0)

print("rp2-selftest: %d of %d passed" % (checks - fails, checks))

import sys

sys.exit(1 if fails else 0)
