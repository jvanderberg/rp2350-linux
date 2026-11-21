/*
 * rain.c - a falling-glyph "digital rain" screensaver for ANSI terminals.
 *
 * Build:  cc -o rain rain.c
 * Run:    ./rain        (half-width katakana + digits)
 *         ./rain -a     (plain ASCII, for fonts without katakana)
 * Keys:   c = colour theme, + / - = speed, space = pause,
 *         any other key quits.
 *
 * Only cells that changed since the last frame are sent to the terminal.
 */
#include "term.h"
#include <poll.h>
#include <stdio.h>

#define FRAME_US   50000        /* 20 frames per second */
#define DROPS      2            /* drops that can share one column */
#define INVALID    0xFFFFFFFFu
#define HINT_US    4000000

/* A drop's head position and speed are in 1/256ths of a row. */
typedef struct {
    int y;
    int speed;
    int active;
    int wait;                   /* frames until it may respawn */
} Drop;

static int cols, rows;
static unsigned char *level;    /* brightness 0..255 per cell */
static unsigned char *glyph;    /* glyph index per cell */
static unsigned char *head;     /* 1 where a drop's head is this frame */
static unsigned char *decay;    /* fade rate per column */
static unsigned int *shown;     /* what the terminal currently shows */
static Drop *drops;

static char glyphs[96][4];
static int nglyphs;

/* 8 brightness steps from dim to bright, then the head colour. */
static const unsigned char themes[][9] = {
    { 22, 22, 28, 28, 34, 40, 46, 120, 231 },   /* green  */
    { 17, 18, 19, 25, 31, 38, 45, 159, 231 },   /* ice    */
    { 52, 52, 88, 124, 160, 196, 203, 217, 231 }, /* red  */
    { 53, 54, 55, 91, 127, 165, 171, 219, 231 }, /* violet */
    { 58, 94, 130, 136, 172, 178, 214, 229, 231 }, /* amber */
};
#define NTHEMES ((int)(sizeof themes / sizeof themes[0]))

static int theme;
static int speed_pct = 100;
static int paused;
static volatile sig_atomic_t resized;

static unsigned int rng_state = 1;

static unsigned int rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Row of a fixed-point position, rounding toward minus infinity. */
static int row_of(int y)
{
    return y >= 0 ? y >> 8 : -((-y + 255) >> 8);
}

