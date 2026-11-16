# rp2 for MicroPython on rp2350-linux: the RP2350's PIO1 and PIO2 through
# /dev/pio1 and /dev/pio2 (guest/linux/drivers/rvemu/pio-rvemu.c), with the
# rp2 port's API:
#   rp2.asm_pio, rp2.asm_pio_encode, rp2.PIOASMError  the rp2 port's own
#   rp2.PIO(1 or 2)               PIO0 is the board's radio
#   rp2.StateMachine(4-11, ...)   PIO1's are 4-7, PIO2's 8-11
#   rp2.DMA()                     between a buffer and a state machine only
#
#   import rp2
#   from machine import Pin
#   @rp2.asm_pio(set_init=rp2.PIO.OUT_LOW)
#   def blink():
#       wrap_target()
#       set(pins, 1) [31]
#       set(pins, 0) [31]
#       wrap()
#   sm = rp2.StateMachine(4, blink, freq=10000, set_base=Pin(14))
#   sm.active(1)          (freq: at least the 348 MHz clock / 65536)
#
# Differences from the rp2 port: one program at a time has a block (the
# first PIO or StateMachine to use it opens it; OSError EBUSY when another
# program has it); a pin given to a StateMachine leaves machine.Pin (a Pin
# object for it stops working); irq handlers run in a thread; StateMachine.get/put wait
# by sleeping on the block's interrupt; DMA moves data only between a
# buffer and a state machine's FIFO, through a buffer the kernel gave this
# program (so a read lands in your buffer when the transfer is done and
# active() or wait() has seen it).
#
# The assembler (PIOASMEmit, asm_pio, asm_pio_encode) is the rp2 port's
# rp2.py: MIT license; Copyright (c) 2020-2021 Damien P. George.
# Changed here: PIOASMEmit's defaults are numbers, not PIO constants, and
# mov to pindirs is RP2350's destination 3. The license text: LICENSE-micropython.
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Josh Vanderberg (the rest of this file)

from micropython import const
import _rvmem
import fcntl
import struct
import uctypes

# SPDX-SnippetBegin
# SPDX-SnippetCopyrightText: Copyright (c) 2020-2021 Damien P. George
# SPDX-License-Identifier: MIT
_PROG_DATA = const(0)
_PROG_OFFSET_PIO0 = const(1)
_PROG_OFFSET_PIO1 = const(2)
_PROG_OFFSET_PIO2 = const(3)
_PROG_EXECCTRL = const(4)
_PROG_SHIFTCTRL = const(5)
_PROG_OUT_PINS = const(6)
_PROG_SET_PINS = const(7)
_PROG_SIDESET_PINS = const(8)
_PROG_MAX_FIELDS = const(9)

class PIOASMError(Exception):
    pass


