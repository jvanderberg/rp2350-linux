# wolf3d

Wolfenstein 3D on the board, in a window on an X server such as XQuartz on a
Mac. It is [Wolf4SDL](PROVENANCE), the SDL port of id Software's released
source, with SDL replaced by `xwolf.c`, which talks to the X server through
[`../demos/x11.h`](../demos/x11.h). No Xlib, no SDL and no sound.

With the shareware episode at 640x400 it runs at about 45 frames a second,
sending about 8 KB a frame, and uses about 1.3 MB of the board's memory.

## Getting the game data

The data isn't in this repository. On the Mac (it needs curl, unzip and
7-Zip, `brew install sevenzip`):

```
$ ./fetch-shareware.sh
```

This downloads the shareware, version 1.4, checks its MD5 and puts the eight
`*.wl1` files in `data/`. The shareware is free to copy as it comes; see
`data/READ1ST.TXT`.

## Building and running

Put `extras/` on the board as in the [extras README](../README.md) (the build
needs `../demos/x11.h`), then the data next to the program:

```
$ scp data/*.wl1 root@BOARD:/mnt/sd/extras/wolf3d/
```

On the board, build it (about a minute):

```
# cd /mnt/sd/extras/wolf3d && ./build.sh
```

On the Mac, allow network clients (XQuartz: Settings > Security) and run
`xhost +BOARD`. Then on the board, from the same directory, since the game
looks for its data there:

```
# DISPLAY=MAC:0 ./xwolf
```

Arrows move and turn, Ctrl fires, Space opens doors, Shift runs, Alt
(Option on a Mac) with the arrows strafes, 1-4 pick a weapon, Esc is the
menu. The window can be resized: the picture is scaled by a whole number to
fit.

Settings and saved games go in `$HOME/.wolf4sdl`; `--configdir DIR` puts
them elsewhere, such as on the SD card. `--nowait` skips the opening screens.

## How it draws

Sending the picture as pixels would take 256 KB a frame, a few frames a
second at best over the board's network. So the X server keeps the
pictures, and the board sends what to do with them:

- **The 3D view** never reaches the engine's 8-bit buffer. Hooks in
  `wl_draw.c` and `wl_scale.c` give `xwolf.c` what the raycaster found for
  each column. Each wall texture is uploaded once, and the server keeps it
  scaled to each height it has been seen at; a column of wall is then one
  28-byte copy. The weapon, and anything seen at the same size two frames
  running, is kept on the server with a mask, and drawn with one masked copy.
- **Everything else** (menus, the status bar, intermissions) goes as
  rectangles of one colour, for the pixels that changed since the last
  frame.
- **Flashes** when you are hit or pick something up are a dither drawn over
  the picture, instead of a palette change that would mean sending all of it
  again.

`XW_STATS=1 ./xwolf` prints, when you quit, the frame rate and where each
frame's bytes and time went. `XW_SOFT=1` draws the 3D view with the engine's
own renderer instead, as pixels: slow, but useful for comparing.

To fit the board's memory, VSWAP pages and graphics are read from their
files when needed instead of all at startup ([PROVENANCE](PROVENANCE) lists
every change to the engine).

## License

The engine is Wolf4SDL's, under the GNU GPL version 2 ([LICENSE-GPL-2.0](LICENSE-GPL-2.0));
see [PROVENANCE](PROVENANCE) for where it came from and what changed.
`xwolf.c`, `SDL.h` and the scripts are MIT, like the rest of the repository.
The game data is id Software's and is not included.
