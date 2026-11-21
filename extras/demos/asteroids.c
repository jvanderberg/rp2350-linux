/*
 * asteroids.c - a text-mode rocks-in-space shooter for ANSI terminals.
 *
 * Build:  cc -o asteroids asteroids.c      (or ./build.sh asteroids)
 * Run:    ./asteroids       (Unicode ship glyphs)
 *         ./asteroids -a    (plain ASCII)
 *         ./asteroids -n N  (headless test: run N frames flat out, reading
 *                            one input byte per frame, then exit)
 * Keys:   left/right or a/d = turn, up or w = thrust, space = fire,
 *         p = pause, r = restart, q = quit.
 *
 * Terminals do not report key releases, so a tap turns the ship one step
 * (45 degrees) and gives a short burn; hold the key to keep going.
 *
 * All maths is 16.16 fixed point (no FPU needed). The world is measured in
 * rows on both axes and a row is two columns wide, so motion looks the same
 * speed in every direction. Only cells that changed are redrawn.
 */
#include "term.h"
#include <poll.h>
#include <stdio.h>

#define FRAME_US   50000        /* 20 frames per second */
#define FP         65536
#define MAX_ROCKS  48
#define MAX_SHOTS  4
#define MAX_PARTS  64
#define SHOT_LIFE  22
#define START_LIVES 3
#define EXTRA_LIFE 5000
#define HINT_FRAMES 160
#define SHIP_GLYPH 128          /* glyph ids 128..135 are the ship headings */

enum { PLAY, DEAD, OVER };

typedef struct { int x, y, vx, vy, size, variant; } Rock;
typedef struct { int x, y, vx, vy, life; } Shot;
typedef struct { int x, y, vx, vy, life, fg; char ch; } Part;

/* Heading 0 is up, then clockwise in 45 degree steps. */
static const int dirx[8] = { 0, 46341, 65536, 46341, 0, -46341, -65536, -46341 };
static const int diry[8] = { -65536, -46341, 0, 46341, 65536, 46341, 0, -46341 };
/* The screen cell "behind" each heading, for the exhaust flame. */
static const int cellx[8] = { 0, 2, 1, 2, 0, -2, -1, -2 };
static const int celly[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

static const char *ship_utf8[8] = {
    "\xe2\x96\xb2", "\xe2\x97\xa5", "\xe2\x96\xb6", "\xe2\x97\xa2",
    "\xe2\x96\xbc", "\xe2\x97\xa3", "\xe2\x97\x80", "\xe2\x97\xa4",
};
static const char ship_ascii[8] = { '^', '/', '>', '\\', 'v', '/', '<', '\\' };

/* Rock pictures; spaces are transparent. */
static const char *big_a[5] = {
    "  .-~~-.  ",
    " /      \\ ",
    "|    .   |",
    " \\  '   / ",
    "  `-..-'  ",
};
static const char *big_b[5] = {
    "  _.--._  ",
    " /  .   \\ ",
    "(      , )",
    " \\ '    / ",
    "  `-==-'  ",
};
static const char *mid_a[3] = { " .--. ", "(  . )", " `--' " };
static const char *mid_b[3] = { " ,-~. ", "{ '  }", " `-~' " };
static const char *small_a[1] = { "()" };
static const char *small_b[1] = { "<>" };

/* Per size (1 small, 2 medium, 3 big): radius, speed, points. */
static const int rock_r[4]   = { 0, 52000, 92000, 150000 };
static const int rock_spd[4] = { 0, 10500, 7200, 4600 };
static const int rock_pts[4] = { 0, 100, 50, 20 };

static int cols, rows;          /* terminal size */
static int W, H;                /* world size, 16.16 rows */
static unsigned char *cell_ch, *cell_fg;
static unsigned short *shown;   /* what the terminal currently shows */
static int ascii_only;
static volatile sig_atomic_t resized;

static Rock rocks[MAX_ROCKS];
static Shot shots[MAX_SHOTS];
static Part parts[MAX_PARTS];

static struct { int x, y, vx, vy, dir, burn, safe; } ship;
static int state, timer, wave_wait;
static int score, best, lives, level, next_extra;
static int paused, hint, frame_no;
static char best_path[256];

static unsigned int rng_state = 1;

static int rnd(int n)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (int)(rng_state % (unsigned int)n);
}