class PIOASMEmit:
    def __init__(
        self,
        *,
        out_init=None,
        set_init=None,
        sideset_init=None,
        side_pindir=False,
        in_shiftdir=0,
        out_shiftdir=0,
        autopush=False,
        autopull=False,
        push_thresh=32,
        pull_thresh=32,
        fifo_join=0,
    ):
        # array is a built-in module so importing it here won't require
        # scanning the filesystem.
        from array import array

        self.labels = {}
        execctrl = side_pindir << 29
        shiftctrl = (
            fifo_join << 30
            | (pull_thresh & 0x1F) << 25
            | (push_thresh & 0x1F) << 20
            | out_shiftdir << 19
            | in_shiftdir << 18
            | autopull << 17
            | autopush << 16
        )
        self.prog = [array("H"), -1, -1, -1, execctrl, shiftctrl, out_init, set_init, sideset_init]

        self.wrap_used = False

        if sideset_init is None:
            self.sideset_count = 0
        elif isinstance(sideset_init, int):
            self.sideset_count = 1
        else:
            self.sideset_count = len(sideset_init)

    def start_pass(self, pass_):
        if pass_ == 1:
            if not self.wrap_used and self.num_instr:
                self.wrap()
            self.delay_max = 31
            if self.sideset_count:
                self.sideset_opt = self.num_sideset != self.num_instr
                if self.sideset_opt:
                    self.prog[_PROG_EXECCTRL] |= 1 << 30
                    self.sideset_count += 1
                self.delay_max >>= self.sideset_count
        self.pass_ = pass_
        self.num_instr = 0
        self.num_sideset = 0

    def __getitem__(self, key):
        return self.delay(key)

    def delay(self, delay):
        if self.pass_ > 0:
            if delay > self.delay_max:
                raise PIOASMError("delay too large")
            self.prog[_PROG_DATA][-1] |= delay << 8
        return self

    def side(self, value):
        self.num_sideset += 1
        if self.pass_ > 0:
            if self.sideset_count == 0:
                raise PIOASMError("no sideset")
            elif value >= (1 << self.sideset_count):
                raise PIOASMError("sideset too large")
            set_bit = 13 - self.sideset_count
            self.prog[_PROG_DATA][-1] |= self.sideset_opt << 12 | value << set_bit
        return self

    def wrap_target(self):
        self.prog[_PROG_EXECCTRL] |= self.num_instr << 7

    def wrap(self):
        assert self.num_instr
        self.prog[_PROG_EXECCTRL] |= (self.num_instr - 1) << 12
        self.wrap_used = True

    def label(self, label):
        if self.pass_ == 0:
            if label in self.labels:
                raise PIOASMError("duplicate label {}".format(label))
            self.labels[label] = self.num_instr

    def word(self, instr, label=None):
        self.num_instr += 1
        if self.pass_ > 0:
            if label is None:
                label = 0
            else:
                if label not in self.labels:
                    raise PIOASMError("unknown label {}".format(label))
                label = self.labels[label]
            self.prog[_PROG_DATA].append(instr | label)
        return self

    def nop(self):
        return self.word(0xA042)

    def jmp(self, cond, label=None):
        if label is None:
            label = cond
            cond = 0  # always
        return self.word(0x0000 | cond << 5, label)

    def wait(self, polarity, src, index):
        if src == 6:
            src = 1  # "pin"
        elif src != 0:
            src = 2  # "irq"
        return self.word(0x2000 | polarity << 7 | src << 5 | index)

    def in_(self, src, data):
        if not 0 < data <= 32:
            raise PIOASMError("invalid bit count {}".format(data))
        return self.word(0x4000 | src << 5 | data & 0x1F)

    def out(self, dest, data):
        if dest == 8:
            dest = 7  # exec
        if not 0 < data <= 32:
            raise PIOASMError("invalid bit count {}".format(data))
        return self.word(0x6000 | dest << 5 | data & 0x1F)

    def push(self, value=0, value2=0):
        value |= value2
        if not value & 1:
            value |= 0x20  # block by default
        return self.word(0x8000 | (value & 0x60))

    def pull(self, value=0, value2=0):
        value |= value2
        if not value & 1:
            value |= 0x20  # block by default
        return self.word(0x8080 | (value & 0x60))

    def mov(self, dest, src):
        if dest == 8:
            dest = 4  # exec
        elif dest == 4:
            dest = 3  # pindirs: RP2350's mov destination 3 (4 is exec)
        return self.word(0xA000 | dest << 5 | src)

    def irq(self, mod, index=None):
        if index is None:
            index = mod
            mod = 0  # no modifiers
        return self.word(0xC000 | (mod & 0x60) | index)

    def set(self, dest, data):
        return self.word(0xE000 | dest << 5 | data)


_pio_funcs = {
    # source constants for wait
    "gpio": 0,
    # "pin": see below, translated to 1
    # "irq": see below function, translated to 2
    # source/dest constants for in_, out, mov, set
    "pins": 0,
    "x": 1,
    "y": 2,
    "null": 3,
    "pindirs": 4,
    "pc": 5,
    "status": 5,
    "isr": 6,
    "osr": 7,
    "exec": 8,  # translated to 4 for mov, 7 for out
    # operation functions for mov's src
    "invert": lambda x: x | 0x08,
    "reverse": lambda x: x | 0x10,
    # jmp condition constants
    "not_x": 1,
    "x_dec": 2,
    "not_y": 3,
    "y_dec": 4,
    "x_not_y": 5,
    "pin": 6,
    "not_osre": 7,
    # constants for push, pull
    "noblock": 0x01,
    "block": 0x21,
    "iffull": 0x40,
    "ifempty": 0x40,
    # constants and modifiers for irq
    # "noblock": see above
    # "block": see above
    "clear": 0x40,
    "rel": lambda x: x | 0x10,
}


