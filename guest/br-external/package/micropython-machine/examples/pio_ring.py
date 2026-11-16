# Endless DMA around a ring, from MicroPython: a state machine pushes a
# counting-down X as fast as its clock lets it, and a DMA channel keeps
# writing those words around a 64-byte ring until it is stopped. The ring
# then holds the last 16 values, one apart except where the writes wrapped.
# micropython pio_ring.py
from array import array
import rp2
import time


@rp2.asm_pio()
def count():
    label("top")
    mov(isr, x)
    push()
    jmp(x_dec, "top")
    jmp("top")


sm = rp2.StateMachine(4, count, freq=100_000)
ring = array("I", bytes(64))
dma = rp2.DMA()
# count 0xF.......: TRANS_COUNT's endless mode; ring_size 6: 2^6 bytes
dma.config(read=sm, write=ring, count=0xF000_0000, ctrl=dma.pack_ctrl(size=2, ring_size=6), trigger=True)
sm.active(1)
time.sleep(0.5)
print("running after 0.5 s:", dma.active())
sm.active(0)
dma.active(0)  # stops it, and copies the ring into `ring`
steps = sum(1 for i in range(16) if (ring[i] - ring[(i + 1) % 16]) & 0xFFFFFFFF == 1)
print("ring:", steps, "of 16 steps of 1")
dma.close()