static int wrap(int v, int size)
{
    v %= size;
    return v < 0 ? v + size : v;
}

/* Shortest signed distance from b to a on a wrapping axis. */
static int wdelta(int a, int b, int size)
{
    int d = a - b;

    if (d > size / 2)
        d -= size;
    else if (d < -size / 2)
        d += size;
    return d;
}

static int within(int x1, int y1, int x2, int y2, int r)
{
    int dx = wdelta(x1, x2, W) >> 8;
    int dy = wdelta(y1, y2, H) >> 8;

    r >>= 8;
    return dx * dx + dy * dy < r * r;
}

/* ---------- high score ---------- */

static void load_best(void)
{
    const char *home = getenv("HOME");
    FILE *f;

    if (home == NULL)
        return;
    snprintf(best_path, sizeof best_path, "%s/.asteroids_best", home);
    f = fopen(best_path, "r");
    if (f != NULL) {
        if (fscanf(f, "%d", &best) != 1)
            best = 0;
        fclose(f);
    }
}

static void save_best(void)
{
    FILE *f;

    if (best_path[0] == '\0')
        return;
    f = fopen(best_path, "w");
    if (f != NULL) {
        fprintf(f, "%d\n", best);
        fclose(f);
    }
}

/* ---------- drawing ---------- */

/* Plot into the play field (below the status row), wrapping at the edges. */
static void plot(int col, int row, int ch, int fg)
{
    int i = (wrap(row, rows - 1) + 1) * cols + wrap(col, cols);

    cell_ch[i] = (unsigned char)ch;
    cell_fg[i] = (unsigned char)fg;
}

static void text_at(int row, int col, const char *s, int fg)
{
    for (; *s != '\0'; s++, col++) {
        if (row >= 0 && row < rows && col >= 0 && col < cols) {
            cell_ch[row * cols + col] = (unsigned char)*s;
            cell_fg[row * cols + col] = (unsigned char)fg;
        }
    }
}

static void text_mid(int row, const char *s, int fg)
{
    text_at(row, (cols - (int)strlen(s)) / 2, s, fg);
}

static void draw_stars(void)
{
    for (int r = 1; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            unsigned int h = (unsigned int)(r * 7919 + c * 104729);

            h ^= h >> 7;
            h *= 2654435761u;
            if ((h >> 16) % 61 == 0) {
                cell_ch[r * cols + c] = '.';
                cell_fg[r * cols + c] = 237;
            }
        }
}

static void draw_rock(const Rock *a)
{
    const char **pic;
    int w, h, fg;
    int col = (a->x * 2) >> 16, row = a->y >> 16;

    if (a->size == 3) {
        pic = a->variant ? big_b : big_a; w = 10; h = 5; fg = 250;
    } else if (a->size == 2) {
        pic = a->variant ? mid_b : mid_a; w = 6; h = 3; fg = 180;
    } else {
        pic = a->variant ? small_b : small_a; w = 2; h = 1; fg = 215;
    }
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            if (pic[r][c] != ' ')
                plot(col - w / 2 + c, row - h / 2 + r, pic[r][c], fg);
}

static void draw_ship(void)
{
    int col = (ship.x * 2) >> 16, row = ship.y >> 16;

    if (ship.safe > 0 && (frame_no & 2))
        return;                 /* blink while invulnerable */
    if (ship.burn > 0 && (frame_no & 1))
        plot(col - cellx[ship.dir], row - celly[ship.dir], '*', 208);
    else if (ascii_only)
        plot(col - cellx[ship.dir], row - celly[ship.dir], 'o', 244);
    plot(col, row, ascii_only ? ship_ascii[ship.dir] : SHIP_GLYPH + ship.dir,
         51);
}

static void draw_hud(void)
{
    char line[96];
    int n;

    n = snprintf(line, sizeof line, " SCORE %06d   WAVE %d   BEST %06d   SHIPS ",
                 score, level, best);
    text_at(0, 0, line, 252);
    for (int i = 0; i < lives && n + i < cols; i++) {
        cell_ch[n + i] = (unsigned char)(ascii_only ? '^' : SHIP_GLYPH);
        cell_fg[n + i] = 51;
    }
}