_pio_directives = (
    "wrap_target",
    "wrap",
    "label",
)


_pio_instructions = (
    "word",
    "nop",
    "jmp",
    "wait",
    "in_",
    "out",
    "push",
    "pull",
    "mov",
    "irq",
    "set",
)


def asm_pio(**kw):
    emit = PIOASMEmit(**kw)

    def dec(f):
        nonlocal emit

        gl = f.__globals__
        old_gl = gl.copy()
        gl.clear()

        try:
            gl.update(_pio_funcs)
            for name in _pio_directives:
                gl[name] = getattr(emit, name)
            for name in _pio_instructions:
                gl[name] = getattr(emit, name)

            emit.start_pass(0)
            f()

            emit.start_pass(1)
            f()
        finally:
            gl.clear()
            gl.update(old_gl)

        return emit.prog

    return dec


# sideset_count is inclusive of enable bit
def asm_pio_encode(instr, sideset_count, sideset_opt=False):
    emit = PIOASMEmit()
    emit.sideset_count = sideset_count
    emit.sideset_opt = sideset_opt != 0
    emit.delay_max = 31 >> (emit.sideset_count + emit.sideset_opt)
    emit.pass_ = 1
    emit.num_instr = 0
    emit.num_sideset = 0

    gl = _pio_funcs
    for name in _pio_instructions:
        gl[name] = getattr(emit, name)
    gl["jmp"] = None  # emit.jmp currently not supported

    try:
        exec(instr, gl)
    finally:
        for name in _pio_instructions:
            del gl[name]

    if len(emit.prog[_PROG_DATA]) != 1:
        raise PIOASMError("expecting exactly 1 instruction")
    return emit.prog[_PROG_DATA][0]
# SPDX-SnippetEnd


# ------------------------------------------------------------------ the blocks

# registers
_CTRL = const(0x000)
_FSTAT = const(0x004)
_FDEBUG = const(0x008)
_FLEVEL = const(0x00C)
_TXF = const(0x010)
_RXF = const(0x020)
_IRQ = const(0x030)
_INSTR_MEM = const(0x048)
_SM = const(0x0C8)
_CLKDIV = const(0x00)
_EXECCTRL = const(0x04)
_SHIFTCTRL = const(0x08)
_ADDR = const(0x0C)
_INSTR = const(0x10)
_PINCTRL = const(0x14)
_GPIOBASE = const(0x168)
_INTE = const(0x170)
_SET = const(0x2000)
_CLR = const(0x3000)

# rvemu-pio.h
_MAP_SIZE = const(0x4000)
_IOC_INFO = const(0x80185250)
_IOC_PIN_CLAIM = const(0x40045251)
_DMA_BUF_OFFSET = const(0x10000)
_DMA_MAX_BUF = const(0x10000)
_IOC_DMA_ALLOC = const(0x40045253)
_IOC_DMA_START = const(0xC0305254)
_IOC_DMA_STATUS = const(0xC0305255)
_IOC_DMA_WAIT = const(0xC0305256)
_IOC_DMA_STOP = const(0x40305257)
_DMA_FMT = "<12I"
_DMA_NEW = const(0xFFFFFFFF)


