# tm - a small terminal multiplexer

A tmux work-alike for small boards: detachable sessions, several windows
with a status line, compressed scrollback with copy mode, and the mouse.
About 80 KB, and a session's server uses roughly 600 KB of RAM.

    tm                     attach to session "main", starting it if needed
    tm new [-d] [-s NAME]  start a session (-d: in the background)
    tm attach [-t NAME]    attach (also: tm a)
    tm ls                  list sessions
    tm kill [-t NAME]      end a session

## Keys

Press the prefix **Ctrl-B**, then:

| key   | action                  | key   | action            |
|-------|-------------------------|-------|-------------------|
| c     | new window              | d     | detach            |
| n / p | next / previous window  | 0-9   | go to window      |
| l     | last window             | ,     | rename window     |
| & / x | kill window             | r     | redraw            |
| [     | copy mode (PgUp too)    | ]     | paste             |
| m     | mouse on / off          | ?     | help              |
| C-b   | send Ctrl-B             |       |                   |

Copy mode: arrows or hjkl, PgUp/PgDn, C-u/C-d, g/G for top/bottom, 0/$,
/ and ? to search down/up, n/N to repeat, Space or v to start a
selection, Enter to copy it, q or Esc to leave.

## Mouse

- Click a window name in the status line to switch to it; the wheel
  over the status line cycles windows.
- The wheel scrolls back through history (copy mode; scrolling back to
  the bottom leaves it).
- A left drag selects text; releasing copies it. The text is kept for
  C-b ] and sent to the terminal's clipboard with OSC 52.
- Programs that ask for the mouse (mc, vim with mouse=a) get it instead.
  Hold Shift to select with the terminal's own selection.
- Full-screen programs without mouse support (less, man) get arrow keys
  from the wheel.

C-b m (or TM_MOUSE=0) turns tm's mouse handling off.

## Scrollback

History is stored as text with attribute escapes, packed into 4 KB
blocks and LZ4-compressed (typical shell output: about 8 bytes a line).

- Blocks go to a ring file in `$TM_HISTORY_DIR` (default `/mnt/sd/.tm`),
  `$TM_HISTORY_KB` per window (default 2048). That is about 250,000
  lines, on the SD card rather than in RAM.
- The file is unlinked as soon as it is opened, so nothing is left
  behind.
- If no directory is usable (or `TM_HISTORY_DIR=none`), 64 KB per window
  is kept compressed in RAM instead.

## Inside

Programs see `TERM=screen-256color` and `$TM` (the socket path).

| file             | what                                                    |
|------------------|---------------------------------------------------------|
| main.c           | command line, starting the server                       |
| client.c         | the attached terminal: raw mode, keys in, output out    |
| server.c         | windows, keys, mouse, status line, drawing              |
| vt.c             | the terminal emulator (one per window)                  |
| render.c         | sends only the cells that changed to the real terminal  |
| copy.c           | copy mode                                               |
| sb.c             | scrollback storage (ring file or RAM)                   |
| lz.c             | LZ4-format block compressor                             |
| keys.c           | key and mouse report decoding                           |
| proto.c          | client/server message framing                           |
| util.c           | buffers, UTF-8, allocation                              |

Each session is one background server on the socket `/tmp/tm-UID/NAME`.
The client and server exchange frames on it: a type byte, a 2-byte
length, then the data.

- Client to server: `I` keys, `R` size.
- Server to client: `O` output, `D` detached, `X` exited.

A new client's first frame decides what it wants: `R` attaches,
replacing any attached client, and `X` kills the session.

## Building

    ./build.sh             # tcc, one file at a time (fits the board)
    CC=cc ./build.sh       # any other compiler
