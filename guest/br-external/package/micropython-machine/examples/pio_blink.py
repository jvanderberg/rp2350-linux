# A PIO program from MicroPython: a pin blinking, its timing all in PIO1
# (rp2 port style). micropython pio_blink.py [PIN]
# Each half period is 2112 cycles of the state machine's 100 kHz clock: about
# 23.7 Hz. (A state machine runs at clk_sys / 65536 at the slowest.)
import sys
import time
import rp2
from machine import Pin


@rp2.asm_pio(set_init=rp2.PIO.OUT_LOW)
def blink():
    wrap_target()
    set(pins, 1)    [31]
    set(x, 31)      [31]
    label("high")
    nop()           [31]
    jmp(x_dec, "high") [31]
    set(pins, 0)    [31]
    set(x, 31)      [31]
    label("low")
    nop()           [31]
    jmp(x_dec, "low") [31]
    wrap()


pin = int(sys.argv[1]) if len(sys.argv) > 1 else 14
sm = rp2.StateMachine(4, blink, freq=100_000, set_base=Pin(pin))
sm.active(1)
time.sleep(3)
sm.active(0)
