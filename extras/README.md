# extras

On-device C programs, demos, and utilities archived from `/mnt/sd/rp2350-linux` on the RP2350 Linux board. Everything here is designed to build directly on the board with TinyCC (`cc` / `tcc`).

Compiled binaries, object files, and runtime state (`data/` logs and test uploads) are excluded; most directories include a build script, while `noice` and `ws2812` build with a single `cc` command.

## Inventory

| Directory | What | How to build & run on the board |
|---|---|---|
| [`bbs/`](bbs/) | **RP2350 BBS** — multi-node telnet BBS (`bbs.c`, `bbs.conf`, `welcome.txt`) with message boards, chat, XMODEM/YMODEM file transfers, and a built-in RP2350-themed MUD (`mud/*.txt`: `start`, `bus`, `core0`, `core1`, `flash`, `gpio`, `pio`, `sram`, `items`, `npcs`). | `./bbsctl build` and `./bbsctl start` (listens via `inetd` on `telnet_port` from `bbs.conf`). Note: stop the default `/usr/sbin/telnetd` or change `telnet_port` if port 23 is already in use. |
| [`demos/`](demos/) | **Terminal demos & games** — `asteroids` (vector Asteroids), `stackfall` (falling-blocks game), `life` (Conway's Game of Life), `rain` (digital rain), `pulse` (live system monitor: CPU, memory, network, processes), `mandel` (Mandelbrot set), `bogomips` (CPU loop benchmark), `httpd` (small HTTP server), shared `term.h`, and `tools/vtshot.c` (VT100 screen-capture helper). | `./build.sh` (or `./build.sh stackfall pulse ...`); `cc -o mandel mandel.c`, `cc -o httpd httpd.c`, `cc -o tools/vtshot tools/vtshot.c`. |
| [`mc/`](mc/) | **mc** — dependency-free Midnight Commander lookalike (~6,200 lines of C, no curses or glib) with twin panels, built-in viewer, dialogs, pull-down menus, and user menu. See [`mc/README.md`](mc/README.md). | `./build.sh && ./mc` |
| [`tm/`](tm/) | **tm** — lightweight `tmux` work-alike (~80 KB binary, ~600 KB server RAM) with detachable sessions, multiple windows, compressed scrollback, copy mode, and mouse support. See [`tm/README.md`](tm/README.md). | `./build.sh && ./tm` |
| [`nnn/`](nnn/) | **nnn** — port of the `nnn` terminal file manager adapted to build on-device with `tcc` against narrow `libncurses` (using `compat/` shims and headers from `../noice/include`). | `./build.sh && ./nnn` |
| [`noice/`](noice/) | **noice** — minimal `curses`-based file browser with bundled `ncurses` headers (`include/`) and `strlcpy`/`strlcat` shims for `tcc`. | `cc -Iinclude -o noice noice.c strlcat.c strlcpy.c -lncurses && ./noice` |
| [`ws2812/`](ws2812/) | **WS2812 PIO demo** — drives NeoPixel/WS2812 LEDs on a GPIO pin using RP2350 PIO via `libpio` (`ws2812.pio`, `ws2812.pio.h`, `ws2812.c`). | `pioasm ws2812.pio ws2812.pio.h && cc -o ws2812 ws2812.c -lpio && ./ws2812 PIN COUNT RRGGBB` |

## Provenance & licensing

- **`bbs/`, `demos/`, `mc/`, `tm/`:** Original C programs written for this project's RP2350 Linux board (under the repository's MIT license).
- **`nnn/`:** Third-party port of upstream `nnn` (BSD-2-Clause, see [`nnn/LICENSE`](nnn/LICENSE)) with local `tcc` / narrow-ncurses compatibility shims (`compat/fts.h`, `compat/narrowcurses.h`, `build.sh`).
- **`noice/`:** Third-party port of upstream `noice` (BSD-2-Clause, see [`noice/LICENSE`](noice/LICENSE)) with local `strlcat.c`/`strlcpy.c` shims and bundled `ncurses` headers (`include/`, MIT-X11) for on-device `tcc` builds.
- **`ws2812/`:** `ws2812.pio` and generated `ws2812.pio.h` are derived from `raspberrypi/pico-examples` (`pio/ws2812/ws2812.pio`, Copyright (c) 2020 Raspberry Pi (Trading) Ltd., BSD-3-Clause); `ws2812.c` is a small `libpio` driver wrapper.

## Copying back to the board

With the board on Wi-Fi (e.g. `192.168.4.30`) and `/mnt/sd` mounted:

```sh
tar cf - -C extras . | ssh root@192.168.4.30 'mkdir -p /mnt/sd/rp2350-linux && tar xf - -C /mnt/sd/rp2350-linux'
```