static void build_glyphs(int ascii)
{
    const char *extra = ascii
        ? "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ$%&*+=<>:;?"
        : "0123456789:.=*+-<>|";

    nglyphs = 0;
    if (!ascii) {
        /* Half-width katakana, U+FF66..U+FF9D, as UTF-8. */
        for (unsigned int cp = 0xFF66; cp <= 0xFF9D; cp++) {
            glyphs[nglyphs][0] = (char)(0xE0 | (cp >> 12));
            glyphs[nglyphs][1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            glyphs[nglyphs][2] = (char)(0x80 | (cp & 0x3F));
            glyphs[nglyphs][3] = 0;
            nglyphs++;
        }
    }
    for (const char *p = extra; *p; p++) {
        glyphs[nglyphs][0] = *p;
        glyphs[nglyphs][1] = 0;
        nglyphs++;
    }
}

static void spawn(Drop *d, int c)
{
    d->active = 1;
    d->y = -(int)(rng() % (4 * 256));
    d->speed = 48 + (int)(rng() % 208);         /* 0.2 .. 1 row per frame */
    decay[c] = (unsigned char)(5 + rng() % 16); /* trail length */
}

static void *alloc_or_die(size_t n)
{
    void *p = calloc(n, 1);

    if (p == NULL) {
        term_end();
        fprintf(stderr, "rain: out of memory\n");
        exit(1);
    }
    return p;
}

static void setup_grid(void)
{
    size_t n = (size_t)cols * (size_t)rows;

    free(level);
    free(glyph);
    free(head);
    free(decay);
    free(shown);
    free(drops);
    level = alloc_or_die(n);
    glyph = alloc_or_die(n);
    head = alloc_or_die(n);
    decay = alloc_or_die((size_t)cols);
    shown = alloc_or_die(n * sizeof *shown);    /* 0 = blank, as cleared */
    drops = alloc_or_die((size_t)cols * DROPS * sizeof *drops);

    /* Start mid-storm: first drops are already spread above the screen. */
    for (int c = 0; c < cols; c++) {
        Drop *d = &drops[c * DROPS];

        spawn(&d[0], c);
        d[0].y = -(int)(rng() % (unsigned int)(rows * 256));
        d[1].active = 0;
        d[1].wait = (int)(rng() % (unsigned int)(rows * 2));
    }
}

static void update(void)
{
    /* Fade every lit cell. */
    for (int r = 0; r < rows; r++) {
        unsigned char *row = level + r * cols;

        for (int c = 0; c < cols; c++)
            if (row[c])
                row[c] = row[c] > decay[c] ? (unsigned char)(row[c] - decay[c]) : 0;
    }

    /* Move drops; each row a head enters gets a fresh bright glyph. */
    for (int c = 0; c < cols; c++) {
        for (int k = 0; k < DROPS; k++) {
            Drop *d = &drops[c * DROPS + k];
            Drop *other = &drops[c * DROPS + (1 - k)];
            int old, now;

            if (!d->active) {
                if (d->wait > 0)
                    d->wait--;
                else if (!other->active || row_of(other->y) > rows / 2)
                    spawn(d, c);
                continue;
            }

            old = row_of(d->y);
            d->y += d->speed * speed_pct / 100;
            now = row_of(d->y);
            for (int r = old + 1; r <= now; r++) {
                if (r >= 0 && r < rows) {
                    level[r * cols + c] = 255;
                    glyph[r * cols + c] = (unsigned char)(rng() % (unsigned int)nglyphs);
                }
            }
            if (now >= rows) {
                d->active = 0;
                d->wait = (int)(rng() % (unsigned int)(rows * 2));
            }
        }
    }

    /* Shimmer: a few lit glyphs change on every frame. */
    for (int i = 0; i < cols / 3; i++) {
        int idx = (int)(rng() % (unsigned int)(rows * cols));

        if (level[idx])
            glyph[idx] = (unsigned char)(rng() % (unsigned int)nglyphs);
    }
}

static const char hint[] =
    " c colour   + - speed   space pause   any other key quits ";
#define HINT_LEN ((int)sizeof hint - 1)

static void render(int show_hint)
{
    const unsigned char *pal = themes[theme];
    int cur_r = -1, cur_c = -1, cur_fg = -1;

    memset(head, 0, (size_t)cols * (size_t)rows);
    for (int c = 0; c < cols; c++) {
        for (int k = 0; k < DROPS; k++) {
            Drop *d = &drops[c * DROPS + k];
            int r = row_of(d->y);

            if (d->active && r >= 0 && r < rows)
                head[r * cols + c] = 1;
        }
    }

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int i = r * cols + c;
            unsigned int code = 0;
            int color = 0;

            if (show_hint && r == rows - 1 && c < HINT_LEN)
                continue;

            if (level[i]) {
                color = head[i] ? pal[8] : pal[level[i] >> 5];
                code = (unsigned int)color << 8 | (unsigned int)(glyph[i] + 1);
            }
            if (shown[i] == code)
                continue;

            if (r != cur_r || c != cur_c)
                term_goto(r + 1, c + 1);
            if (code == 0) {
                term_put(" ", 1);
            } else {
                if (color != cur_fg) {
                    term_fg(color);
                    cur_fg = color;
                }
                term_str(glyphs[glyph[i]]);
            }
            shown[i] = code;
            cur_r = r;
            cur_c = c + 1;
        }
    }

    if (show_hint && cols >= HINT_LEN) {
        term_goto(rows, 1);
        term_str("\x1b[38;5;250;48;5;236m");
        term_str(hint);
        term_reset();
    }
    term_flush();
}

static void on_winch(int sig)
{
    (void)sig;
    resized = 1;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    long long next, hint_until;
    int hint_on = 1;

    build_glyphs(argc > 1 && strcmp(argv[1], "-a") == 0);
    rng_state = (unsigned int)term_now_us() | 1u;

    term_size(&cols, &rows);
    setup_grid();
    term_begin();

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);

    next = term_now_us();
    hint_until = next + HINT_US;

    while (!term_quit_flag) {
        struct pollfd pfd;
        long long now;
        int k, wait_ms;

        if (resized) {
            resized = 0;
            term_size(&cols, &rows);
            setup_grid();
            term_str("\x1b[0m\x1b[2J");
        }

        while ((k = term_read_byte()) >= 0) {
            if (k == 'c' || k == 'C')
                theme = (theme + 1) % NTHEMES;
            else if (k == '+' || k == '=')
                speed_pct = speed_pct < 400 ? speed_pct + 25 : 400;
            else if (k == '-' || k == '_')
                speed_pct = speed_pct > 25 ? speed_pct - 25 : 25;
            else if (k == ' ')
                paused = !paused;
            else
                term_quit_flag = 1;
        }
        if (term_quit_flag)
            break;

        now = term_now_us();
        if (hint_on && now >= hint_until) {
            /* Hint gone: force the cells under it to be redrawn. */
            hint_on = 0;
            for (int c = 0; c < HINT_LEN && c < cols; c++)
                shown[(rows - 1) * cols + c] = INVALID;
        }
        if (now >= next) {
            if (!paused)
                update();
            render(hint_on && cols >= HINT_LEN);
            next += FRAME_US;
            if (next < now)
                next = now + FRAME_US;
        }

        wait_ms = (int)((next - term_now_us() + 999) / 1000);
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll(&pfd, 1, wait_ms < 0 ? 0 : wait_ms);
    }

    term_end();
    return 0;
}
