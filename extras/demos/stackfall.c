/*
 * stackfall.c - a falling-block puzzle game for ANSI 256-colour terminals.
 *
 * Build:  cc -o stackfall stackfall.c
 * Keys:   arrows or WASD / HJKL, z = rotate the other way, space = drop,
 *         c = hold, p = pause, r = restart (when paused or over), q = quit.
 */
#include "term.h"
#include <poll.h>
#include <stdio.h>

#define BW 10                   /* board width in cells */
#define BH 20                   /* board height in cells */
#define BOARD_COLS ((BW + 2) * 2)
#define PANEL_COLS 27
#define SCREEN_COLS (BOARD_COLS + 3 + PANEL_COLS)
#define SCREEN_ROWS (BH + 1)

#define WALL_BG   239
#define EMPTY_BG0 233
#define EMPTY_BG1 234
#define FLASH_BG  255

enum {
    K_NONE, K_LEFT, K_RIGHT, K_DOWN, K_ROT_CW, K_ROT_CCW,
    K_DROP, K_HOLD, K_PAUSE, K_RESTART, K_QUIT
};

typedef struct { signed char x, y; } Cell;
typedef struct { int type, rot, x, y; } Piece;

/* Each shape: bounding-box size, its four blocks, and its colour. */
static const struct {
    int size;
    Cell b[4];
    unsigned char color;
} shapes[7] = {
    { 4, { {0, 1}, {1, 1}, {2, 1}, {3, 1} }, 203 },  /* long bar  */
    { 2, { {0, 0}, {1, 0}, {0, 1}, {1, 1} },  45 },  /* square    */
    { 3, { {1, 0}, {0, 1}, {1, 1}, {2, 1} }, 220 },  /* tee       */
    { 3, { {1, 0}, {2, 0}, {0, 1}, {1, 1} }, 170 },  /* step R    */
    { 3, { {0, 0}, {1, 0}, {1, 1}, {2, 1} }, 114 },  /* step L    */
    { 3, { {0, 0}, {0, 1}, {1, 1}, {2, 1} }, 208 },  /* hook L    */
    { 3, { {2, 0}, {0, 1}, {1, 1}, {2, 1} },  69 },  /* hook R    */
};

static unsigned char board[BH][BW];    /* 0 = empty, otherwise a colour */

static Piece cur;
static int have_piece;
static int next_type;
static int hold_type = -1;
static int hold_used;
static long score, lines, level, best;
static int over, paused;
static long long next_fall;
static unsigned long clear_mask;        /* rows flashing before removal */
static long long clear_until;

/* Animated hard drop: the piece accelerates down, leaving a streak. */
#define DROP_FRAME_US 16000
#define LAND_FLASH_US 90000
static int dropping;
static int drop_speed;                  /* rows per frame, grows each frame */
static int drop_start_y;
static long long drop_next;
static Cell landed[4];                  /* last locked piece, flashed white */
static long long landed_until;

/* Keys pressed during a drop are replayed once the piece lands. */
static int key_queue[8];
static int key_queue_len;

static int ox, oy;                      /* screen origin of the board */
static char hiscore_path[256];

/* ---- random numbers and the 7-piece bag ---- */

static unsigned int rng_state = 1;

static unsigned int rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int bag[7], bag_left;

static int next_from_bag(void)
{
    if (bag_left == 0) {
        for (int i = 0; i < 7; i++)
            bag[i] = i;
        for (int i = 6; i > 0; i--) {
            int j = (int)(rng() % (unsigned int)(i + 1));
            int t = bag[i];
            bag[i] = bag[j];
            bag[j] = t;
        }
        bag_left = 7;
    }
    return bag[--bag_left];
}

/* ---- piece geometry ---- */

static void piece_cells(const Piece *p, Cell out[4])
{
    int n = shapes[p->type].size;

    for (int i = 0; i < 4; i++) {
        int x = shapes[p->type].b[i].x;
        int y = shapes[p->type].b[i].y;

        for (int r = 0; r < p->rot; r++) {     /* rotate clockwise */
            int t = x;
            x = n - 1 - y;
            y = t;
        }
        out[i].x = (signed char)(p->x + x);
        out[i].y = (signed char)(p->y + y);
    }
}

