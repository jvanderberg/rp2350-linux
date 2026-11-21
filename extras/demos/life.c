/*
 * life.c - Conway's Game of Life for ANSI 256-colour terminals.
 *
 * Build:  cc -o life life.c   (or ./build.sh life)
 * Keys:   arrows / HJKL move the cursor, space toggles a cell,
 *         p = run / pause, n = one generation, + / - = faster / slower,
 *         r = random fill, c = clear, 1-6 = stamp a pattern at the cursor,
 *         ? = help, q = quit.
 *
 * The grid fills the terminal and wraps at the edges. Each character shows
 * two cells (half blocks), and only characters that changed are redrawn.
 *
 * ./life -b N runs N generations on a 128x128 grid and prints the time.
 */
#include "term.h"
#include <poll.h>
#include <stdio.h>

#define LIVE_FG   114           /* green */
#define CURSOR_BG 45            /* cyan */
#define CURSOR_FG 16            /* black */
#define BAR_BG    238
#define BAR_FG    252
#define HELP_BG   252
#define HELP_FG   16

#define CURSOR_BIT 4
#define STALE      0xff         /* screen cell that must be redrawn */
#define GAP_MAX    4            /* redraw up to this many unchanged chars rather than jump */

static unsigned char *grid, *next_grid, *colsum;
static int gw, gh;              /* grid size in cells */
static int tcols, trows;        /* terminal size in characters */
static unsigned char *screen;   /* what each character shows now */
static unsigned char *rowbuf;   /* what each character of a row should show */

static int cx, cy;              /* cursor, in cells */
static int running = 1;
static unsigned long generation;
static long pop;
static int delay_ms = 100;
static int help_on = 1, help_drawn;
static int bar_stale = 1;
static unsigned int rng_state;

static const char *const help_text[] = {
    "Conway's Game of Life",
    "",
    "arrows / hjkl  move the cursor",
    "space / enter  toggle the cell",
    "p              run / pause",
    "n              one generation",
    "+ / -          faster / slower",
    "r              random fill",
    "c              clear",
    "1 glider  2 spaceship  3 R-pentomino",
    "4 acorn   5 pulsar     6 glider gun",
    "               (stamped at the cursor)",
    "?              show / hide this help",
    "q              quit",
};
#define HELP_LINES ((int)(sizeof help_text / sizeof help_text[0]))
#define HELP_W 42

struct pattern {
    char key;
    const char *rows[16];       /* '#' alive, '.' dead; NULL ends the list */
};

static const struct pattern patterns[] = {
    { '1', { ".#.", "..#", "###" } },                              /* glider */
    { '2', { ".#..#", "#....", "#...#", "####." } },               /* spaceship */
    { '3', { ".##", "##.", ".#." } },                              /* R-pentomino */
    { '4', { ".#.....", "...#...", "##..###" } },                  /* acorn */
    { '5', { "..###...###..", ".............", "#....#.#....#",   /* pulsar */
             "#....#.#....#", "#....#.#....#", "..###...###..",
             ".............", "..###...###..", "#....#.#....#",
             "#....#.#....#", "#....#.#....#", ".............",
             "..###...###.." } },
    { '6', { "........................#...........",               /* Gosper gun */
             "......................#.#...........",
             "............##......##............##",
             "...........#...#....##............##",
             "##........#.....#...##..............",
             "##........#...#.##....#.#...........",
             "..........#.....#.......#...........",
             "...........#...#....................",
             "............##......................" } },
};

/* ---- the simulation ---- */

static int grid_alloc(int w, int h)
{
    size_t n = (size_t)w * (size_t)h;

    grid = calloc(n, 1);
    next_grid = calloc(n, 1);
    colsum = malloc((size_t)w);
    if (grid == NULL || next_grid == NULL || colsum == NULL)
        return -1;
    gw = w;
    gh = h;
    return 0;
}

static unsigned int rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void count_pop(void)
{
    size_t n = (size_t)gw * (size_t)gh;

    pop = 0;
    for (size_t i = 0; i < n; i++)
        pop += grid[i];
}

static void step(void)
{
    long p = 0;

    for (int y = 0; y < gh; y++) {
        const unsigned char *up = grid + (size_t)((y + gh - 1) % gh) * gw;
        const unsigned char *mid = grid + (size_t)y * gw;
        const unsigned char *dn = grid + (size_t)((y + 1) % gh) * gw;
        unsigned char *o = next_grid + (size_t)y * gw;
        int x;

        for (x = 0; x < gw; x++)
            colsum[x] = (unsigned char)(up[x] + mid[x] + dn[x]);
        /* t counts the 3x3 block, the cell itself included */
        for (x = 0; x < gw; x++) {
            int l = x ? x - 1 : gw - 1;
            int r = x + 1 < gw ? x + 1 : 0;
            int t = colsum[l] + colsum[x] + colsum[r];

            o[x] = (unsigned char)(t == 3 || (t == 4 && mid[x]));
            p += o[x];
        }
    }
    {
        unsigned char *t = grid;
        grid = next_grid;
        next_grid = t;
    }
    pop = p;
    generation++;
}