class _Block:
    """One open block: its file, its registers, what is loaded."""

    def __init__(self, n):
        self.n = n
        self.f = open("/dev/pio%d" % n, "r+b", buffering=0)
        self.fd = self.f.fileno()
        info = bytearray(24)
        fcntl.ioctl(self.fd, _IOC_INFO, info)
        self.clk = struct.unpack_from("<I", info, 4)[0]
        self.base = _rvmem.mmap(self.fd, _MAP_SIZE)
        self.used = 0  # instruction memory
        self.progs = {}  # id(program) -> (program, offset)
        self.irq_thread = False
        self.handlers = {}  # IRQ flag -> (handler, object)
        self.dma = None  # (address, size) of the DMA buffer
        self.dma_used = []  # [offset, size] regions
        self.pinned = 0  # state machines whose pins are set for GPIOBASE

    def rd(self, r):
        return _rvmem.read32(self.base + r)

    def wr(self, r, v):
        _rvmem.write32(self.base + r, v & 0xFFFFFFFF)

    def set_bits(self, r, m):
        _rvmem.write32(self.base + _SET + r, m)

    def clear_bits(self, r, m):
        _rvmem.write32(self.base + _CLR + r, m)

    def claim(self, pin):
        # As on the rp2 port, a pin given to PIO leaves machine.Pin: drop
        # the GPIO line a Pin object holds for it, or the claim is EBUSY
        import machine

        l = machine._lines.pop(pin, None)
        if l is not None:
            l.handler = None
            if l.req is not None:
                l.req.close()
                l.req = None
        fcntl.ioctl(self.fd, _IOC_PIN_CLAIM, bytearray(struct.pack("<I", pin)))

    def wait_irq(self, timeout_ms=100):
        """Sleep until the block's IRQ0 output is up (or timeout_ms)."""
        import select

        p = select.poll()
        p.register(self.f, select.POLLIN)
        if p.poll(timeout_ms):
            self.f.read(4)

    def wait(self, ready, source):
        """Until ready(): a few looks, then sleeping on IRQ0 with `source`
        (an INTR bit) enabled."""
        for _ in range(16):
            if ready():
                return
        m = 1 << source
        while not ready():
            had = self.rd(_INTE) & m
            if not had:
                self.set_bits(_INTE, m)
            if not ready():
                self.wait_irq()
            if not had:
                self.clear_bits(_INTE, m)

    def add(self, prog):
        k = id(prog)
        if k in self.progs:
            return self.progs[k][1]
        code = prog[_PROG_DATA]
        n = len(code)
        mask = (1 << n) - 1
        for off in range(32 - n, -1, -1):
            if not self.used & (mask << off):
                break
        else:
            raise OSError(28)  # ENOSPC
        for i in range(n):
            ins = code[i]
            if ins >> 13 == 0:  # JMP: relative to the program's start
                ins += off
            self.wr(_INSTR_MEM + 4 * i + 4 * off, ins)
        self.used |= mask << off
        self.progs[k] = (prog, off)
        return off

    def remove(self, prog=None):
        if prog is None:
            self.used = 0
            self.progs = {}
            return
        e = self.progs.pop(id(prog), None)
        if e:
            self.used &= ~(((1 << len(prog[_PROG_DATA])) - 1) << e[1])

    def irq_loop(self):
        while self.handlers:
            ints = self.rd(_INTE + 8)  # IRQ0_INTS
            if not ints:
                self.wait_irq(1000)
                continue
            flags = ints >> 8 & 0xFF
            self.wr(_IRQ, flags)
            for f in range(8):
                if flags >> f & 1 and f in self.handlers:
                    h, o = self.handlers[f]
                    h(o)

    def set_handler(self, flag, handler, obj):
        if handler is None:
            self.handlers.pop(flag, None)
            self.clear_bits(_INTE, 1 << (8 + flag))
            return
        self.handlers[flag] = (handler, obj)
        self.set_bits(_INTE, 1 << (8 + flag))
        if not self.irq_thread:
            import _thread

            self.irq_thread = True

            def run():
                try:
                    self.irq_loop()
                finally:
                    self.irq_thread = False

            _thread.start_new_thread(run, ())


_blocks = {}


def _block(n):
    if n == 0:
        raise ValueError("PIO0 is the board's radio: use PIO 1 or 2")
    if n not in (1, 2):
        raise ValueError("no PIO %d" % n)
    b = _blocks.get(n)
    if b is None:
        b = _blocks[n] = _Block(n)
    return b


def _pin(p):
    if p is None:
        return None
    if isinstance(p, int):
        return p
    import machine

    return machine._pin_number(p)


