# PIO1's FIFOs from MicroPython, with no pins: a state machine sends back
# each word inverted. Words by put/get, then a block of them by DMA.
# micropython pio_echo.py
from array import array
import rp2


@rp2.asm_pio()
def echo():
    wrap_target()
    pull()
    mov(isr, invert(osr))
    push()
    wrap()


sm = rp2.StateMachine(4, echo)
sm.active(1)
bad = 0
for v in (0, 1, 0x12345678, 0xFFFFFFFF):
    sm.put(v)
    if sm.get() != ~v & 0xFFFFFFFF:
        bad += 1
print("put/get:", 4 - bad, "of 4 right")

src = array("I", range(1000, 1256))
dst = array("I", bytes(4 * 256))
rx = rp2.DMA()
tx = rp2.DMA()
rx.config(read=sm, write=dst, count=256, ctrl=rx.pack_ctrl(size=2), trigger=True)
tx.config(read=src, write=sm, count=256, ctrl=tx.pack_ctrl(size=2), trigger=True)
tx.wait(2000)
rx.wait(2000)
ok = sum(1 for i in range(256) if dst[i] == ~src[i] & 0xFFFFFFFF)
print("DMA:", ok, "of 256 right")
rx.close()
tx.close()
sm.active(0)