static int fits(const Piece *p)
{
    Cell c[4];

    piece_cells(p, c);
    for (int i = 0; i < 4; i++) {
        if (c[i].x < 0 || c[i].x >= BW || c[i].y >= BH)
            return 0;
        if (c[i].y >= 0 && board[c[i].y][c[i].x])
            return 0;
    }
    return 1;
}

static int try_move(int dx, int dy)
{
    Piece p = cur;

    p.x += dx;
    p.y += dy;
    if (!fits(&p))
        return 0;
    cur = p;
    return 1;
}

static void try_rotate(int dir)
{
    static const int kicks[][2] = {
        {0, 0}, {-1, 0}, {1, 0}, {-2, 0}, {2, 0}, {0, -1}
    };
    Piece p = cur;

    p.rot = (p.rot + (dir > 0 ? 1 : 3)) & 3;
    for (size_t k = 0; k < sizeof kicks / sizeof kicks[0]; k++) {
        Piece q = p;

        q.x += kicks[k][0];
        q.y += kicks[k][1];
        if (fits(&q)) {
            cur = q;
            return;
        }
    }
}

/* ---- game flow ---- */

static long fall_us(void)
{
    long t = 800000;

    for (long i = 1; i < level && t > 60000; i++)
        t = t * 85 / 100;
    return t < 60000 ? 60000 : t;
}

static void load_best(void)
{
    const char *home = getenv("HOME");
    FILE *f;

    snprintf(hiscore_path, sizeof hiscore_path, "%s/.stackfall_best",
             home ? home : ".");
    f = fopen(hiscore_path, "r");
    if (f != NULL) {
        if (fscanf(f, "%ld", &best) != 1)
            best = 0;
        fclose(f);
    }
}

static void save_best(void)
{
    FILE *f = fopen(hiscore_path, "w");

    if (f != NULL) {
        fprintf(f, "%ld\n", best);
        fclose(f);
    }
}

static void game_over(void)
{
    over = 1;
    have_piece = 0;
    if (score > best) {
        best = score;
        save_best();
    }
}

static void spawn(int type, long long now)
{
    cur.type = type;
    cur.rot = 0;
    cur.x = (BW - shapes[type].size) / 2;
    cur.y = (type == 0) ? -1 : 0;   /* the bar sits on row 1 of its box */
    have_piece = 1;
    next_fall = now + fall_us();
    if (!fits(&cur))
        game_over();
}

static void spawn_next(long long now)
{
    spawn(next_type, now);
    next_type = next_from_bag();
}

static void lock_piece(long long now)
{
    Cell c[4];
    int above = 0;

    piece_cells(&cur, c);
    for (int i = 0; i < 4; i++) {
        if (c[i].y < 0)
            above = 1;
        else
            board[c[i].y][c[i].x] = shapes[cur.type].color;
        landed[i] = c[i];
    }
    landed_until = now + LAND_FLASH_US;
    have_piece = 0;
    hold_used = 0;
    if (above) {
        game_over();
        return;
    }

    clear_mask = 0;
    for (int y = 0; y < BH; y++) {
        int full = 1;

        for (int x = 0; x < BW; x++)
            if (!board[y][x])
                full = 0;
        if (full)
            clear_mask |= 1UL << y;
    }
    if (clear_mask)
        clear_until = now + 200000;     /* flash, then remove */
    else
        spawn_next(now);
}

static void finish_clear(long long now)
{
    static const int points[5] = { 0, 100, 300, 500, 800 };
    int n = 0;

    for (int y = 0; y < BH; y++) {
        if (clear_mask >> y & 1) {
            memmove(board[1], board[0], (size_t)y * BW);
            memset(board[0], 0, BW);
            n++;
        }
    }
    clear_mask = 0;
    score += points[n] * level;
    lines += n;
    level = 1 + lines / 10;
    spawn_next(now);
}