class PIO:
    IN_LOW = 0
    IN_HIGH = 1
    OUT_LOW = 2
    OUT_HIGH = 3
    SHIFT_LEFT = 0
    SHIFT_RIGHT = 1
    JOIN_NONE = 0
    JOIN_TX = 1
    JOIN_RX = 2
    IRQ_SM0 = 0x100
    IRQ_SM1 = 0x200
    IRQ_SM2 = 0x400
    IRQ_SM3 = 0x800

    def __init__(self, id):
        self._b = _block(id)
        self._id = id

    def __repr__(self):
        return "PIO(%d)" % self._id

    def add_program(self, prog):
        self._b.add(prog)

    def remove_program(self, prog=None):
        self._b.remove(prog)

    def state_machine(self, id, program=None, *args, **kw):
        if not 0 <= id < 4:
            raise ValueError("state machine 0-3")
        return StateMachine(4 * self._id + id, program, *args, **kw)

    def gpio_base(self, base=None):
        if base is None:
            return self._b.rd(_GPIOBASE) & 16
        if base not in (0, 16):
            raise ValueError("base 0 or 16")
        self._b.wr(_GPIOBASE, base)

    def irq(self, handler=None, trigger=0xF00, hard=False):
        """handler(pio) when a state machine sets one of the IRQ flags in
        trigger (IRQ_SM0-IRQ_SM3; flag N is 0x100 << N)."""
        for f in range(8):
            if trigger >> (8 + f) & 1:
                self._b.set_handler(f, handler, self)