static void draw_frame(void)
{
    memset(cell_ch, ' ', (size_t)(cols * rows));
    memset(cell_fg, 7, (size_t)(cols * rows));
    draw_stars();

    for (int i = 0; i < MAX_PARTS; i++)
        if (parts[i].life > 0)
            plot((parts[i].x * 2) >> 16, parts[i].y >> 16, parts[i].ch,
                 parts[i].life > 4 ? parts[i].fg : 240);
    for (int i = 0; i < MAX_ROCKS; i++)
        if (rocks[i].size)
            draw_rock(&rocks[i]);
    for (int i = 0; i < MAX_SHOTS; i++)
        if (shots[i].life > 0)
            plot((shots[i].x * 2) >> 16, shots[i].y >> 16, '*', 226);
    if (state == PLAY)
        draw_ship();

    draw_hud();
    if (state == OVER) {
        text_mid(rows / 2 - 1, "  G A M E   O V E R  ", 196);
        text_mid(rows / 2 + 1, "  r = play again   q = quit  ", 252);
    } else if (paused) {
        text_mid(rows / 2, "  P A U S E D  ", 226);
    } else if (hint > 0) {
        text_mid(rows - 1,
                 " arrows/WASD turn + thrust   SPACE fire   P pause   Q quit ",
                 244);
    }
}

static void render(void)
{
    int n = cols * rows - 1;    /* never touch the bottom-right cell */
    int cursor = -1, fg = -1;

    for (int i = 0; i < n; i++) {
        unsigned short v = (unsigned short)(cell_ch[i] | cell_fg[i] << 8);

        if (v == shown[i])
            continue;
        if (i != cursor)
            term_goto(i / cols + 1, i % cols + 1);
        if (cell_fg[i] != fg) {
            fg = cell_fg[i];
            term_fg(fg);
        }
        if (cell_ch[i] >= SHIP_GLYPH) {
            term_str(ship_utf8[(cell_ch[i] - SHIP_GLYPH) & 7]);
        } else {
            char c = (char)cell_ch[i];
            term_put(&c, 1);
        }
        shown[i] = v;
        cursor = (i + 1) % cols ? i + 1 : -1;
    }
    term_flush();
}

static int set_size(void)
{
    int c, r;
    unsigned char *a, *b;
    unsigned short *s;

    term_size(&c, &r);
    if (c < 20)
        c = 20;
    if (r < 8)
        r = 8;
    a = realloc(cell_ch, (size_t)(c * r));
    if (a != NULL)
        cell_ch = a;
    b = realloc(cell_fg, (size_t)(c * r));
    if (b != NULL)
        cell_fg = b;
    s = realloc(shown, (size_t)(c * r) * sizeof *shown);
    if (s != NULL)
        shown = s;
    if (a == NULL || b == NULL || s == NULL)
        return -1;
    cols = c;
    rows = r;
    W = cols * (FP / 2);
    H = (rows - 1) * FP;
    memset(shown, 0xFF, (size_t)(c * r) * sizeof *shown);
    term_str("\x1b[0m\x1b[2J");
    return 0;
}

/* ---------- game ---------- */

static void burst(int x, int y, int n, int fg)
{
    static const char bits[] = ".,'`*+";

    for (int i = 0; i < MAX_PARTS && n > 0; i++) {
        if (parts[i].life > 0)
            continue;
        parts[i].x = x;
        parts[i].y = y;
        parts[i].vx = rnd(40001) - 20000;
        parts[i].vy = rnd(40001) - 20000;
        parts[i].life = 6 + rnd(12);
        parts[i].ch = bits[rnd((int)sizeof bits - 1)];
        parts[i].fg = fg;
        n--;
    }
}

static void add_rock(int x, int y, int size)
{
    int sp = rock_spd[size] + (level > 10 ? 10 : level) * 400;

    for (int i = 0; i < MAX_ROCKS; i++) {
        Rock *a = &rocks[i];

        if (a->size)
            continue;
        a->x = x;
        a->y = y;
        a->size = size;
        a->variant = rnd(2);
        do {
            a->vx = rnd(2 * sp + 1) - sp;
            a->vy = rnd(2 * sp + 1) - sp;
        } while (abs(a->vx) + abs(a->vy) < sp);
        return;
    }
}

static void reset_ship(void)
{
    ship.x = W / 2;
    ship.y = H / 2;
    ship.vx = ship.vy = 0;
    ship.dir = 0;
    ship.burn = 0;
    ship.safe = 50;
}