static void new_game(long long now)
{
    memset(board, 0, sizeof board);
    score = 0;
    lines = 0;
    level = 1;
    over = 0;
    paused = 0;
    clear_mask = 0;
    hold_type = -1;
    hold_used = 0;
    bag_left = 0;
    dropping = 0;
    landed_until = 0;
    key_queue_len = 0;
    next_type = next_from_bag();
    spawn_next(now);
}

static void hard_drop(long long now)
{
    Piece below = cur;

    below.y++;
    if (!fits(&below)) {        /* already resting: lock straight away */
        lock_piece(now);
        return;
    }
    dropping = 1;
    drop_speed = 1;
    drop_start_y = cur.y;
    drop_next = now;
}

/* One frame of the drop animation. Returns 1 when the piece has landed. */
static int drop_step(long long now)
{
    Piece below;

    for (int i = 0; i < drop_speed; i++) {
        if (!try_move(0, 1))
            break;
        score += 2;
    }
    drop_speed++;
    drop_next = now + DROP_FRAME_US;

    below = cur;
    below.y++;
    if (fits(&below))
        return 0;
    dropping = 0;
    lock_piece(now);
    return 1;
}

static void hold(long long now)
{
    int t = cur.type;

    if (hold_used)
        return;
    if (hold_type < 0) {
        hold_type = t;
        spawn_next(now);
    } else {
        int h = hold_type;

        hold_type = t;
        spawn(h, now);
    }
    hold_used = 1;
}

/* ---- input ---- */

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
            case 'A': return K_ROT_CW;
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
        switch (c) {
        case 'a': case 'h': return K_LEFT;
        case 'd': case 'l': return K_RIGHT;
        case 's': case 'j': return K_DOWN;
        case 'w': case 'k': case 'x': return K_ROT_CW;
        case 'z': return K_ROT_CCW;
        case ' ': return K_DROP;
        case 'c': return K_HOLD;
        case 'p': return K_PAUSE;
        case 'r': return K_RESTART;
        case 'q': case 'Q': case 3: return K_QUIT;
        default: break;
        }
    }
    return K_NONE;
}

static void handle_key(int k, long long now)
{
    if (k == K_QUIT) {
        term_quit_flag = 1;
        return;
    }
    if (dropping || (clear_mask && !over)) {
        if (key_queue_len < (int)(sizeof key_queue / sizeof key_queue[0]))
            key_queue[key_queue_len++] = k;
        return;
    }
    if (k == K_RESTART && (over || paused)) {
        new_game(now);
        return;
    }
    if (over)
        return;
    if (k == K_PAUSE) {
        paused = !paused;
        if (!paused)
            next_fall = now + fall_us();
        return;
    }
    if (paused || !have_piece)
        return;

    switch (k) {
    case K_LEFT:    try_move(-1, 0); break;
    case K_RIGHT:   try_move(1, 0); break;
    case K_ROT_CW:  try_rotate(1); break;
    case K_ROT_CCW: try_rotate(-1); break;
    case K_DROP:    hard_drop(now); break;
    case K_HOLD:    hold(now); break;
    case K_DOWN:
        if (try_move(0, 1)) {
            score += 1;
            next_fall = now + fall_us();
        } else {
            lock_piece(now);
        }
        break;
    default:
        break;
    }
}

/* ---- drawing ---- */

static void blocks(int n)
{
    for (int i = 0; i < n; i++)
        term_put("  ", 2);
}

/* A small preview of a piece in a 4x2-cell box (8 columns). */
static void draw_mini(int row, int col, int type)
{
    for (int r = 0; r < 2; r++) {
        term_goto(row + r, col);
        term_reset();
        term_str("        ");
    }
    if (type < 0)
        return;

    for (int i = 0; i < 4; i++) {
        int x = shapes[type].b[i].x;
        int y = shapes[type].b[i].y;

        if (type == 0)
            y -= 1;             /* bar: use the top row */
        else if (shapes[type].size == 2)
            x += 1;             /* square: centre it */
        term_goto(row + y, col + x * 2);
        term_bg(shapes[type].color);
        blocks(1);
    }
    term_reset();
}

