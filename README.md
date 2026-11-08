# rp2350-linux

Linux 6.12 on an RP2350B board. The RP2350's Hazard3 RISC-V cores have no
MMU, so they can't run Linux themselves: one of them runs an emulated
RISC-V CPU that has one, and the kernel runs on that. The guest has the
board's 8 MB PSRAM as RAM, a root filesystem in flash, `/home` on flash,
a USB serial console and Wi-Fi networking.

```
rp2350 login: root
# uname -a
Linux rp2350 6.12.116 #29 Sun Nov  8 12:56:58 UTC 2026 riscv32 GNU/Linux
```

## What's included

| | Default | How to use it |
|---|---|---|
| [USB console and Wi-Fi](#console-and-network) | on | `screen /dev/cu.usbmodemrvemu1 115200`; `eth0` |
| [`/home` and `/root` on flash](#home-on-flash) | on | files there are kept across reboots |
| [C compiler](#c-compiler) | on | `cc`, `tcc -run`, `tcc -ar` |
| [Swap on flash](#swap) | off | `scripts/pico-shell.sh -w` |
| [GPIO, I2C, SPI, PWM, ADC](#pins) | off | `PINS=1 guest/run-build.sh disk` |

The guest also has nano, mg, BusyBox `vi`, MicroPython, Lua 5.4, htop,
tmux, lrzsz, bc, ncdu, lsof and less. Log in as root, no password.

In the examples, lines starting with `#` are in the board's shell, and
lines starting with `$` on the Mac the board is plugged into.

## Hardware

- An RP2350B board with 8 MB APS6404L PSRAM on QMI CS1 (GP0) and 16 MB of
  flash.
- An RM2 (CYW43439) Wi-Fi module on the Pico 2 W pins: GP23 WL_ON, GP24
  data/IRQ, GP25 CS, GP29 CLK. Optional: without it `eth0` stays down.
- The board's USB to the Mac, for loading and the console.
- Optional: an SWD probe (a second RP2350 running debugprobe), for recovery.

## Prerequisites (macOS)

- Rust stable with the `riscv32imac-unknown-none-elf` and
  `riscv32im-unknown-none-elf` targets.
- Docker, for the guest build.
- `dtc`, `picotool` and `screen`.
- probe-rs from master, in `~/git/toolchains/probe-rs-master` (override with
  `PROBE_RS=`), only for when the board can't be put into BOOTSEL over USB.
  See [rp2350/README.md](rp2350/README.md).

## Quickstart

```sh
export BOARD_LOCK_OWNER=$USER

# 1. Build the guest into guest/out-disk (Docker).
cd guest && ./run-build.sh disk && cd ../rvemu-rs

# 2. Store the Wi-Fi network in the board's flash (asks for the passphrase).
../rp2350/tools/board-lock acquire $USER -l 120
scripts/pico-wifi.sh "My Network"

# 3. Load the guest and the firmware over USB, and boot.
#    -b rebuilds the guest first, -w adds swap.
scripts/pico-shell.sh

# 4. Connect. Log in as root, no password. Ctrl-A k quits screen.
screen /dev/cu.usbmodemrvemu1 115200
```

`pico-shell.sh` refuses to run while another program (`screen`, say) has
the console port open. Running it again reboots the board.

## Console and network

The console is the board's USB serial port. A terminal opened after boot
shows the end of the boot log. `eth0` joins the Wi-Fi network stored with
`scripts/pico-wifi.sh` and takes its address by DHCP, in the background.
There is no syslog or cron: `dmesg` has the kernel log.

**Example:**

```
# ifconfig eth0 | grep "inet "
          inet addr:192.168.4.171  Bcast:192.168.7.255  Mask:255.255.252.0
# ping -c 10 192.168.4.1 | tail -1
round-trip min/avg/max = 0.915/1.054/1.311 ms
# wget -q -O /tmp/1m.bin http://192.168.4.47:8771/1m.bin; md5sum /tmp/1m.bin
5c2b2733524d557484177d013844e229  /tmp/1m.bin
```

## /home on flash

`/home` is a 1 MB filesystem on flash, and `/root` lives in it
(`/home/root`). Files there are kept across reboots and power cuts. Every
other change to the filesystem is lost at reboot.

When `/home` (or `/tmp`) is full, BusyBox tools say "I/O error", as in
`head: standard output: I/O error`.

**Example:**

```
# head -c 65536 /dev/urandom > /home/docs-test.bin; md5sum /home/docs-test.bin > /home/docs-test.md5; sync
$ scripts/pico-shell.sh          # reboot the board
# md5sum -c /home/docs-test.md5
/home/docs-test.bin: OK
```

## C compiler

`cc` (TinyCC) compiles, assembles and links C on the board, with musl's
headers and the kernel's device headers (`linux/gpio.h`, `i2c-dev.h`,
`spi/spidev.h`, ...).

- `cc hello.c && ./a.out`, `cc -c`, `cc -o prog a.c b.c`
- `tcc -run file.c ARGS`, or `#!/usr/bin/tcc -run` as a script's first line
- `tcc -ar rcs libfoo.a a.o b.o` for static libraries

Programs link dynamically (there is no `-static`), and there is no `make`.
When memory runs short, tcc stops with "memory full"; build large programs
a file at a time with `cc -c`.

**Example:**

```
# printf '#include <stdio.h>\nint main(void) { printf("hello from tcc\\n"); return 0; }\n' > hello.c
# cc hello.c && ./a.out
hello from tcc
# tcc -run hello.c
hello from tcc
```

**Example:** a static library.

```
# printf 'int sq(int x) { return x * x; }\n' > sq.c
# printf 'int cube(int x) { return x * x * x; }\n' > cube.c
# printf '#include <stdio.h>\nint sq(int), cube(int);\nint main(void) { printf("%%d %%d\\n", sq(7), cube(3)); return 0; }\n' > main.c
# cc -c sq.c cube.c && tcc -ar rcs libm2.a sq.o cube.o && tcc -ar t libm2.a
sq.o
cube.o
# cc -o calc main.c -L. -lm2 && ./calc
49 27
```

## Swap

`scripts/pico-shell.sh -w` boots the guest with 1788 KiB of swap on flash
(`/dev/vdb`). It is enabled at boot and empty at every boot.
`flashswap-stats` prints the swap device's counters.

**Example:**

```
$ scripts/pico-shell.sh -w
# cat /proc/swaps
Filename				Type		Size		Used		Priority
/dev/vdb                                partition	1788		0		-2
# lua -e 't={} for i=1,40000 do t[i]=string.rep("x",40)..i end print(#t)'; free | tail -1
40000
Swap:          1788         116        1672
```

## Pins

A `PINS=1` build of the guest gets the board's pins through the standard
Linux interfaces. Build it, then load it with `scripts/pico-shell.sh` as
usual:

```
cd guest && PINS=1 ./run-build.sh disk
```

| Linux | Board |
|---|---|
| `/dev/gpiochip0` (lines 0-47 = GP0-GP47) | free pins GP1-GP3, GP8-GP15, GP20-GP22, GP26-GP28, GP30-GP39; pull-up/pull-down/none, rising/falling edge events |
| `/dev/i2c-0`, `/dev/i2c-1` | I2C0 on GP4 (SDA) / GP5 (SCL), I2C1 on GP6 / GP7; 100 kHz, internal pull-ups |
| `/dev/spidev0.0` | SPI0: GP16 MISO, GP17 CS, GP18 SCK, GP19 MOSI; 100 kHz to 50 MHz, modes 0-3, 8-bit |
| `/sys/class/pwm/pwmchip0` (channel N = GPN) | any free GPIO below 40; pins on one PWM slice share its period |
| `/sys/bus/iio/devices/iio:device0` | `in_voltage0..7` = GP40-GP47 (mV = raw x `in_voltage_scale`), `in_temp` (the chip's sensor, m°C = (raw + offset) x scale) |

GP0 (PSRAM) and GP23/24/25/29 (Wi-Fi) are not available; `gpioinfo` names
every line. The build includes:

- libgpiod tools (`gpiodetect`, `gpioinfo`, `gpioget`, `gpioset`,
  `gpiomon`), i2c-tools (`i2cdetect`, `i2cget`, `i2cset`, `i2ctransfer`)
  and `spidev_test`;
- MicroPython's `machine` module with `Pin` (and `Pin.irq()`), `I2C`,
  `SPI`, `PWM` and `ADC` (`ADC(Pin(40))`, `ADC(ADC.CORE_TEMP).read_temp()`);
- C examples, built and as source, in `/usr/share/examples/pins`:
  `gpio-blink`, `gpio-edge`, `i2c-scan`, `spi-xfer`, `adc-read`. Build
  them with `cc -o gpio-blink gpio-blink.c`.

**Example** (nothing wired to the pins):

```
# cd /usr/share/examples/pins
# ./gpio-blink 15 3 100 | head -2
GP15 set 1, pad reads 1
GP15 set 0, pad reads 0
# ./gpio-edge 14 4
pull-up: rising edge on GP14 (seqno 1)
pull-down: falling edge on GP14 (seqno 2)
pull-up: rising edge on GP14 (seqno 3)
pull-down: falling edge on GP14 (seqno 4)
4 of 4 edges
# ./adc-read | tail -1
temp raw  877  26.7 C
# micropython -c "from machine import ADC; print(ADC(ADC.CORE_TEMP).read_temp())"
26.67292789499999
# cd /sys/class/pwm/pwmchip0 && echo 15 > export && cd pwm15 &&
  echo 1000000 > period && echo 250000 > duty_cycle && echo 1 > enable   # 1 kHz, 25% on GP15
# mkdir /tmp/b && cd /tmp/b && cp /usr/share/examples/pins/gpio-blink.c . &&
  cc -o gpio-blink gpio-blink.c && ./gpio-blink 15 2 50
GP15 set 1, pad reads 1
GP15 set 0, pad reads 0
GP15 set 1, pad reads 1
GP15 set 0, pad reads 0
```

## Flash map (16 MB)

| Address | Contents | Written by |
|---|---|---|
| 0x10000000 | firmware | `pico-shell.sh` |
| 0x10100000 | `/home`, 1 MB | the guest |
| 0x10200000 | swap, up to 0x103FEFFF | the guest, only with `pico-shell.sh -w` |
| 0x103FF000 | Wi-Fi settings, 4 KB | `scripts/pico-wifi.sh` only |
| 0x10400000 | guest blob, at most 2 MB: kernel, initramfs, device tree | `pico-shell.sh` |
| 0x10600000 | root filesystem (squashfs) | `pico-shell.sh` |
| 0x11000000 − text size | kernel text, at most 3.5 MiB, ending at the end of flash | `pico-shell.sh` |

Never write 0x103FF000 with picotool or SWD: it holds the Wi-Fi
passphrase. `pico-shell.sh` writes only the regions whose contents changed
(`FORCE_BLOB=1` writes them all).

## Layout

| Path | What |
|---|---|
| `guest/` | the Linux guest: kernel config, root filesystem, the guest's own drivers and packages ([README](guest/README.md)) |
| `rvemu-rs/` | the emulator and the board firmware, and the scripts that build, load and test them ([README](rvemu-rs/README.md)) |
| `rp2350/` | board support and host tools ([README](rp2350/README.md)) |
| `rvemu/` | the C reference emulator ([README](rvemu/README.md)) |