static void start_wave(void)
{
    int n = 3 + level;
    int keep = (W < H ? W : H) / 3;

    if (n > 9)
        n = 9;
    while (n-- > 0) {
        int x, y, tries = 0;

        do {
            x = rnd(W);
            y = rnd(H);
        } while (within(x, y, ship.x, ship.y, keep) && ++tries < 50);
        add_rock(x, y, 3);
    }
}

static void new_game(void)
{
    memset(rocks, 0, sizeof rocks);
    memset(shots, 0, sizeof shots);
    memset(parts, 0, sizeof parts);
    score = 0;
    lives = START_LIVES;
    level = 1;
    next_extra = EXTRA_LIFE;
    state = PLAY;
    wave_wait = 0;
    paused = 0;
    hint = HINT_FRAMES;
    reset_ship();
    start_wave();
}

static void add_score(int pts)
{
    score += pts;
    if (score >= next_extra) {
        next_extra += EXTRA_LIFE;
        if (lives < 6)
            lives++;
    }
    if (score > best)
        best = score;
}

static void hit_rock(Rock *a)
{
    int size = a->size, x = a->x, y = a->y;

    a->size = 0;
    add_score(rock_pts[size]);
    burst(x, y, 3 + size * 2, 250);
    if (size > 1) {
        add_rock(x, y, size - 1);
        add_rock(x, y, size - 1);
    }
}

static void fire(void)
{
    if (state != PLAY)
        return;
    for (int i = 0; i < MAX_SHOTS; i++) {
        Shot *s = &shots[i];

        if (s->life > 0)
            continue;
        s->x = wrap(ship.x + dirx[ship.dir] / 2, W);
        s->y = wrap(ship.y + diry[ship.dir] / 2, H);
        s->vx = ship.vx + dirx[ship.dir] / 10 * 9;
        s->vy = ship.vy + diry[ship.dir] / 10 * 9;
        s->life = SHOT_LIFE;
        return;
    }
}

static void kill_ship(void)
{
    burst(ship.x, ship.y, 16, 51);
    burst(ship.x, ship.y, 8, 208);
    lives--;
    if (lives <= 0) {
        state = OVER;
        save_best();
    } else {
        state = DEAD;
        timer = 30;
    }
}

static void step_ship(void)
{
    int limit = 45000 >> 8;     /* top speed, rows per frame */

    if (ship.burn > 0) {
        ship.burn--;
        ship.vx += dirx[ship.dir] >> 5;
        ship.vy += diry[ship.dir] >> 5;
    }
    ship.vx -= ship.vx / 128;   /* a little drag */
    ship.vy -= ship.vy / 128;
    while ((ship.vx >> 8) * (ship.vx >> 8) + (ship.vy >> 8) * (ship.vy >> 8) >
           limit * limit) {
        ship.vx -= ship.vx / 16;
        ship.vy -= ship.vy / 16;
    }
    ship.x = wrap(ship.x + ship.vx, W);
    ship.y = wrap(ship.y + ship.vy, H);
    if (ship.safe > 0)
        ship.safe--;
}

static void step(void)
{
    int left = 0;

    frame_no++;
    if (hint > 0)
        hint--;

    for (int i = 0; i < MAX_PARTS; i++)
        if (parts[i].life > 0) {
            parts[i].life--;
            parts[i].x = wrap(parts[i].x + parts[i].vx, W);
            parts[i].y = wrap(parts[i].y + parts[i].vy, H);
        }

    for (int i = 0; i < MAX_ROCKS; i++)
        if (rocks[i].size) {
            rocks[i].x = wrap(rocks[i].x + rocks[i].vx, W);
            rocks[i].y = wrap(rocks[i].y + rocks[i].vy, H);
        }

    for (int i = 0; i < MAX_SHOTS; i++) {
        Shot *s = &shots[i];

        if (s->life <= 0)
            continue;
        s->life--;
        s->x = wrap(s->x + s->vx, W);
        s->y = wrap(s->y + s->vy, H);
        for (int j = 0; j < MAX_ROCKS; j++)
            if (rocks[j].size &&
                within(s->x, s->y, rocks[j].x, rocks[j].y,
                       rock_r[rocks[j].size])) {
                s->life = 0;
                hit_rock(&rocks[j]);
                break;
            }
    }

    if (state == PLAY) {
        step_ship();
        if (ship.safe == 0)
            for (int j = 0; j < MAX_ROCKS; j++)
                if (rocks[j].size &&
                    within(ship.x, ship.y, rocks[j].x, rocks[j].y,
                           rock_r[rocks[j].size] + 20000)) {
                    hit_rock(&rocks[j]);
                    kill_ship();
                    break;
                }
    } else if (state == DEAD) {
        int clear = 1;

        if (timer > 0)
            timer--;
        for (int j = 0; j < MAX_ROCKS; j++)
            if (rocks[j].size &&
                within(W / 2, H / 2, rocks[j].x, rocks[j].y, 5 * FP))
                clear = 0;
        if (timer == 0 && clear) {
            reset_ship();
            state = PLAY;
        }
    }

    for (int j = 0; j < MAX_ROCKS; j++)
        if (rocks[j].size)
            left++;
    if (left == 0 && state != OVER) {
        if (wave_wait == 0) {
            wave_wait = 30;
        } else if (--wave_wait == 0) {
            level++;
            start_wave();
        }
    }
}

