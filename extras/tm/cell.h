/*
 * cell.h - one character cell of a terminal screen.
 *
 * A cell is a Unicode code point plus a packed attribute word:
 *
 *   bits  0..8   foreground colour: 0..255, or COL_DEFAULT
 *   bits  9..17  background colour: 0..255, or COL_DEFAULT
 *   bits 18..25  AT_* flags (bold, underline, ...)
 *   bit  26      CELL_CONT: right half of a double-width character
 */
#ifndef CELL_H
#define CELL_H

#include <stdint.h>

#define COL_DEFAULT 256

#define AT_BOLD     0x01
#define AT_DIM      0x02
#define AT_ITALIC   0x04
#define AT_UNDER    0x08
#define AT_BLINK    0x10
#define AT_REVERSE  0x20
#define AT_HIDDEN   0x40
#define AT_STRIKE   0x80

#define CELL_CONT   (1u << 26)

#define MKATTR(fg, bg, fl) \
    ((uint32_t)(fg) | ((uint32_t)(bg) << 9) | ((uint32_t)(fl) << 18))
#define CELL_FG(a)  ((a) & 0x1FF)
#define CELL_BG(a)  (((a) >> 9) & 0x1FF)
#define CELL_FL(a)  (((a) >> 18) & 0xFF)
#define ATTR_DEFAULT MKATTR(COL_DEFAULT, COL_DEFAULT, 0)

struct cell {
    uint32_t ch;
    uint32_t attr;
};

#endif /* CELL_H */
