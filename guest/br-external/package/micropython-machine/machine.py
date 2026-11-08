# MicroPython's machine module for the rp2350-linux guest: Pin, I2C, SPI, PWM
# and ADC on the board's pins, through the Linux interfaces the kernel offers
# for them:
#   Pin  /dev/gpiochip0, GPIO character device v2 ioctls (fcntl.ioctl)
#   I2C  /dev/i2c-N, I2C_RDWR
#   SPI  /dev/spidevN.0, SPI_IOC_MESSAGE (the kernel drives chip select)
#   PWM  /sys/class/pwm/pwmchip0 (channel N is GPIO N)
#   ADC  IIO sysfs, rp2350-adc (channels 0-7 GP40-GP47, 8 the temperature)
# The built-in machine module's own functions (mem32, idle, ...) stay.
#
#   from machine import Pin
#   Pin(15, Pin.OUT).value(1)

from umachine import *  # noqa: F401,F403
import fcntl
import struct
import uctypes

_GPIOCHIP = "/dev/gpiochip0"


def _iowr(nr, size, typ):
    return 0xC0000000 | size << 16 | typ << 8 | nr


def _iow(nr, size, typ):
    return 0x40000000 | size << 16 | typ << 8 | nr


# linux/gpio.h, v2
_LINE_REQ = 592  # struct gpio_v2_line_request
_LINE_CFG = 272  # struct gpio_v2_line_config
_REQ_CONFIG = 288  # offset of .config in the request
_REQ_NLINES = 560
_REQ_FD = 588
_GET_LINE = _iowr(0x07, _LINE_REQ, 0xB4)
_SET_CONFIG = _iowr(0x0D, _LINE_CFG, 0xB4)
_GET_VALUES = _iowr(0x0E, 16, 0xB4)
_SET_VALUES = _iowr(0x0F, 16, 0xB4)
_F_INPUT = 1 << 2
_F_OUTPUT = 1 << 3
_F_EDGE_RISING = 1 << 4
_F_EDGE_FALLING = 1 << 5
_F_OPEN_DRAIN = 1 << 6
_F_PULL_UP = 1 << 8
_F_PULL_DOWN = 1 << 9
_F_BIAS_DISABLED = 1 << 10
_ATTR_OUTPUT_VALUES = 2
_EVENT = 48  # struct gpio_v2_line_event


def _pin_number(id):
    if isinstance(id, Pin):
        return id._n
    if isinstance(id, str) and id.startswith("GP"):
        return int(id[2:])
    return int(id)


class _Irq:
    def __init__(self, line):
        self._line = line

    def flags(self):
        return self._line.last_edge

    def trigger(self):
        return self._line.trigger


class _Line:
    # One GPIO line's request and configuration, shared by every Pin object
    # made for it (as on a microcontroller, Pin(15) twice is the same pin)
    def __init__(self, n):
        self.n = n
        self.req = None
        self.mode = Pin.IN
        self.pull = -1
        self.trigger = 0
        self.handler = None
        self.last_edge = 0
        self.out = 0


_lines = {}


