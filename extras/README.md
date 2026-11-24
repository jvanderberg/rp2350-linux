# extras

Programs for the board that aren't part of the image: games and demos, a
telnet BBS, two file managers, a Midnight Commander lookalike, a terminal
multiplexer and Wolfenstein 3D. They are built on the board with `cc`.

| Directory | What it is | Build and run |
|---|---|---|
| [`demos/`](demos/) | Terminal games and demos | `./build.sh`, then `./asteroids` |
| [`wolf3d/`](wolf3d/README.md) | Wolfenstein 3D in an X11 window | see its README |
| [`bbs/`](bbs/) | A telnet BBS with a MUD | `./bbsctl start` |
| [`mc/`](mc/README.md) | A Midnight Commander lookalike | `./build.sh && ./mc` |
| [`tm/`](tm/README.md) | A small tmux work-alike | `./build.sh && ./tm` |
| [`nnn/`](nnn/) | The nnn file manager | `./build.sh && ./nnn` |
| [`noice/`](noice/) | The noice file browser | see below |
| [`ws2812/`](ws2812/) | WS2812 LEDs driven by PIO | see below |

In the examples, lines starting with `#` are in the board's shell, and
lines starting with `$` on the Mac.

## Putting them on the board

`/home` holds only 1 MB, so use the SD card. With the card mounted at
`/mnt/sd`:

```
$ tar cf - -C extras . | ssh root@ADDRESS 'mkdir -p /mnt/sd/extras && tar xf - -C /mnt/sd/extras'
```

`nnn` uses the curses headers in `noice/include`, so copy `noice/` with it.

## demos

`asteroids`, `stackfall` (falling blocks), `life`, `rain` (digital rain),
`pulse` (CPU, memory, network and processes), `bogomips`, `maze` (a
first-person 3D maze; `./maze -demo` explores it by itself), `mandel` and a
small `httpd`. `./build.sh` builds the first seven and the three X11 programs
below, or only the ones named (`./build.sh life rain`). Build the others with
`cc`:

```
# cc -o mandel mandel.c
# cc -o httpd httpd.c
```

`xeyes`, `xmaze` and `xpulse` open a window on an X server instead of using
the terminal, such as XQuartz on a Mac. `xmaze` is the maze drawn as X shapes
rather than pixels, about 1 KB a frame, so it stays fast over the board's
network link and sharp at any window size; it takes `-demo` too. `xpulse` is
`pulse` in a window, with graphs, a process table sorted by clicking a
column, and a right-click menu to signal a process; an update sends only
what changed, 1-4 KB. All three use `x11.h`, which speaks the X11 protocol
itself, so no Xlib is needed; it is the window counterpart of `term.h`. On the machine with the screen, allow
network clients (XQuartz: Settings > Security) and run
`xhost +BOARD-ADDRESS`, then on the board:

```
# DISPLAY=SCREEN-ADDRESS:0 ./xmaze -demo
```

## bbs

A telnet BBS with message boards, chat, XMODEM and YMODEM transfers, and a
MUD set inside the RP2350 (`mud/*.txt`). `./bbsctl start` builds it and
starts `inetd` on `telnet_port` from `bbs.conf`; `./bbsctl stop` ends it.
Port 23 is the board's own telnet, so stop that or pick another port. Run
`./bbsctl start` again after each reboot.

## noice

```
# cc -Iinclude -o noice noice.c strlcat.c strlcpy.c -lncurses && ./noice
```

## ws2812

The same program as `ws2812` in `/usr/share/examples/pio`:

```
# pioasm ws2812.pio ws2812.pio.h && cc -o ws2812 ws2812.c -lpio
# ./ws2812 PIN COUNT RRGGBB
```

## Licenses

`demos/`, `bbs/`, `mc/` and `tm/` are under the repository's MIT license.

- `wolf3d/` is a port of Wolf4SDL, under the GNU GPL version 2 (see its
  `PROVENANCE` and `LICENSE-GPL-2.0`); its own additions, `xwolf.c`,
  `SDL.h` and the scripts, are MIT. The game data is not included.
- `nnn/` and `noice/` are ports of upstream nnn and noice (BSD-2-Clause,
  see their `LICENSE` files), with small shims so they build with tcc.
  `noice/include` holds ncurses headers (MIT-X11).
- `ws2812/ws2812.pio` comes from the pico-examples (Copyright (c) 2020
  Raspberry Pi (Trading) Ltd., BSD-3-Clause).