static void randomize(void)
{
    size_t n = (size_t)gw * (size_t)gh;

    for (size_t i = 0; i < n; i++)
        grid[i] = (unsigned char)(rng() % 100 < 30);
    generation = 0;
    count_pop();
}

static const struct pattern *find_pattern(int key)
{
    for (size_t i = 0; i < sizeof patterns / sizeof patterns[0]; i++)
        if (patterns[i].key == key)
            return &patterns[i];
    return NULL;
}

static void stamp(const struct pattern *p, int ox, int oy)
{
    for (int r = 0; r < 16 && p->rows[r] != NULL; r++)
        for (int c = 0; p->rows[r][c] != '\0'; c++)
            grid[(size_t)((oy + r) % gh) * gw + (ox + c) % gw] =
                (unsigned char)(p->rows[r][c] == '#');
    count_pop();
}

/* ---- drawing ---- */

static const char *const half[4] = {
    " ", "\xe2\x96\x80", "\xe2\x96\x84", "\xe2\x96\x88"     /* ' ' upper lower full */
};

static int help_x(void) { return (tcols - HELP_W) / 2; }
static int help_y(void) { return (trows - 1 - (HELP_LINES + 2)) / 2; }

static int under_help(int tx, int ty)
{
    int bx = help_x(), by = help_y();

    return help_on && bx >= 0 && by >= 0 &&
           tx >= bx && tx < bx + HELP_W && ty >= by && ty < by + HELP_LINES + 2;
}

static void draw_help(void)
{
    int bx = help_x(), by = help_y();

    if (bx < 0 || by < 0)
        return;
    term_bg(HELP_BG);
    term_fg(HELP_FG);
    for (int i = 0; i < HELP_LINES + 2; i++) {
        const char *line = (i == 0 || i == HELP_LINES + 1) ? "" : help_text[i - 1];
        int len = (int)strlen(line);

        term_goto(by + i + 1, bx + 1);
        term_str("  ");
        term_put(line, (size_t)len);
        for (int j = len + 2; j < HELP_W; j++)
            term_put(" ", 1);
    }
    term_reset();
}

static void draw_bar(void)
{
    char s[160];
    int n = snprintf(s, sizeof s, " %s  gen %lu  pop %ld  %d ms  |  ?:help  p:run/pause  q:quit",
                     running ? "RUN " : "STOP", generation, pop, delay_ms);

    if (n < 0)
        n = 0;
    if (n > tcols - 1)          /* never write the last column: it can scroll */
        n = tcols - 1;
    term_goto(trows, 1);
    term_bg(BAR_BG);
    term_fg(BAR_FG);
    term_put(s, (size_t)n);
    for (int i = n; i < tcols - 1; i++)
        term_put(" ", 1);
    term_reset();
}

/* Send one character with the colours its code needs; *attr tracks the current ones. */
static void put_char(unsigned char v, int *attr)
{
    int want = (v & CURSOR_BIT) ? 2 : 1;

    if (want != *attr) {
        term_reset();
        if (want == 2) {
            term_bg(CURSOR_BG);
            term_fg(CURSOR_FG);
        } else {
            term_fg(LIVE_FG);
        }
        *attr = want;
    }
    term_str(half[v & 3]);
}

static void render(void)
{
    int attr = 0;               /* 0 unknown, 1 plain, 2 cursor */

    for (int ty = 0; ty < trows - 1; ty++) {
        const unsigned char *a = grid + (size_t)(ty * 2) * gw;
        const unsigned char *b = a + gw;
        unsigned char *s = screen + (size_t)ty * tcols;
        int pos = -1;           /* where the terminal cursor is in this row, -1 unknown */

        for (int tx = 0; tx < tcols; tx++) {
            unsigned char v = (unsigned char)(a[tx] | b[tx] << 1);

            if (tx == cx && ty == cy / 2)
                v |= CURSOR_BIT;
            rowbuf[tx] = v;
        }
        for (int tx = 0; tx < tcols; tx++) {
            if (under_help(tx, ty)) {
                s[tx] = STALE;  /* repaint when the help closes */
                continue;
            }
            if (s[tx] == rowbuf[tx])
                continue;
            if (pos < 0 || tx - pos > GAP_MAX) {
                term_goto(ty + 1, tx + 1);
            } else {
                /* a short gap: cheaper to resend the unchanged chars */
                for (int g = pos; g < tx; g++)
                    put_char(rowbuf[g], &attr);
            }
            put_char(rowbuf[tx], &attr);
            s[tx] = rowbuf[tx];
            pos = tx + 1;
        }
    }
    term_reset();
    if (help_on && !help_drawn) {
        draw_help();
        help_drawn = 1;
    }
    if (bar_stale) {
        draw_bar();
        bar_stale = 0;
    }
    term_flush();
}

/* ---- input ---- */

enum { K_NONE = -1, K_UP = 256, K_DOWN, K_LEFT, K_RIGHT };