static void panel_line(int line, const char *text)
{
    term_goto(oy + line, ox + BOARD_COLS + 3);
    term_reset();
    term_str(text);
    term_str("\x1b[K");
}

static void render(long long now)
{
    unsigned char view[BH][BW];
    unsigned char ghost[BH][BW];
    unsigned char trail[BH][BW];        /* 2 = just above piece, 1 = fainter */
    unsigned char flash[BH][BW];
    char text[64];
    Cell c[4];

    memcpy(view, board, sizeof view);
    memset(ghost, 0, sizeof ghost);
    memset(trail, 0, sizeof trail);
    memset(flash, 0, sizeof flash);

    if (now < landed_until)
        for (int i = 0; i < 4; i++)
            if (landed[i].y >= 0)
                flash[landed[i].y][landed[i].x] = 1;

    if (have_piece && dropping) {
        int fallen = cur.y - drop_start_y;

        piece_cells(&cur, c);
        for (int i = 0; i < 4; i++) {
            for (int d = 1; d <= 2 && d <= fallen; d++) {
                int ty = c[i].y - d;

                if (ty >= 0 && trail[ty][c[i].x] < 3 - d)
                    trail[ty][c[i].x] = (unsigned char)(3 - d);
            }
        }
    }

    if (have_piece) {
        Piece g = cur;

        for (;;) {
            Piece t = g;
            t.y++;
            if (!fits(&t))
                break;
            g = t;
        }
        piece_cells(&g, c);
        for (int i = 0; i < 4; i++)
            if (c[i].y >= 0)
                ghost[c[i].y][c[i].x] = shapes[cur.type].color;
        piece_cells(&cur, c);
        for (int i = 0; i < 4; i++)
            if (c[i].y >= 0)
                view[c[i].y][c[i].x] = shapes[cur.type].color;
    }

    /* Board rows, with a wall on each side. */
    for (int y = 0; y < BH; y++) {
        term_goto(oy + y, ox);
        term_bg(WALL_BG);
        blocks(1);
        for (int x = 0; x < BW; x++) {
            if (clear_mask >> y & 1) {
                term_bg(FLASH_BG);
                blocks(1);
            } else if (flash[y][x]) {
                term_bg(231);
                blocks(1);
            } else if (view[y][x]) {
                term_bg(view[y][x]);
                blocks(1);
            } else if (trail[y][x]) {
                term_bg((x & 1) ? EMPTY_BG1 : EMPTY_BG0);
                term_fg(shapes[cur.type].color);
                term_str(trail[y][x] == 2 ? "\xe2\x96\x92\xe2\x96\x92"    /* medium shade */
                                          : "\xe2\x96\x91\xe2\x96\x91");  /* light shade */
            } else if (ghost[y][x]) {
                term_bg((x & 1) ? EMPTY_BG1 : EMPTY_BG0);
                term_fg(ghost[y][x]);
                term_str("\xe2\x96\x91\xe2\x96\x91");     /* light shade */
            } else {
                term_bg((x & 1) ? EMPTY_BG1 : EMPTY_BG0);
                blocks(1);
            }
        }
        term_bg(WALL_BG);
        blocks(1);
    }
    term_goto(oy + BH, ox);
    term_bg(WALL_BG);
    blocks(BW + 2);
    term_reset();

    if (paused || over) {
        const char *msg = over ? "  GAME OVER  " : "   PAUSED    ";

        term_goto(oy + BH / 2 - 1, ox + 2 + (BW * 2 - 13) / 2);
        term_str("\x1b[1;97;48;5;52m");
        term_str(msg);
        term_reset();
    }

    /* Side panel. */
    panel_line(0, "\x1b[1;38;5;220mS\x1b[38;5;208mT\x1b[38;5;203mA"
                  "\x1b[38;5;170mC\x1b[38;5;69mK\x1b[38;5;45mF"
                  "\x1b[38;5;114mA\x1b[38;5;220mL\x1b[38;5;208mL\x1b[0m");
    panel_line(2, "\x1b[38;5;245mNEXT\x1b[0m");
    draw_mini(oy + 3, ox + BOARD_COLS + 3, next_type);
    panel_line(6, "\x1b[38;5;245mHOLD\x1b[0m");
    draw_mini(oy + 7, ox + BOARD_COLS + 3, hold_type);

    snprintf(text, sizeof text, "SCORE  \x1b[1m%ld", score);
    panel_line(10, text);
    snprintf(text, sizeof text, "LINES  \x1b[1m%ld", lines);
    panel_line(11, text);
    snprintf(text, sizeof text, "LEVEL  \x1b[1m%ld", level);
    panel_line(12, text);
    snprintf(text, sizeof text, "BEST   \x1b[1m%ld", best > score ? best : score);
    panel_line(13, text);

    panel_line(15, "\x1b[38;5;245m\xe2\x86\x90 \xe2\x86\x92  move    "
                   "\xe2\x86\x93  soft drop");
    panel_line(16, "\x1b[38;5;245m\xe2\x86\x91 x  rotate  z  rotate back");
    panel_line(17, "\x1b[38;5;245mspace drop    c  hold");
    panel_line(18, "\x1b[38;5;245mp  pause      q  quit");

    if (over)
        panel_line(20, "\x1b[1;38;5;203mr to play again\x1b[0m");
    else if (paused)
        panel_line(20, "\x1b[38;5;220mr to restart\x1b[0m");
    else
        panel_line(20, "");

    term_reset();
    term_flush();
}

