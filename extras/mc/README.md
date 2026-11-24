# mc — a Midnight Commander lookalike

A small two-panel file manager for the RP2350 Linux board. It is laid out and
coloured like GNU Midnight Commander's default skin (checked side by side
against mc 4.8.33), but it is a separate program of about 6,200 lines of commented C
with no dependencies: no curses, no glib, no subshell.

    ./build.sh          # build on the board with tcc
    ./mc [left [right]] # start, optionally in two directories

## What it does

| Key | Action |
| --- | --- |
| Up/Down, PgUp/PgDn, Home/End | move the cursor |
| Tab | switch panels |
| Enter | open a directory; run a program (ELF or `#!`); view anything else |
| Insert, C-t | mark/unmark a file and move down |
| `+` `-` `*` | mark by pattern / unmark by pattern / invert (when the command line is empty) |
| F1 | help |
| F2 | user menu (see below) |
| F3 | view the file in the built-in viewer (see below) |
| F4 | edit with `$EDITOR` (default `vi`) |
| F5 / F6 | copy / move-rename the marked files (or the current one) |
| F7 | make a directory (`a/b/c` creates the parents too) |
| F8 | delete, asking before going into a non-empty directory |
| F9 | pull-down menus (sort order, options, etc.) |
| F10 | quit |
| C-s | quick search: type the start of a name; C-s again finds the next match |
| C-r | rescan the panel |
| C-u | swap the panels |
| C-o | show the output of the last command |
| C-x d | compare directories: marks files that are missing or newer on one side |
| C-PgUp / C-PgDn | parent directory / into the directory under the cursor |
| M-. | show/hide dot files |
| M-o / M-i | other panel to the directory under the cursor / to this directory |
| M-Enter | put the current name on the command line |
| Esc 1 … Esc 0 | same as F1 … F10, for terminals that eat the function keys |

Anything else you type goes to the command line at the bottom; Enter runs it
with `/bin/sh` in the active panel's directory, and `cd` is handled
internally. The mouse works too: click to select, double-click to open,
right-click to mark, the wheel scrolls, and the menu and key bars are
clickable.

Copy, move and delete are done in-process (no `cp`/`rm` children). They
recurse into directories, keep modes, modification times and symlinks, show a
progress box (Esc aborts), and ask before overwriting
(Yes/No/All/None/Abort). Errors offer Skip or Abort.

## Viewer (F3)

The built-in viewer looks like mc's: a status line with the file name,
position and percentage, then the text wrapped to the screen width.

| Key | Action |
| --- | --- |
| Up/Down, PgUp/PgDn, Space, Home/End | scroll |
| Left/Right | scroll sideways (when unwrapped) |
| F2 | wrap / unwrap long lines |
| F4 | hex dump / text |
| F5 | go to a line number (in hex: an offset; `0x` for hex) |
| F7 or `/`, then `n` | search (ignoring case), next match |
| F9 | Markdown formatting on/off |
| F3, F10, q, Esc | back to the panels |

Markdown formatting is on by default for `*.md` files. It renders `#`
headings, **bold**, *italic*, `code` spans, fenced and indented code
blocks, bullets and numbered lists, task boxes, `>` quotes, `---` rules,
links (text, then the URL in grey) and `|` tables with box-drawing lines.

The file is memory-mapped and its lines are indexed only as far as you
scroll, so even a big log opens instantly. Options > Use internal viewer
switches F3 to `$PAGER` (default `less`) instead.

## User menu (F2)

F2 shows a list of shell snippets for the file under the cursor. Pick one
with the arrow keys and Enter, or press its hotkey. The list comes from
the first of `.mc.menu` in the current directory, `~/.config/mc/menu`
and `~/.mc/menu`. If none exists, a built-in menu is used: compile or
compile-and-run a `.c` file with tcc, run `./build.sh`, extract or create
archives, gzip/gunzip, hex dump, md5sum, wc, disk usage, find, grep, and
scp to another machine.

The format is mc's:

```
shell_patterns=1

+ f *.c
c       Compile with tcc
        tcc -Wall -o %b %f
```

A `+` line is a condition for the entry after it. Tests are joined left
to right with `&` and `|`, and `!` negates one:

- `f pattern` matches the file name
- `d pattern` matches the directory
- `t` followed by type letters matches the entry type: `r` file, `d` directory, `l` link, `x` executable, `t` files are marked
- `x file` is true if that file is executable

The uppercase forms `F`, `D` and `T` test the other panel. Patterns are
shell wildcards, or regular expressions after `shell_patterns=0`.

Commands are the indented lines. These macros are substituted, with file
names shell-quoted:

