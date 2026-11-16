#!/usr/bin/env python3
"""Test the Linux board's pins against a second board running ../pintester.

usage: pin-test.py [--host ADDR] [--only NAME,...]

The tester's USB serial port is /dev/cu.usbmodempintest*; the Linux board is
reached over ssh (root, key auth; --host, or its address from the console).
Wiring (tester GP <-> Linux board GP), plus a common ground:

    tester GP10 <- GP13   board output
    tester GP11 -> GP14   board input, edge events
    tester GP12 <- GP15   board PWM
    tester GP13 -- GP3    board pull-up
    tester GP14 -- GP4    I2C0 SDA (the tester is a target at 0x42)
    tester GP15 -- GP5    I2C0 SCL
    board GP19 -> GP16    SPI0 loopback (a jumper on the Linux board)

The pio section uses GP13 (PIO output) and GP14 (PIO input) the same way.

Pull-downs aren't tested: on the RP2350 a line that has been high stays
near 2 V against the internal pull-down (erratum RP2350-E9).
"""
import argparse
import glob
import os
import re
import select
import subprocess
import sys
import termios
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))

# GPIO uAPI v2 helper, built on the board with tcc. "toggle LINE N HALF_MS",
# "edges LINE N SECS"; prints "ready" once the line is requested.
GPIOV2_C = r'''
#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	if (argc < 5) return 2;
	int chip = open("/dev/gpiochip0", O_RDWR), line = atoi(argv[2]), n = atoi(argv[3]), t = atoi(argv[4]);
	struct gpio_v2_line_request r; memset(&r, 0, sizeof r);
	r.offsets[0] = line; r.num_lines = 1; strcpy(r.consumer, "pin-test");
	int out = !strcmp(argv[1], "toggle");
	r.config.flags = out ? GPIO_V2_LINE_FLAG_OUTPUT : GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
	r.event_buffer_size = 64;
	if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &r) < 0) { perror("GET_LINE"); return 1; }
	printf("ready\n"); fflush(stdout);
	if (out) {
		usleep(300000);
		struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
		for (int i = 0; i < n; i++) {
			struct gpio_v2_line_values v = { .bits = i & 1 ? 0 : 1, .mask = 1 };
			ioctl(r.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
			next.tv_nsec += t * 1000000L; while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, 0);
		}
		printf("toggled %d\n", n);
		return 0;
	}
	int got = 0, last = -1, alt = 1;
	struct pollfd p = { .fd = r.fd, .events = POLLIN };
	time_t end = time(0) + t;
	while (got < n && time(0) < end) {
		if (poll(&p, 1, 200) <= 0) continue;
		struct gpio_v2_line_event e[16]; ssize_t k = read(r.fd, e, sizeof e);
		for (int i = 0; i < k / (ssize_t)sizeof e[0]; i++) {
			int rise = e[i].id == GPIO_V2_LINE_EVENT_RISING_EDGE;
			if (last == rise) alt = 0;
			last = rise; got++;
		}
	}
	printf("events=%d alternating=%d\n", got, alt);
	return 0;
}
'''

# SPI0 loopback with random data at every mode, several speeds and sizes.
SPILOOP_C = r'''
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
int main(void)
{
	int fd = open("/dev/spidev0.0", O_RDWR), bad = 0, all = 0;
	if (fd < 0) { perror("spidev0.0"); return 1; }
	static uint8_t tx[4096], rx[4096];
	uint32_t hz[] = {100000, 1000000, 10000000, 25000000, 50000000};
	int sizes[] = {1, 7, 64, 4096};
	for (int m = 0; m < 4; m++) {
		uint8_t mode = m; ioctl(fd, SPI_IOC_WR_MODE, &mode);
		for (unsigned h = 0; h < 5; h++)
			for (unsigned s = 0; s < 4; s++) {
				int n = sizes[s], errs = 0;
				for (int i = 0; i < n; i++) tx[i] = rand();
				memset(rx, 0, n);
				struct spi_ioc_transfer t = { .tx_buf = (uintptr_t)tx, .rx_buf = (uintptr_t)rx, .len = n, .speed_hz = hz[h], .bits_per_word = 8 };
				if (ioctl(fd, SPI_IOC_MESSAGE(1), &t) < 0) { perror("SPI_IOC_MESSAGE"); return 1; }
				for (int i = 0; i < n; i++) errs += tx[i] != rx[i];
				bad += errs != 0; all++;
			}
	}
	printf("bad=%d of %d\n", bad, all);
	return 0;
}
'''