/* ---- main loop ---- */

int main(void)
{
    int cols, rows, dirty = 1;

    term_size(&cols, &rows);
    if (cols < SCREEN_COLS || rows < SCREEN_ROWS) {
        fprintf(stderr, "stackfall needs a %dx%d terminal (this one is %dx%d)\n",
                SCREEN_COLS, SCREEN_ROWS, cols, rows);
        return 1;
    }
    ox = (cols - SCREEN_COLS) / 2 + 1;
    oy = (rows - SCREEN_ROWS) / 2 + 1;

    rng_state = (unsigned int)term_now_us() | 1u;
    load_best();
    term_begin();
    new_game(term_now_us());

    while (!term_quit_flag) {
        struct pollfd pfd;
        long long now = term_now_us();
        int timeout = 100;
        int k;

        if (dirty) {
            render(now);
            dirty = 0;
        }

        if (!over && !paused) {
            long long due = dropping ? drop_next
                          : clear_mask ? clear_until : next_fall;
            long long ms;

            if (landed_until > now && landed_until < due)
                due = landed_until;
            ms = (due - now + 999) / 1000;
            if (ms < timeout)
                timeout = ms < 0 ? 0 : (int)ms;
        }
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll(&pfd, 1, timeout);

        now = term_now_us();
        while ((k = read_key()) != K_NONE) {
            handle_key(k, now);
            dirty = 1;
        }

        if (landed_until != 0 && now >= landed_until) {
            landed_until = 0;           /* end of the landing flash */
            dirty = 1;
        }

        if (!over && !paused) {
            if (dropping) {
                if (now >= drop_next) {
                    dirty = 1;
                    drop_step(now);
                }
            } else if (clear_mask) {
                if (now >= clear_until) {
                    finish_clear(now);
                    dirty = 1;
                }
            } else if (have_piece && now >= next_fall) {
                if (!try_move(0, 1))
                    lock_piece(now);
                next_fall = now + fall_us();
                dirty = 1;
            }
        }

        /* Replay keys pressed during a drop once the next piece is in. */
        if (key_queue_len > 0 && !dropping && !clear_mask) {
            int n = key_queue_len;

            key_queue_len = 0;
            for (int i = 0; i < n; i++)
                handle_key(key_queue[i], now);
            dirty = 1;
        }
    }

    term_end();
    if (score > best) {
        best = score;
        save_best();
    }
    printf("Stackfall: score %ld, %ld lines, level %ld (best %ld)\n",
           score, lines, level, best);
    return 0;
}
