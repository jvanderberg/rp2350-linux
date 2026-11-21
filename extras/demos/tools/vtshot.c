/*
 * vtshot.c - replay ANSI output into a text grid so a frame can be inspected
 * without a real terminal. Handles only the escapes the demos emit.
 * Cells with a background colour and a blank glyph print a letter for the
 * colour (see legend()). Usage: ./prog | vtshot [cols rows]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXC 200
#define MAXR 80

static char glyph[MAXR][MAXC][5];
static int bgc[MAXR][MAXC];
static int cols = 80, rows = 24;

static char legend(int bg)
{
    switch (bg) {
    case -1:  return ' ';
    case 239: return '#';
    case 233: case 234: return '.';
    case 255: return '=';
    case 203: return 'I';
    case 45:  return 'O';
    case 220: return 'T';
    case 170: return 'S';
    case 114: return 'Z';
    case 208: return 'J';
    case 69:  return 'L';
    default:  return '?';
    }
}

static void clear_all(void)
{
    for (int r = 0; r < MAXR; r++)
        for (int c = 0; c < MAXC; c++) {
            strcpy(glyph[r][c], " ");
            bgc[r][c] = -1;
        }
}

static void dump(void)
{
    for (int r = 0; r < rows; r++) {
        printf("%2d|", r + 1);
        for (int c = 0; c < cols; c++) {
            if (strcmp(glyph[r][c], " ") == 0)
                putchar(legend(bgc[r][c]));
            else
                fputs(glyph[r][c], stdout);
        }
        printf("|\n");
    }
}

/*
 * Optional 3rd/4th args: a marker row and column. Every cursor move to that
 * position (the start of a frame) dumps the screen as it was before it.
 */
int main(int argc, char **argv)
{
    int ch, row = 0, col = 0, bg = -1, mrow = 0, mcol = 0, frame = 0;

    if (argc > 2) {
        cols = atoi(argv[1]);
        rows = atoi(argv[2]);
    }
    if (argc > 4) {
        mrow = atoi(argv[3]);
        mcol = atoi(argv[4]);
    }
    clear_all();

    while ((ch = getchar()) != EOF) {
        if (ch == 27) {
            int params[16], np = 0, cur = 0, have = 0, priv = 0, fin;

            if (getchar() != '[')
                continue;
            for (;;) {
                fin = getchar();
                if (fin == EOF)
                    break;
                if (fin == '?') { priv = 1; continue; }
                if (fin >= '0' && fin <= '9') { cur = cur * 10 + fin - '0'; have = 1; continue; }
                if (fin == ';') { if (np < 16) params[np++] = cur; cur = 0; have = 0; continue; }
                break;
            }
            if (have && np < 16)
                params[np++] = cur;
            if (priv)
                continue;
            if (fin == 'H') {
                row = (np > 0 ? params[0] : 1) - 1;
                col = (np > 1 ? params[1] : 1) - 1;
                if (mrow && row == mrow - 1 && col == mcol - 1) {
                    if (frame > 0) {
                        printf("--- frame %d ---\n", frame);
                        dump();
                    }
                    frame++;
                }
            } else if (fin == 'J') {
                clear_all();
            } else if (fin == 'K') {
                for (int c = col; c < MAXC; c++) {
                    strcpy(glyph[row][c], " ");
                    bgc[row][c] = bg;
                }
            } else if (fin == 'm') {
                if (np == 0)
                    bg = -1;
                for (int i = 0; i < np; i++) {
                    if (params[i] == 0 || params[i] == 49)
                        bg = -1;
                    else if (params[i] == 48 && i + 2 < np) {
                        bg = params[i + 2];
                        i += 2;
                    } else if (params[i] == 38 && i + 2 < np)
                        i += 2;
                }
            }
            continue;
        }
        if (ch == '\r') { col = 0; continue; }
        if (ch == '\n') { row++; continue; }
        if ((ch & 0xC0) == 0x80)
            continue;           /* UTF-8 continuation: handled below */

        if (row >= 0 && row < MAXR && col >= 0 && col < MAXC) {
            char *g = glyph[row][col];
            int n = 0;

            g[n++] = (char)ch;
            if (ch >= 0xC0) {
                int extra = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1;
                while (extra-- > 0 && n < 4) {
                    int nx = getchar();
                    if (nx == EOF)
                        break;
                    g[n++] = (char)nx;
                }
            }
            g[n] = 0;
            bgc[row][col] = bg;
        }
        col++;
    }

    if (mrow)
        printf("--- final ---\n");
    dump();
    return 0;
}