class Pin:
    IN = 0
    OUT = 1
    OPEN_DRAIN = 2
    PULL_UP = 1
    PULL_DOWN = 2
    IRQ_RISING = 1
    IRQ_FALLING = 2

    def __init__(self, id, mode=-1, pull=-1, *, value=None):
        self._n = _pin_number(id)
        self._l = _lines.get(self._n)
        if self._l is None:
            self._l = _lines[self._n] = _Line(self._n)
        elif mode == -1 and pull == -1 and value is None:
            return  # Pin(n) of a pin in use: that pin as it is
        # as on the rp2 port: a mode without a pull means no pull
        if mode != -1 and pull == -1:
            pull = None
        self.init(mode, pull, value=value)

    def __repr__(self):
        return "Pin(%d)" % self._n

    def _flags(self):
        l = self._l
        f = _F_OUTPUT if l.mode in (Pin.OUT, Pin.OPEN_DRAIN) else _F_INPUT
        if l.mode == Pin.OPEN_DRAIN:
            f |= _F_OPEN_DRAIN
        # pull -1 (never given): leave the line's bias as it is
        if l.pull == Pin.PULL_UP:
            f |= _F_PULL_UP
        elif l.pull == Pin.PULL_DOWN:
            f |= _F_PULL_DOWN
        elif l.pull is None:
            f |= _F_BIAS_DISABLED
        if l.trigger & Pin.IRQ_RISING:
            f |= _F_EDGE_RISING
        if l.trigger & Pin.IRQ_FALLING:
            f |= _F_EDGE_FALLING
        return f

    def _config(self, buf, off):
        # struct gpio_v2_line_config: flags, num_attrs, then the output value
        # as attribute 0 for line 0
        struct.pack_into("<QI", buf, off, self._flags(), 1)
        struct.pack_into("<IIQQ", buf, off + 32, _ATTR_OUTPUT_VALUES, 0, self._l.out, 1)

    def init(self, mode=-1, pull=-1, *, value=None):
        l = self._l
        if mode != -1:
            l.mode = mode
        if pull != -1:
            l.pull = pull
        if value is not None:
            l.out = 1 if value else 0
        if l.req is None:
            r = bytearray(_LINE_REQ)
            struct.pack_into("<I", r, 0, self._n)
            r[256:256 + 13] = b"micropython\0\0"
            self._config(r, _REQ_CONFIG)
            struct.pack_into("<I", r, _REQ_NLINES, 1)
            with open(_GPIOCHIP, "rb") as chip:
                fcntl.ioctl(chip, _GET_LINE, r)
            l.req = open(struct.unpack_from("<i", r, _REQ_FD)[0], "rb")
        else:
            c = bytearray(_LINE_CFG)
            self._config(c, 0)
            fcntl.ioctl(l.req, _SET_CONFIG, c)
        return self

    def value(self, v=None):
        l = self._l
        b = bytearray(16)
        if v is None:
            struct.pack_into("<QQ", b, 0, 0, 1)
            fcntl.ioctl(l.req, _GET_VALUES, b)
            return struct.unpack_from("<Q", b, 0)[0] & 1
        l.out = 1 if v else 0
        struct.pack_into("<QQ", b, 0, l.out, 1)
        fcntl.ioctl(l.req, _SET_VALUES, b)

    __call__ = value

    def on(self):
        self.value(1)

    def off(self):
        self.value(0)

    high = on
    low = off

    def toggle(self):
        self.value(1 - self._l.out)

    def mode(self, mode=None):
        if mode is None:
            return self._l.mode
        self.init(mode)

    def pull(self, pull=-1):
        if pull == -1:
            return None if self._l.pull == -1 else self._l.pull
        self.init(pull=pull)

    def wait_edge(self):
        """Block until the next edge irq() asked for; Pin.IRQ_RISING or
        Pin.IRQ_FALLING."""
        l = self._l
        ev = l.req.read(_EVENT)
        l.last_edge = Pin.IRQ_RISING if struct.unpack_from("<I", ev, 8)[0] == 1 else Pin.IRQ_FALLING
        return l.last_edge

    def _irq_loop(self):
        while self._l.handler is not None:
            self.wait_edge()
            h = self._l.handler
            if h is not None:
                h(self)

    def irq(self, handler=None, trigger=IRQ_FALLING | IRQ_RISING):
        """Edge interrupts: handler(pin) runs in a thread of its own for each
        edge. No handler: edges are still detected, read them with
        wait_edge()."""
        l = self._l
        start = handler is not None and l.handler is None
        l.handler = handler
        l.trigger = trigger
        self.init()
        if start:
            import _thread

            _thread.start_new_thread(self._irq_loop, ())
        return _Irq(l)

    def deinit(self):
        l = self._l
        l.handler = None
        if l.req is not None:
            l.req.close()
            l.req = None
        _lines.pop(self._n, None)


# linux/i2c-dev.h, linux/i2c.h
_I2C_RDWR = 0x0707
_I2C_M_RD = 1