enum { K_NONE, K_LEFT, K_RIGHT, K_UP };

/* Returns 0 to quit. */
static int handle_key(int c)
{
    static int esc;             /* 1 after ESC, 2 after ESC [ or ESC O */
    int k = K_NONE;

    if (esc == 1) {
        esc = (c == '[' || c == 'O') ? 2 : 0;
        return 1;
    }
    if (esc == 2) {
        esc = 0;
        if (c == 'A')
            k = K_UP;
        else if (c == 'C')
            k = K_RIGHT;
        else if (c == 'D')
            k = K_LEFT;
    } else {
        switch (c) {
        case 27:            esc = 1; return 1;
        case 'q': case 'Q': return 0;
        case 'a': case 'A': k = K_LEFT; break;
        case 'd': case 'D': k = K_RIGHT; break;
        case 'w': case 'W': k = K_UP; break;
        case 'r': case 'R':
            if (state == OVER)
                new_game();
            return 1;
        case 'p': case 'P':
            if (state != OVER)
                paused = !paused;
            return 1;
        case ' ':
            if (!paused)
                fire();
            return 1;
        default:
            return 1;
        }
    }
    if (paused || state != PLAY)
        return 1;
    if (k == K_LEFT)
        ship.dir = (ship.dir + 7) & 7;
    else if (k == K_RIGHT)
        ship.dir = (ship.dir + 1) & 7;
    else if (k == K_UP)
        ship.burn = 5;
    return 1;
}

static void on_winch(int sig)
{
    (void)sig;
    resized = 1;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    long long next;
    int max_frames = 0, running = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0)
            ascii_only = 1;
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            max_frames = atoi(argv[++i]);
    }

    rng_state = (unsigned int)term_now_us() | 1u;
    if (max_frames > 0)
        rng_state = 12345;      /* repeatable test runs */
    load_best();

    term_begin();
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    if (set_size() != 0) {
        term_end();
        fputs("asteroids: out of memory\n", stderr);
        return 1;
    }
    new_game();

    next = term_now_us();
    while (running && !term_quit_flag) {
        int c;

        if (resized) {
            resized = 0;
            if (set_size() != 0)
                break;
        }
        if (max_frames > 0) {
            if ((c = term_read_byte()) >= 0)
                running = handle_key(c);
            if (frame_no >= max_frames)
                running = 0;
        } else {
            long long wait = next - term_now_us();

            if (wait > 0) {
                poll(&pfd, 1, (int)(wait / 1000) + 1);
                while ((c = term_read_byte()) >= 0)
                    if (!handle_key(c))
                        running = 0;
                if (term_now_us() < next)
                    continue;
            }
            next += FRAME_US;
            if (next < term_now_us())
                next = term_now_us();   /* fell behind: do not catch up */
        }
        if (!paused && state != OVER)
            step();
        draw_frame();
        render();
    }

    if (score >= best && score > 0)
        save_best();
    if (max_frames > 0) {
        term_flush();           /* leave the last frame for inspection */
        if (term_raw)
            tcsetattr(STDIN_FILENO, TCSANOW, &term_saved);
    } else {
        term_end();
    }
    return 0;
}