class Tester:
    """The pintester board's command port: one command, one reply ending in ok/err."""

    def __init__(self):
        ports = sorted(glob.glob("/dev/cu.usbmodempintest*"))
        if not ports:
            sys.exit("pin-test: no /dev/cu.usbmodempintest* (is ../pintester running on the second board?)")
        self.fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        a = termios.tcgetattr(self.fd)
        a[4] = a[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, a)

    def __call__(self, cmd, timeout=10):
        os.write(self.fd, (cmd + "\n").encode())
        buf, end = b"", time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                buf += os.read(self.fd, 4096)
            if re.search(rb"(^|\n)(ok|err [^\n]*)\n$", buf):
                break
        lines = buf.decode().strip().split("\n")
        if not lines or not lines[-1].startswith(("ok", "err")):
            raise RuntimeError(f"tester: no reply to {cmd!r}")
        if lines[-1].startswith("err"):
            raise RuntimeError(f"tester {cmd!r}: {lines[-1]}")
        return "\n".join(lines[:-1])

    def level(self, pin):
        return int(self(f"get {pin}").split("=")[1])

    def meas(self, pin, ms):
        return {k: float(v) for k, v in (kv.split("=") for kv in self(f"meas {pin} {ms}").split())}


class Linux:
    def __init__(self, host):
        self.host = host
        self.ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", f"root@{host}"]

    def run(self, cmd, timeout=120, stdin=None):
        r = subprocess.run(self.ssh + [cmd], input=stdin, capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).strip()

    def start(self, cmd, ready="ready"):
        """`cmd` in the background, after it prints its first line (starting
        with `ready`)."""
        p = subprocess.Popen(self.ssh + [cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        first = p.stdout.readline().strip()
        if not first.startswith(ready):
            p.kill()
            raise RuntimeError(f"board: {cmd!r} said {first!r}")
        return p


RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print(f"{'PASS' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}", flush=True)


def t_gpio(t, b):
    t("in 10 n")
    for v in (1, 0, 1, 0):
        b.run(f"killall gpioset 2>/dev/null; gpioset -z -c gpiochip0 13={v}")
        time.sleep(0.2)
        check(f"GP13 output {v}", t.level(10) == v)
    b.run("killall gpioset 2>/dev/null; true")
    for v in (1, 0, 1, 0):
        t(f"out 11 {v}")
        _, out = b.run("gpioget -c gpiochip0 14")
        check(f"GP14 input {v}", out.strip() == f'"14"={"active" if v else "inactive"}', out.strip())
    t("in 11 n")


def t_pull(t, b):
    b.run("killall gpiomon 2>/dev/null; (gpiomon -b pull-up -c gpiochip0 3 >/dev/null 2>&1 &); usleep 300000")
    t("out 13 0")
    time.sleep(0.05)
    t("in 13 n")
    time.sleep(0.3)
    check("GP3 pull-up raises a released line", t.level(13) == 1)
    b.run("killall gpiomon 2>/dev/null; true")


def t_edges(t, b):
    t("out 11 0")
    for n, us, must in ((40, 20000, True), (100, 5000, True), (200, 2000, False), (200, 1000, False)):
        p = b.start(f"/tmp/pin-test-gpiov2 edges 14 {n} 4")
        time.sleep(0.2)
        t(f"pulse 11 {n} {us}", timeout=10)
        out = p.communicate(timeout=30)[0].strip()
        got = int(re.search(r"events=(\d+)", out).group(1))
        alt = "alternating=1" in out
        name = f"GP14 edges, {n} at {us} us"
        if must:
            check(name, got == n and alt, out)
        else:
            print(f"INFO {name}: {out}")
        t("out 11 0")
        time.sleep(0.3)
    t("in 10 n")
    p = b.start("/tmp/pin-test-gpiov2 toggle 13 40 25")
    m = t.meas(10, 1100)
    p.communicate(timeout=30)
    print(f"INFO GP13 toggled every 25 ms: {m['hz']:.1f} Hz at the tester (20 expected)")


def t_pwm(t, b):
    t("in 12 n")
    b.run("cd /sys/class/pwm/pwmchip0 && { [ -d pwm15 ] || echo 15 > export; }")
    for period, duty in ((1000000, 250000), (100000, 50000), (10000000, 7500000), (1000, 500)):
        b.run(f"cd /sys/class/pwm/pwmchip0/pwm15 && echo 0 > enable; echo 0 > duty_cycle; echo {period} > period && echo {duty} > duty_cycle && echo 1 > enable")
        time.sleep(0.2)
        m = t.meas(12, 300)
        hz, pct = 1e9 / period, 100 * duty / period
        ok = abs(m["hz"] - hz) <= hz * 0.01 and abs(m["duty"] - pct) <= 2
        check(f"GP15 PWM {hz:.0f} Hz {pct:.0f}%", ok, f"{m['hz']:.1f} Hz {m['duty']:.1f}%")
    b.run("echo 0 > /sys/class/pwm/pwmchip0/pwm15/enable; echo 15 > /sys/class/pwm/pwmchip0/unexport")
    time.sleep(0.2)
    check("GP15 PWM off", t.meas(12, 100)["rises"] == 0)


def t_i2c(t, b):
    _, out = b.run("i2cdetect -y 0")
    check("i2cdetect finds 0x42", re.search(r"^40:.* 42 ", out, re.M) is not None)
    b.run("i2cset -y 0 0x42 0x05 0xab")
    check("i2cset byte", t("regs").split()[5] == "ab")
    _, out = b.run("i2cget -y 0 0x42 0x05")
    check("i2cget byte", out == "0xab", out)
    b.run("i2cset -y 0 0x42 0x08 0x1234 w")
    _, out = b.run("i2cget -y 0 0x42 0x08 w")
    check("i2c word", out == "0x1234", out)
    _, out = b.run("i2ctransfer -y 0 w5@0x42 0x20 0xde 0xad 0xbe 0xef && i2ctransfer -y 0 w1@0x42 0x20 r4@0x42")
    check("i2ctransfer, repeated start", out == "0xde 0xad 0xbe 0xef", out)
    rc, _ = b.run("i2cget -y 0 0x43 0x00")
    check("absent address fails", rc != 0)
    _, out = b.run('micropython -c "from machine import I2C; i=I2C(0); i.writeto_mem(0x42, 0x30, bytes([1,2,3])); print(i.scan(), i.readfrom_mem(0x42, 0x30, 3).hex())"')
    check("MicroPython I2C", out == "[66] 010203", out)


def t_spi(t, b):
    _, out = b.run("/tmp/pin-test-spiloop")
    check("SPI0 loopback (needs GP19-GP16 jumper)", out.startswith("bad=0 "), out)
    _, out = b.run('micropython -c "from machine import SPI; import os; s=SPI(0, baudrate=10_000_000); d=os.urandom(256); r=bytearray(256); s.write_readinto(d, r); print(r == d)"')
    check("MicroPython SPI", out == "True", out)


def t_mpy(t, b):
    t("in 10 n")
    p = b.start('micropython -c "import sys, time; from machine import Pin; p = Pin(13, Pin.OUT); p.value(1); print(\'ready\'); sys.stdout.flush(); time.sleep(1); p.value(0); time.sleep(1)"')
    time.sleep(0.3)
    hi = t.level(10)
    time.sleep(1.0)
    lo = t.level(10)
    p.communicate(timeout=30)
    check("MicroPython Pin out", (hi, lo) == (1, 0), f"{hi} then {lo}")
    t("out 11 0")
    p = b.start('micropython -c "import sys, time; from machine import Pin; n = []; p = Pin(14, Pin.IN); p.irq(lambda q: n.append(1), Pin.IRQ_RISING | Pin.IRQ_FALLING); print(\'ready\'); sys.stdout.flush(); time.sleep(3); print(len(n))"')
    time.sleep(0.2)
    t("pulse 11 20 20000")
    out = p.communicate(timeout=30)[0].strip()
    check("MicroPython Pin.irq, 20 edges at 20 ms", out == "20", out)
    t("in 12 n")
    p = b.start('micropython -c "import sys, time; from machine import Pin, PWM; w = PWM(Pin(15), freq=2000, duty_u16=16384); print(\'ready\'); sys.stdout.flush(); time.sleep(1.5); print(w.duty_u16()); w.deinit()"')
    time.sleep(0.3)
    m = t.meas(12, 500)
    out = p.communicate(timeout=30)[0].strip()
    check("MicroPython PWM 2 kHz 25%", abs(m["hz"] - 2000) < 20 and abs(m["duty"] - 25) < 2 and out == "16384",
          f"{m['hz']:.1f} Hz {m['duty']:.1f}%, duty_u16() {out}")


def t_pio(t, b):
    d = "/tmp/pin-test-pio"
    rc, out = b.run(f"rm -rf {d}; mkdir {d} && cd {d} && cp /usr/share/examples/pio/* . && "
                    "for p in squarewave pio-fifo pio-dma pio-edges; do cc -o $p $p.c -lpio || exit 1; done && "
                    "pioasm ws2812.pio ws2812.pio.h && cc -o ws2812 ws2812.c -lpio", timeout=300)
    check("PIO examples build (tcc, pioasm)", rc == 0, out)
    # square waves from PIO1 on GP13, measured by the tester; its pull-down
    # holds the line low while no program drives it (GP13 is back to an
    # unpulled GPIO between them after the earlier tests, and would float)
    t("in 10 d")
    for hz in (1000, 100000, 1000000):
        p = b.start(f"{d}/squarewave 13 {hz} 3", ready="GP13:")
        time.sleep(0.3)
        m = t.meas(10, 300)
        p.communicate(timeout=30)
        check(f"PIO square wave {hz} Hz on GP13", abs(m["hz"] - hz) <= hz * 0.01 and abs(m["duty"] - 50) <= 3,
              f"{m['hz']:.1f} Hz {m['duty']:.1f}%")
    # ws2812: 10 LEDs of 24 bits, a rise per bit
    p = b.start(f"sh -c 'echo ready; sleep 1; {d}/ws2812 13 10 ff00ff'")
    m = t.meas(10, 3000)
    p.communicate(timeout=30)
    check("ws2812 from pioasm's header: 240 bits on GP13", m["rises"] == 240, f"{m['rises']:.0f} rises")
    # every edge timestamped into DMA, where the GPIO events lose them
    t("out 11 0")
    for n, us in ((200, 1000), (2000, 20), (2000, 5)):
        p = b.start(f"{d}/pio-edges 14 {n} 8000")
        time.sleep(0.2)
        t(f"pulse 11 {n} {us}", timeout=20)
        out = p.communicate(timeout=30)[0].strip()
        m = re.search(r"(\d+) edges.*mean ([\d.]+) us", out)
        ok = m and int(m.group(1)) == n and abs(float(m.group(2)) - us) <= us * 0.05 + 0.1
        check(f"PIO edge capture, {n} at {us} us on GP14", bool(ok), out)
        t("out 11 0")
        time.sleep(0.3)
    t("in 11 n")
    rc, out = b.run(f"{d}/pio-fifo 2000")
    check("PIO FIFO round trips", rc == 0, out)
    rc, out = b.run(f"{d}/pio-dma 4096")
    check("PIO DMA echo and ring", rc == 0, out.replace("\n", "; "))
    # MicroPython's rp2
    p = b.start(f"sh -c 'echo ready; micropython {d}/pio_blink.py 13'")
    time.sleep(1.5)
    m = t.meas(10, 1000)
    p.communicate(timeout=30)
    check("MicroPython rp2.StateMachine blink on GP13 (23.7 Hz)", abs(m["hz"] - 100000 / 4224) < 1, f"{m['hz']:.2f} Hz")
    rc, out = b.run(f"micropython {d}/pio_echo.py")
    check("MicroPython rp2 put/get and DMA", "4 of 4" in out and "256 of 256" in out, out.replace("\n", "; "))
    rc, out = b.run(f"micropython {d}/pio_ring.py")
    check("MicroPython rp2 endless DMA ring", "after 0.5 s: True" in out and "15 of 16" in out, out.replace("\n", "; "))


TESTS = {"gpio": t_gpio, "pull": t_pull, "edges": t_edges, "pwm": t_pwm, "i2c": t_i2c, "spi": t_spi, "micropython": t_mpy,
         "pio": t_pio}


def board_address():
    r = subprocess.run([sys.executable, os.path.join(HERE, "board-cmd.py"), "--timeout", "30", "ifconfig eth0"],
                       capture_output=True, text=True)
    m = re.search(r"inet addr:(\S+)", r.stdout)
    if not m:
        sys.exit("pin-test: no address for the board's eth0 (give --host)")
    return m.group(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", help="the Linux board's address (default: asked over its console)")
    ap.add_argument("--only", help="comma-separated: " + ",".join(TESTS))
    a = ap.parse_args()
    t = Tester()
    check("tester answers", t("id") == "pintester")
    b = Linux(a.host or board_address())
    for name, src in (("gpiov2", GPIOV2_C), ("spiloop", SPILOOP_C)):
        rc, out = b.run(f"cat > /tmp/pin-test-{name}.c && tcc -o /tmp/pin-test-{name} /tmp/pin-test-{name}.c", stdin=src)
        check(f"tcc builds {name}", rc == 0, out)
    for name in (a.only.split(",") if a.only else TESTS):
        TESTS[name](t, b)
    b.run("rm -f /tmp/pin-test-*")
    print(f"{sum(RESULTS)} of {len(RESULTS)} passed")
    return 0 if all(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