class StateMachine:
    def __init__(self, id, program=None, *args, **kw):
        if not 0 <= id < 12:
            raise ValueError("state machine 0-11")
        self._id = id
        self._b = _block(id // 4)
        self._sm = id % 4
        self._prog = None
        if program is not None:
            self.init(program, *args, **kw)

    def __repr__(self):
        return "StateMachine(%d)" % self._id

    def _r(self, r):
        return _SM + 0x18 * self._sm + r

    def init(
        self,
        program,
        freq=-1,
        *,
        in_base=None,
        out_base=None,
        set_base=None,
        jmp_pin=None,
        sideset_base=None,
        in_shiftdir=None,
        out_shiftdir=None,
        push_thresh=None,
        pull_thresh=None,
    ):
        b, sm = self._b, self._sm
        off = b.add(program)
        self._prog = program
        if freq is None or freq < 0:
            clkdiv = 1 << 16
        else:
            div = b.clk / freq
            if div < 1:
                raise ValueError("freq too large")
            if div > 65536:
                raise ValueError("freq too small")
            i = int(div)
            clkdiv = (i & 0xFFFF) << 16 | int((div - i) * 256) << 8
        execctrl = program[_PROG_EXECCTRL]
        wrap_target = (execctrl >> 7 & 0x1F) + off
        wrap = (execctrl >> 12 & 0x1F) + off
        execctrl = execctrl & ~(0x1F << 7 | 0x1F << 12) | wrap_target << 7 | wrap << 12
        shiftctrl = program[_PROG_SHIFTCTRL]
        if in_shiftdir is not None:
            shiftctrl = shiftctrl & ~(1 << 18) | in_shiftdir << 18
        if out_shiftdir is not None:
            shiftctrl = shiftctrl & ~(1 << 19) | out_shiftdir << 19
        if push_thresh is not None:
            shiftctrl = shiftctrl & ~(0x1F << 20) | (push_thresh & 0x1F) << 20
        if pull_thresh is not None:
            shiftctrl = shiftctrl & ~(0x1F << 25) | (pull_thresh & 0x1F) << 25

        def init_list(v):
            if v is None:
                return ()
            return v if isinstance(v, tuple) else (v,)

        outs = init_list(program[_PROG_OUT_PINS])
        sets = init_list(program[_PROG_SET_PINS])
        sides = init_list(program[_PROG_SIDESET_PINS])
        pins = {}
        in_base, out_base, set_base = _pin(in_base), _pin(out_base), _pin(set_base)
        jmp_pin, sideset_base = _pin(jmp_pin), _pin(sideset_base)
        used = [p for p in (in_base, out_base, set_base, jmp_pin, sideset_base) if p is not None]
        # GPIOBASE: 16 for pins above 31, else 0; a machine without pins
        # leaves it as it is
        base = b.rd(_GPIOBASE) & 16
        if used:
            want = 16 if max(used) >= 32 else 0
            if want and min(used) < 16:
                raise ValueError("pins must be in 0-31 or 16-47")
            if want != base:
                # not under another machine that runs, or has pins set for
                # this base (they would point elsewhere)
                if (b.rd(_CTRL) & 0xF | b.pinned) & ~(1 << sm):
                    raise ValueError("the block's other state machines use GPIO base %d" % base)
                b.wr(_GPIOBASE, want)
                base = want
            b.pinned |= 1 << sm
        pinctrl = 0
        if out_base is not None:
            pinctrl |= (out_base - base) & 31 | len(outs or (0,)) << 20
            for k, v in enumerate(outs):
                pins[out_base + k] = v
        if set_base is not None:
            pinctrl |= ((set_base - base) & 31) << 5 | len(sets or (0,)) << 26
            for k, v in enumerate(sets):
                pins[set_base + k] = v
        if in_base is not None:
            pinctrl |= ((in_base - base) & 31) << 15
        nside = len(sides)
        if nside:
            # the opt bit counts in SIDESET_COUNT
            pinctrl |= ((sideset_base or 0) - base & 31) << 10 | (nside + (execctrl >> 30 & 1)) << 29
            if sideset_base is not None:
                for k, v in enumerate(sides):
                    pins[sideset_base + k] = v
        if jmp_pin is not None:
            execctrl = execctrl & ~(0x1F << 24) | ((jmp_pin - base) & 31) << 24
        for p in pins:
            b.claim(p)
        b.clear_bits(_CTRL, 1 << sm)
        b.wr(self._r(_CLKDIV), clkdiv)
        b.wr(self._r(_EXECCTRL), execctrl)
        b.wr(self._r(_SHIFTCTRL), shiftctrl)
        b.wr(self._r(_PINCTRL), pinctrl)
        # empty the FIFOs (a join change does), restart, then to the start
        _rvmem.write32(b.base + 0x1000 + self._r(_SHIFTCTRL), 1 << 31)
        _rvmem.write32(b.base + 0x1000 + self._r(_SHIFTCTRL), 1 << 31)
        b.wr(_FDEBUG, 0x01010101 << sm)
        b.set_bits(_CTRL, 1 << (4 + sm) | 1 << (8 + sm))
        b.wr(self._r(_INSTR), off)  # jmp off
        # pins' first directions and levels, by SET on this machine
        if pins:
            ec = b.rd(self._r(_EXECCTRL))
            b.wr(self._r(_EXECCTRL), ec & ~(1 << 17))
            for p, v in pins.items():
                b.wr(self._r(_PINCTRL), 1 << 26 | ((p - base) & 31) << 5)
                b.wr(self._r(_INSTR), 0xE000 | 0 << 5 | (v & 1))  # set pins
                b.wr(self._r(_INSTR), 0xE000 | 4 << 5 | (v >> 1 & 1))  # set pindirs
            b.wr(self._r(_PINCTRL), pinctrl)
            b.wr(self._r(_EXECCTRL), ec)

    def active(self, value=None):
        b = self._b
        if value is None:
            return bool(b.rd(_CTRL) >> self._sm & 1)
        if value:
            b.set_bits(_CTRL, 1 << self._sm)
        else:
            b.clear_bits(_CTRL, 1 << self._sm)

    def restart(self):
        self._b.set_bits(_CTRL, 1 << (4 + self._sm) | 1 << (8 + self._sm))

    def exec(self, instr):
        if isinstance(instr, str):
            p = self._prog
            count, opt = 0, False
            if p is not None and p[_PROG_SIDESET_PINS] is not None:
                s = p[_PROG_SIDESET_PINS]
                count = len(s) if isinstance(s, tuple) else 1
                opt = bool(p[_PROG_EXECCTRL] >> 30 & 1)
                count += opt
            instr = asm_pio_encode(instr, count, opt)
        self._b.wr(self._r(_INSTR), instr)

    def rx_fifo(self):
        return self._b.rd(_FLEVEL) >> (8 * self._sm + 4) & 0xF

    def tx_fifo(self):
        return self._b.rd(_FLEVEL) >> (8 * self._sm) & 0xF

    def put(self, value, shift=0):
        b, sm = self._b, self._sm
        full = 1 << (16 + sm)
        vals = (value,) if isinstance(value, int) else value
        for v in vals:
            if b.rd(_FSTAT) & full:
                b.wait(lambda: not b.rd(_FSTAT) & full, 4 + sm)
            b.wr(_TXF + 4 * sm, v << shift)

    def get(self, buf=None, shift=0):
        b, sm = self._b, self._sm
        empty = 1 << (8 + sm)

        def one():
            if b.rd(_FSTAT) & empty:
                b.wait(lambda: not b.rd(_FSTAT) & empty, sm)
            return b.rd(_RXF + 4 * sm) >> shift

        if buf is None:
            return one()
        for i in range(len(buf)):
            buf[i] = one()

    def irq(self, handler=None, trigger=0, hard=False):
        """handler(sm) when this machine sets IRQ flag N (its own number,
        as `irq(rel(0))` does)."""
        self._b.set_handler(self._sm, handler, self)


# ------------------------------------------------------------------ DMA


class DMA:
    """A DMA channel between a buffer and a state machine's FIFO:
    config(read=sm, write=buf) moves sm's RX FIFO into buf, config(read=buf,
    write=sm) buf into sm's TX FIFO; count transfers of the ctrl's size
    (pack_ctrl(size=0, 1 or 2): bytes, halfwords, words). ring_size and
    ring_sel in ctrl make the buffer side a ring (up to 4096 bytes)."""

    def __init__(self):
        self.channel = None
        self.read = self.write = None
        self.count = 0
        self.ctrl = self.pack_ctrl()
        self._b = None
        self._region = None
        self._handler = None

    @staticmethod
    def pack_ctrl(default=None, **kw):
        size = kw.get("size", 2)
        ring_size = kw.get("ring_size", 0)
        ring_sel = kw.get("ring_sel", False)
        inc_read = kw.get("inc_read", True)
        inc_write = kw.get("inc_write", True)
        treq = kw.get("treq_sel", 0x3F)
        return (
            1
            | size << 2
            | inc_read << 4
            | inc_write << 6
            | ring_size << 8
            | ring_sel << 12
            | treq << 17
        )

    @staticmethod
    def unpack_ctrl(ctrl):
        return {
            "enable": ctrl & 1,
            "size": ctrl >> 2 & 3,
            "inc_read": ctrl >> 4 & 1,
            "inc_write": ctrl >> 6 & 1,
            "ring_size": ctrl >> 8 & 0xF,
            "ring_sel": ctrl >> 12 & 1,
            "treq_sel": ctrl >> 17 & 0x3F,
        }

    def _buffer(self, nbytes):
        b = self._b
        if b.dma is None:
            n = bytearray(4)
            struct.pack_into("<I", n, 0, _DMA_MAX_BUF)
            fcntl.ioctl(b.fd, _IOC_DMA_ALLOC, n)
            b.dma = (_rvmem.mmap(b.fd, _DMA_MAX_BUF, _DMA_BUF_OFFSET), _DMA_MAX_BUF)
        if self._region and self._region[1] >= nbytes:
            return self._region[0]
        self._free_region()
        # first fit, 4 KiB aligned (a ring must be aligned to its size)
        need = (nbytes + 4095) & ~4095
        at = 0
        for o, s in sorted(b.dma_used):
            if at + need <= o:
                break
            at = max(at, o + s)
        if at + need > b.dma[1]:
            raise OSError(12)  # ENOMEM
        self._region = [at, need]
        b.dma_used.append(self._region)
        return at

    def _free_region(self):
        if self._region and self._b:
            self._b.dma_used.remove(self._region)
        self._region = None

    def config(self, read=None, write=None, count=None, ctrl=None, trigger=False):
        if read is not None:
            self.read = read
        if write is not None:
            self.write = write
        if count is not None:
            self.count = count
        if ctrl is not None:
            self.ctrl = ctrl
        if trigger:
            self.active(1)

    def _setup(self):
        rd, wr = self.read, self.write
        if isinstance(rd, StateMachine) and not isinstance(wr, StateMachine):
            sm, buf, from_sm = rd, wr, True
        elif isinstance(wr, StateMachine) and not isinstance(rd, StateMachine):
            sm, buf, from_sm = wr, rd, False
        else:
            raise ValueError("DMA is between a buffer and a StateMachine")
        if self._b is not None and self._b is not sm._b:
            self.close()
        self._b = sm._b
        c = self.unpack_ctrl(self.ctrl)
        size = 1 << c["size"]
        ring = c["ring_size"]
        count, endless = self._count()
        nbytes = (1 << ring) if ring else count * size
        off = self._buffer(nbytes)
        view = uctypes.bytearray_at(self._b.dma[0] + off, nbytes)
        if not from_sm:
            data = bytes(buf)[:nbytes]
            view[: len(data)] = data
        return sm, buf, from_sm, size, off, ring, view

    def _count(self):
        # TRANS_COUNT's MODE, as on the chip: 0xF in the top four bits is
        # endless (with a ring)
        c = self.count
        if c >> 28 == 0xF:
            return (c & 0x0FFFFFFF) or 1, True
        return c, False

    def active(self, value=None):
        if value is None:
            if self.channel is None:
                return False
            busy = self._status()[0]
            if not busy:
                self._done()
            return bool(busy)
        if value:
            sm, buf, from_sm, size, off, ring, view = self._setup()
            count, endless = self._count()
            d = struct.pack(
                _DMA_FMT,
                _DMA_NEW if self.channel is None else self.channel,
                sm._sm,
                (1 if from_sm else 0) | (2 if endless else 0),
                size,
                off,
                count,
                ring,
                0, 0, 0, 0, 0,
            )
            d = bytearray(d)
            fcntl.ioctl(self._b.fd, _IOC_DMA_START, d)
            self.channel = struct.unpack_from("<I", d, 0)[0]
            self._pending = (buf, from_sm, view)
        elif self.channel is not None:
            self._stop()

    def _status(self, cmd=_IOC_DMA_STATUS, timeout_ms=0):
        d = bytearray(struct.pack(_DMA_FMT, self.channel, 0, 0, 0, 0, 0, 0, timeout_ms, 0, 0, 0, 0))
        fcntl.ioctl(self._b.fd, cmd, d)
        v = struct.unpack(_DMA_FMT, d)
        if v[9]:
            raise OSError(5)  # EIO: a bus error
        return v[8], v[10], v[11]  # busy, remaining, pos

    def _done(self):
        p = getattr(self, "_pending", None)
        if p:
            buf, from_sm, view = p
            if from_sm:
                n = min(len(bytes(buf)), len(view))
                uctypes.bytearray_at(uctypes.addressof(buf), n)[:] = view[:n]
            self._pending = None

    def wait(self, timeout_ms=0):
        """Until the transfer is done (the rp2 port has no such call)."""
        if self.channel is not None:
            self._status(_IOC_DMA_WAIT, timeout_ms)
            self._done()

    def _stop(self):
        d = bytearray(struct.pack(_DMA_FMT, self.channel, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
        fcntl.ioctl(self._b.fd, _IOC_DMA_STOP, d)
        self._done()
        self.channel = None

    def irq(self, handler=None, hard=False):
        """handler(dma) once the transfer is done (from a thread)."""
        self._handler = handler
        if handler is None or self.channel is None:
            return
        import _thread

        def run():
            try:
                self.wait()
            finally:
                h = self._handler
                if h:
                    h(self)

        _thread.start_new_thread(run, ())

    def close(self):
        if self.channel is not None:
            self._stop()
        self._free_region()
        self._b = None