class I2C:
    def __init__(self, id=0, *, scl=None, sda=None, freq=100000, timeout=None):
        self._f = open("/dev/i2c-%d" % id, "rb")
        self.freq = freq

    def _xfer(self, addr, msgs):
        # struct i2c_msg {u16 addr, flags, len; u8 *buf} = 12 bytes on rv32
        tab = bytearray(12 * len(msgs))
        for i, (flags, buf) in enumerate(msgs):
            struct.pack_into("<HHHxxI", tab, 12 * i, addr, flags, len(buf), uctypes.addressof(buf) if len(buf) else 0)
        data = struct.pack("<II", uctypes.addressof(tab), len(msgs))
        try:
            fcntl.ioctl(self._f, _I2C_RDWR, bytearray(data))
        except OSError as e:
            # the address wasn't acknowledged: MicroPython's ENODEV
            raise OSError(19) if e.errno == 6 else e

    def scan(self):
        found = []
        for a in range(0x08, 0x78):
            try:
                self._xfer(a, [(0, b"")])
                found.append(a)
            except OSError:
                pass
        return found

    def readfrom_into(self, addr, buf, stop=True):
        self._xfer(addr, [(_I2C_M_RD, buf)])

    def readfrom(self, addr, nbytes, stop=True):
        b = bytearray(nbytes)
        self.readfrom_into(addr, b)
        return bytes(b)

    def writeto(self, addr, buf, stop=True):
        self._xfer(addr, [(0, buf)])
        return len(buf)

    def writevto(self, addr, vector, stop=True):
        return self.writeto(addr, b"".join(vector))

    def _memaddr(self, memaddr, addrsize):
        return memaddr.to_bytes(addrsize // 8, "big")

    def readfrom_mem_into(self, addr, memaddr, buf, *, addrsize=8):
        self._xfer(addr, [(0, self._memaddr(memaddr, addrsize)), (_I2C_M_RD, buf)])

    def readfrom_mem(self, addr, memaddr, nbytes, *, addrsize=8):
        b = bytearray(nbytes)
        self.readfrom_mem_into(addr, memaddr, b, addrsize=addrsize)
        return bytes(b)

    def writeto_mem(self, addr, memaddr, buf, *, addrsize=8):
        self._xfer(addr, [(0, self._memaddr(memaddr, addrsize) + bytes(buf))])

    def deinit(self):
        self._f.close()


# linux/spi/spidev.h
_SPI_IOC_WR_MODE = _iow(1, 1, 0x6B)
_SPI_CPHA = 1
_SPI_CPOL = 2
_SPI_LSB_FIRST = 8


class SPI:
    MSB = 0
    LSB = 1

    def __init__(self, id=0, baudrate=1000000, *, polarity=0, phase=0, bits=8, firstbit=MSB, **kw):
        self._f = open("/dev/spidev%d.0" % id, "rb")
        self.init(baudrate, polarity=polarity, phase=phase, bits=bits, firstbit=firstbit)

    def init(self, baudrate=None, *, polarity=None, phase=None, bits=8, firstbit=None, **kw):
        if bits != 8:
            raise ValueError("bits")
        if baudrate is not None:
            self._hz = baudrate
        if polarity is not None:
            self._pol = polarity
        if phase is not None:
            self._pha = phase
        if firstbit is not None:
            self._first = firstbit
        mode = (_SPI_CPOL if self._pol else 0) | (_SPI_CPHA if self._pha else 0)
        if self._first == SPI.LSB:
            mode |= _SPI_LSB_FIRST
        fcntl.ioctl(self._f, _SPI_IOC_WR_MODE, bytearray([mode]))

    def _xfer(self, tx, rx, n):
        # struct spi_ioc_transfer: tx_buf, rx_buf (u64), len, speed_hz, ...
        t = bytearray(32)
        struct.pack_into(
            "<QQII", t, 0, uctypes.addressof(tx) if tx else 0, uctypes.addressof(rx) if rx else 0, n, self._hz
        )
        t[26] = 8  # bits_per_word
        fcntl.ioctl(self._f, _iow(0, 32, 0x6B), t)

    def write(self, buf):
        self._xfer(buf, None, len(buf))

    def readinto(self, buf, write=0x00):
        self._xfer(bytes([write]) * len(buf), buf, len(buf))

    def read(self, nbytes, write=0x00):
        b = bytearray(nbytes)
        self.readinto(b, write)
        return bytes(b)

    def write_readinto(self, write_buf, read_buf):
        if len(write_buf) != len(read_buf):
            raise ValueError("buffers")
        self._xfer(write_buf, read_buf, len(read_buf))

    def deinit(self):
        self._f.close()


_PWMCHIP = "/sys/class/pwm/pwmchip0"


def _put(path, v):
    with open(path, "w") as f:
        f.write(str(v))


def _get(path):
    with open(path) as f:
        return f.read().strip()


class PWM:
    def __init__(self, dest, *, freq=None, duty_u16=None, duty_ns=None, invert=False):
        self._n = _pin_number(dest)
        self._d = "%s/pwm%d" % (_PWMCHIP, self._n)
        try:
            _get(self._d + "/enable")
        except OSError:
            _put(_PWMCHIP + "/export", self._n)
        self._duty16 = None
        self.init(freq=freq, duty_u16=duty_u16, duty_ns=duty_ns, invert=invert)

    def __repr__(self):
        return "PWM(GP%d, freq=%d, duty_u16=%d)" % (self._n, self.freq(), self.duty_u16())

    def init(self, *, freq=None, duty_u16=None, duty_ns=None, invert=None):
        if invert is not None:
            _put(self._d + "/enable", 0)
            _put(self._d + "/polarity", "inversed" if invert else "normal")
        if freq is not None:
            self.freq(freq)
        if duty_u16 is not None:
            self.duty_u16(duty_u16)
        if duty_ns is not None:
            self.duty_ns(duty_ns)
        if int(_get(self._d + "/period")):
            _put(self._d + "/enable", 1)

    def _period(self):
        return int(_get(self._d + "/period"))

    def freq(self, f=None):
        p = self._period()
        if f is None:
            return 1000000000 // p if p else 0
        duty = self._duty16
        _put(self._d + "/duty_cycle", 0)  # must not exceed the new period
        _put(self._d + "/period", 1000000000 // f)
        if duty is not None:
            self.duty_u16(duty)
        _put(self._d + "/enable", 1)

    def duty_u16(self, d=None):
        p = self._period()
        if d is None:
            return int(_get(self._d + "/duty_cycle")) * 65535 // p if p else 0
        self._duty16 = d
        _put(self._d + "/duty_cycle", p * d // 65535)

    def duty_ns(self, ns=None):
        if ns is None:
            return int(_get(self._d + "/duty_cycle"))
        self._duty16 = None
        _put(self._d + "/duty_cycle", ns)

    def deinit(self):
        _put(self._d + "/enable", 0)
        _put(_PWMCHIP + "/unexport", self._n)


def _iio(name):
    import os

    for d in os.listdir("/sys/bus/iio/devices"):
        p = "/sys/bus/iio/devices/" + d
        try:
            if _get(p + "/name") == name:
                return p
        except OSError:
            pass
    raise OSError(19)


class ADC:
    CORE_TEMP = 8

    def __init__(self, src):
        n = _pin_number(src) if not isinstance(src, int) else src
        self._ch = n - 40 if n >= 40 else n  # Pin(40)..Pin(47) or a channel
        d = _iio("rp2350-adc")
        if self._ch == ADC.CORE_TEMP:
            self._raw, self._scale = d + "/in_temp_raw", None
        else:
            self._raw, self._scale = d + "/in_voltage%d_raw" % self._ch, d + "/in_voltage_scale"

    def __repr__(self):
        return "ADC(%d)" % self._ch

    def read_raw(self):
        return int(_get(self._raw))

    def read_u16(self):
        r = self.read_raw()
        return r << 4 | r >> 8

    def read_uv(self):
        if self._scale is None:
            raise ValueError("temperature channel")
        return int(self.read_raw() * float(_get(self._scale)) * 1000)

    def read_temp(self):
        """The temperature sensor, in degrees C (channel 8 only)."""
        d = self._raw.rsplit("/", 1)[0]
        return (self.read_raw() + float(_get(d + "/in_temp_offset"))) * float(_get(d + "/in_temp_scale")) / 1000