static int esc_state;

static int read_key(void)
{
    int c;

    while ((c = term_read_byte()) >= 0) {
        if (esc_state == 1) {
            if (c == '[' || c == 'O') {
                esc_state = 2;
                continue;
            }
            esc_state = 0;
        } else if (esc_state == 2) {
            esc_state = 0;
            switch (c) {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return K_RIGHT;
            case 'D': return K_LEFT;
            default:  continue;
            }
        }
        if (c == 27) {
            esc_state = 1;
            continue;
        }
        return c;
    }
    return K_NONE;
}

static void handle_key(int k, long long now, long long *next_tick)
{
    const struct pattern *pat;

    if (help_on && k != '?') {  /* any other key closes the help, then acts */
        help_on = 0;
        help_drawn = 0;
    }
    pat = k < 256 ? find_pattern(k) : NULL;
    if (pat != NULL) {
        stamp(pat, cx, cy);
        return;
    }
    switch (k) {
    case 'q': case 'Q': term_quit_flag = 1; break;
    case K_UP:    case 'k': case 'K': cy = (cy + gh - 1) % gh; break;
    case K_DOWN:  case 'j': case 'J': cy = (cy + 1) % gh; break;
    case K_LEFT:  case 'h': case 'H': cx = (cx + gw - 1) % gw; break;
    case K_RIGHT: case 'l': case 'L': cx = (cx + 1) % gw; break;
    case ' ': case '\r': case '\n':
        grid[(size_t)cy * gw + cx] ^= 1;
        count_pop();
        break;
    case 'p': case 'P':
        running = !running;
        *next_tick = now;
        break;
    case 'n': case 'N':
        running = 0;
        step();
        break;
    case 'r': case 'R': randomize(); break;
    case 'c': case 'C':
        memset(grid, 0, (size_t)gw * (size_t)gh);
        generation = 0;
        pop = 0;
        running = 0;
        break;
    case '+': case '=': delay_ms = delay_ms > 20 ? delay_ms / 2 : 10; break;
    case '-': case '_': delay_ms = delay_ms < 1600 ? delay_ms * 2 : 1600; break;
    case '?':
        help_on = !help_on;
        help_drawn = 0;
        break;
    }
}

/* ---- main ---- */

static int bench(int gens)
{
    long long t0, t1;

    rng_state = 1;
    if (grid_alloc(128, 128) < 0)
        return 1;
    randomize();
    t0 = term_now_us();
    for (int i = 0; i < gens; i++)
        step();
    t1 = term_now_us();
    printf("128x128, %d generations: pop %ld, %lld ms (%.1f gen/s)\n", gens, pop,
           (t1 - t0) / 1000, t1 > t0 ? gens * 1e6 / (double)(t1 - t0) : 0.0);
    return 0;
}

int main(int argc, char **argv)
{
    long long next_tick;

    if (argc == 3 && strcmp(argv[1], "-b") == 0)
        return bench(atoi(argv[2]));
    if (argc != 1) {
        fprintf(stderr, "usage: life [-b GENERATIONS]\n");
        return 2;
    }

    term_size(&tcols, &trows);
    if (tcols > 400)
        tcols = 400;
    if (trows > 200)
        trows = 200;
    if (tcols < 20 || trows < 5) {
        fprintf(stderr, "life needs at least a 20x5 terminal (this one is %dx%d)\n",
                tcols, trows);
        return 1;
    }
    screen = malloc((size_t)tcols * (size_t)(trows - 1));
    rowbuf = malloc((size_t)tcols);
    if (screen == NULL || rowbuf == NULL || grid_alloc(tcols, (trows - 1) * 2) < 0) {
        fprintf(stderr, "life: out of memory\n");
        return 1;
    }
    memset(screen, STALE, (size_t)tcols * (size_t)(trows - 1));

    rng_state = (unsigned int)term_now_us() | 1u;
    cx = gw / 2;
    cy = gh / 2;
    stamp(find_pattern('3'), cx - 1, cy - 1);

    term_begin();
    next_tick = term_now_us();
    while (!term_quit_flag) {
        struct pollfd pfd;
        long long now = term_now_us();
        unsigned long gen0 = generation;
        long pop0 = pop;
        int run0 = running, delay0 = delay_ms;
        int timeout = 1000;
        int k;

        render();

        if (running) {
            long long ms = (next_tick - now + 999) / 1000;
            timeout = ms < 0 ? 0 : (int)ms;
        }
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll(&pfd, 1, timeout);

        now = term_now_us();
        while ((k = read_key()) != K_NONE)
            handle_key(k, now, &next_tick);

        if (running && now >= next_tick) {
            step();
            next_tick += delay_ms * 1000LL;
            if (next_tick < now)        /* fell behind: don't try to catch up */
                next_tick = now;
        }
        if (generation != gen0 || pop != pop0 || running != run0 || delay_ms != delay0)
            bar_stale = 1;
    }

    term_end();
    printf("Life: %lu generations, %ld cells alive\n", generation, pop);
    return 0;
}