- `%f` the current file, `%b` its name without the extension, `%x` the extension
- `%d` the current directory
- `%s` the marked files, or the current one if none is marked
- `%t` the marked files only; `%u` the same, and unmarks them afterwards
- `%F %D %S %T` the same for the other panel
- `%{prompt}` asks for text first
- `%%` a literal `%`

The full description is in `usermenu.h`.

## Source layout

| File | Purpose |
| --- | --- |
| `main.c` | screen layout, key bindings, commands, menus, help |
| `panel.c/.h` | reading and sorting a directory panel; marks; quick search |
| `panel_draw.c` | drawing a panel on the terminal (mc only) |
| `dialog.c/.h` | dialog boxes: labels, input fields, buttons, progress gauge |
| `menu.c/.h` | menu bar and pull-down menus |
| `fileops.c/.h` | copy, move, delete and mkdir, with their dialogs |
| `viewer.c/.h` | the F3 viewer: text, hex, search, Markdown rendering |
| `usermenu.c/.h` | the F2 user menu: reading the menu file, conditions, macros |
| `run.c/.h` | running the editor, typed commands and user menu entries |
| `screen.c/.h` | cell buffer; sends only the changed cells to the terminal |
| `keys.c/.h` | turns escape sequences (xterm, Linux console, vt100) and mouse reports into key codes |
| `util.c/.h` | UTF-8, number and date formatting, path helpers |
| `skin.h` | every colour, in one place |
| `xmc.c` | xmc: panels, dialogs, menus, viewer and output view in an X11 window |

The main loop redraws the whole screen into a back buffer after every key, and
`screen.c` compares it with the previous frame and writes only the
difference. Code that draws never has to track what is already on the
terminal, and a key press usually costs a few dozen bytes on the wire.

## Colours

Colours go out as xterm-256 indices chosen to match the classic VGA palette.
The panel blue is a deep `#0000af`, whatever blue the terminal theme uses for
ANSI colour 4. To change it, edit `SKIN_PALETTE` in `skin.h`, or set
`MC_16COLORS=1` to use the terminal's own 16 colours.

## xmc: mc in an X11 window

`./build.sh xmc` builds `xmc`, which shares mc's panels, file operations,
dialogs and user menu but draws them in an X11 window (for XQuartz on a
Mac, say), in the style of `../demos/xpulse`:

    DISPLAY=HOST:0 ./xmc [left-dir [right-dir]]

- **Mouse:** click to select, double-click to open; Ctrl-click marks one,
  Shift-click a range, and Ctrl- or Shift-drag the rows passed over. Drag
  files to the other panel to copy them (Shift: move), onto a folder to put
  them there, or onto the command line to type their names. Right-click
  for a menu with everything F2–F8 do, plus the user menu's entries.
- **Keys:** as in mc. C-n opens a shell, C-o the latest terminal tab,
  Ctrl-Tab the next tab.
- **Tabs.** The bar along the top holds files tabs and terminal tabs. `+`
  opens another files tab where this one is; each then goes its own way.
  The gear holds the settings (dot files). When the tabs no longer fit,
  all but the current one shrink to their icons, and the one under the
  pointer opens up to show its name; the wheel over the bar goes through
  them. A tab's close button, or the middle button, closes it.
- **Commands run in terminal tabs.** A typed command, a program opened with
  a double-click, a user-menu command, F4 (the editor) and C-n (a shell)
  each get a tab of their own: a terminal (`../tm/term.h`), where the keys
  go to the program while it runs, so vi, top or mc work there. The tab
  comes to the front when the program first writes something, soon after
  it starts, so an X program or a command with nothing to say leaves the
  panels where they are. A tab whose program ends well and leaves nothing
  on its screen (an X program, vi when you quit) closes by itself; once
  the program has ended, a key closes it. Closing a tab whose program is
  still running hangs up on it.

It needs `../demos/x11.h` and `../tm`'s terminal (`vt.c`, `sb.c`, `lz.c`,
`util.c`, `term.h`), which `build.sh` compiles in.

## Differences from GNU mc

- No subshell. Typed commands run one at a time with `/bin/sh -c`, and the
  output stays on the terminal's normal screen (C-o shows it).
- F4 runs an external editor; there is no built-in editor like mcedit.
- The viewer has no "Raw" (F8) mode or nroff formatting, but it has Markdown
  formatting, which mc's viewer doesn't.
- The user menu supports the common conditions and macros, not all of mc's.
- The Copy/Move dialog has no source mask or option checkboxes, and the
  pull-down menus contain only the commands listed above.
- The SD card is FAT, which has no execute permission, so every file there
  looks executable. On FAT, only names without an extension are shown as
  executables (green `*`). Enter runs a file only if it starts with an ELF or
  `#!` header.
