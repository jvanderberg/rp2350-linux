// SPDX-License-Identifier: MIT
/*
 * maze.c - a first-person 3D maze for ANSI 256-colour terminals.
 *
 * Build:  cc -o maze maze.c        (or ./build.sh maze)
 * Run:    ./maze          start at level 1 (a 6x6 maze)
 *         ./maze -l N     start at level N (each level is two cells wider)
 *         ./maze -demo    attract mode: it explores the mazes by itself
 *                         (any key takes over, q quits)
 *         ./maze -b N     benchmark: render N frames, print frames/second
 *         ./maze -n N     headless test: run N frames flat out, reading one
 *                         input byte per frame, then exit
 * Keys:   up/down or w/s = walk, left/right or a/d = turn,
 *         m = minimap, t = textures, f = fps, r = new maze, q = quit.
 *
 * Find the green door. Each escape starts a bigger maze.
 *
 * It is a raycaster: one ray per terminal column, stepped through the map
 * grid (DDA) in 16.16 fixed point, because this CPU has no FPU. The floor is
 * cast row by row. Each character shows two pixels (an upper half block with
 * its own foreground and background colour), and only characters that
 * changed since the last frame are sent to the terminal.
 */
#include "term.h"
#include <poll.h>
#include <stdio.h>

#define FP         16
#define ONE        (1 << FP)
#define ANG_STEPS  1024                 /* sine table entries per turn */
#define ANG_FULL   (ANG_STEPS * 16)     /* angle units per turn */
#define TURN_STEP  (ANG_FULL / 32)      /* one key press: 11.25 degrees */
#define WALK_STEP  (ONE / 8)            /* one key press: an eighth of a tile */
#define RADIUS     (ONE / 4)            /* how close you can get to a wall */
#define FRAME_US   40000                /* at most 25 frames per second */
#define PLANE_PCT  66                   /* camera plane / view dir: ~67 deg FOV */
#define LEVELS     16                   /* brightness steps per material */
#define FOG_N      512                  /* fog table index: distance in 1/16 tile */
#define BIG        (1 << 28)
#define MAX_COLS   240
#define MAX_ROWS   100
#define MAX_CELLS  40                   /* maze cells per side */
#define MAP_MAX    (MAX_CELLS * 2 + 1)
#define GAP_MAX    4                    /* resend this many unchanged chars rather than jump */
#define STALE      0xffff
#define DEMO_SPEED (ONE / 8)            /* autopilot: tiles per frame, 16.16 */
#define DEMO_LEVELS 4                   /* autopilot: start again after this level */
#define DEMO_PAUSE 3000000              /* autopilot: how long the win box stays, us */

#define BAR_BG     238
#define BAR_FG     252
#define BOX_BG     252
#define BOX_FG     16
#define MM_BORDER  240
#define MM_UNSEEN  233
#define MM_OPEN    237
#define MM_WALL    246
#define MM_DOOR    46
#define MM_PLAYER  196
#define MM_LOOK    226

enum { T_OPEN, T_WALL, T_DOOR };
enum { M_BRICK, M_MORTAR, M_DOOR, M_FRAME, M_FLOOR_A, M_FLOOR_B, M_COUNT };
enum { S_HINT, S_PLAY, S_WON };

/*
 * xterm-256 colours per material, dark to bright. Hand-picked: scaling an RGB
 * colour and taking the nearest of the 6x6x6 cube turns shaded brick olive.
 */
static const unsigned char pal[M_COUNT][LEVELS] = {
    { 16, 232, 233, 234, 235, 236, 237, 95, 95, 95, 131, 131, 167, 167, 174, 174 },
    { 16, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246 },
    { 16, 232, 22, 22, 28, 28, 34, 34, 35, 41, 41, 47, 47, 84, 84, 120 },
    { 16, 232, 233, 22, 22, 22, 22, 28, 28, 28, 29, 29, 35, 35, 36, 36 },
    { 16, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 243, 244, 244 },
    { 16, 16, 232, 232, 233, 234, 235, 236, 236, 237, 238, 238, 239, 239, 240, 240 },
};
#define SHADE_SIDE 3                    /* walls facing north/south are this much darker */

/* night sky, top row to horizon, and star brightnesses */
static const unsigned char sky[8] = { 16, 16, 232, 17, 17, 17, 18, 18 };
static const unsigned char star[4] = { 239, 245, 250, 230 };

static unsigned char fog[FOG_N];                /* brightness per distance */
static int sintab[ANG_STEPS];                   /* 16.16 */

/* the maze */
static unsigned char map[MAP_MAX * MAP_MAX];
static unsigned char seen[MAP_MAX * MAP_MAX];
static int mw, mh, ncells, level = 1;
static int open_total;
static unsigned int rng_state;

/* the player */
static int px, py, angle;                       /* 16.16 tiles, angle units */
static int pend_turn, pend_walk;                /* movement still to animate */
static int demo_on;                             /* the autopilot is driving */
static unsigned char visited[MAP_MAX * MAP_MAX];/* tiles the autopilot stood on */
static int demo_next = -1;                      /* tile it is walking to */
static int state = S_HINT;
static long long t_start, t_end;

/* the screen */
static int tcols, trows;                        /* terminal size */
static int W, H;                                /* view size in pixels */
static unsigned char *pix;                      /* W*H xterm colour indices */
static int *rowdist;                            /* floor distance per pixel row */
static unsigned short *screen;                  /* what each char shows: top<<8 | bottom */
static int cur_fg = -1, cur_bg = -1;
static int map_on = 1, tex_on = 1, fps_on;
static int box_drawn;
static char bar_shown[256];
static int fps_value;
static long long bench_bytes;
static int bench_mode;

/* ---- tables ---- */

static unsigned int rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Built once at start-up with a Taylor series, so no libm is needed. */
static void make_sintab(void)
{
    const int quarter = ANG_STEPS / 4;

    for (int i = 0; i < ANG_STEPS; i++) {
        int q = i / quarter, k = i % quarter;
        double x = (q & 1 ? quarter - k : k) * (6.283185307179586 / ANG_STEPS);
        double x2 = x * x, term = x, s = x;
        int v;

        for (int n = 1; n < 8; n++) {
            term *= -x2 / ((2 * n) * (2 * n + 1));
            s += term;
        }
        v = (int)(s * ONE + 0.5);
        sintab[i] = q >= 2 ? -v : v;
    }
}

static int isin(int a) { return sintab[(a >> 4) & (ANG_STEPS - 1)]; }
static int icos(int a) { return isin(a + ANG_FULL / 4); }

static void make_tables(void)
{
    make_sintab();
    /* light falls off with distance d (in tiles): 1 / (1 + d/2.5) */
    for (int i = 0; i < FOG_N; i++) {
        double d = i / 16.0;
        int l = (int)((LEVELS - 1) * 2.5 / (2.5 + d) + 0.5);

        fog[i] = (unsigned char)(l < 1 ? 1 : l);
    }
}

static int fog_level(int dist)
{
    int i = dist >> 12;                         /* 16.16 tiles -> 1/16 tiles */

    return fog[i < FOG_N ? i : FOG_N - 1];
}

/* ---- the maze ---- */

static unsigned char *tile(int x, int y) { return &map[y * mw + x]; }

/* Depth-first carving with an explicit stack (the stack here is small),
 * then a few extra openings so there are loops and longer sight lines. */
static void gen_maze(void)
{
    static int stack[MAX_CELLS * MAX_CELLS];
    static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
    int n = 4 + 2 * level, sp = 0;

    if (n > MAX_CELLS)
        n = MAX_CELLS;
    ncells = n;
    mw = mh = 2 * n + 1;
    memset(map, T_WALL, sizeof map);
    memset(seen, 0, sizeof seen);
    memset(visited, 0, sizeof visited);
    demo_next = -1;

    *tile(1, 1) = T_OPEN;
    stack[sp++] = 0;
    while (sp > 0) {
        int c = stack[sp - 1], cx = c % n, cy = c / n;
        int dirs[4], nd = 0;

        for (int d = 0; d < 4; d++) {
            int nx = cx + dx[d], ny = cy + dy[d];

            if (nx >= 0 && nx < n && ny >= 0 && ny < n &&
                *tile(2 * nx + 1, 2 * ny + 1) == T_WALL)
                dirs[nd++] = d;
        }
        if (nd == 0) {
            sp--;
            continue;
        }
        {
            int d = dirs[rng() % (unsigned int)nd];
            int nx = cx + dx[d], ny = cy + dy[d];

            *tile(2 * cx + 1 + dx[d], 2 * cy + 1 + dy[d]) = T_OPEN;
            *tile(2 * nx + 1, 2 * ny + 1) = T_OPEN;
            stack[sp++] = ny * n + nx;
        }
    }
    for (int i = n * n / 10; i > 0; i--) {
        int x = 1 + (int)(rng() % (unsigned int)(mw - 2));
        int y = 1 + (int)(rng() % (unsigned int)(mh - 2));

        if ((x ^ y) & 1)                        /* a wall between two cells */
            *tile(x, y) = T_OPEN;
    }
    *tile(mw - 1, mh - 2) = T_DOOR;             /* beside the far corner cell */

    open_total = 0;
    for (int i = 0; i < mw * mh; i++)
        open_total += map[i] == T_OPEN;

    px = py = ONE + ONE / 2;
    angle = *tile(2, 1) == T_OPEN ? 0 : ANG_FULL / 4;
    pend_turn = pend_walk = 0;
}

static int explored_pct(void)
{
    int n = 0;

    for (int i = 0; i < mw * mh; i++)
        n += map[i] == T_OPEN && seen[i];
    return open_total ? n * 100 / open_total : 0;
}

/* Would a player at (x, y) overlap a wall? Touching the door wins. */
static int blocked(int x, int y)
{
    int hit = 0;

    for (int k = 0; k < 4; k++) {
        int tx = (x + (k & 1 ? RADIUS : -RADIUS)) >> FP;
        int ty = (y + (k & 2 ? RADIUS : -RADIUS)) >> FP;
        int t = tx < 0 || ty < 0 || tx >= mw || ty >= mh ? T_WALL : *tile(tx, ty);

        if (t == T_DOOR && state == S_PLAY) {
            state = S_WON;
            t_end = term_now_us();
            box_drawn = 0;
        }
        if (t != T_OPEN)
            hit = 1;
    }
    return hit;
}

/* Walk d (16.16 tiles, negative is backwards), sliding along walls. */
static void walk(int d)
{
    int dx = (int)((long long)icos(angle) * d >> FP);
    int dy = (int)((long long)isin(angle) * d >> FP);

    if (!blocked(px + dx, py))
        px += dx;
    if (!blocked(px, py + dy))
        py += dy;
}

/* Take part of the pending movement: quick to start, eases to a stop. */
static int ease(int *pending, int min_step)
{
    int d = *pending * 3 / 5;

    if (abs(d) < min_step)
        d = abs(*pending) < min_step ? *pending : (*pending < 0 ? -min_step : min_step);
    *pending -= d;
    return d;
}

static int animate(void)
{
    int moving = pend_turn != 0 || pend_walk != 0;

    if (pend_turn != 0)
        angle = (angle + ease(&pend_turn, 48)) & (ANG_FULL - 1);
    if (pend_walk != 0)
        walk(ease(&pend_walk, ONE / 128));
    return moving;
}

/* ---- the autopilot (-demo) ---- */

/* An open tile worth walking to: not stood on yet, next to something unseen. */
static int frontier(int i)
{
    return map[i] == T_OPEN && !visited[i] &&
           (!seen[i - 1] || !seen[i + 1] || !seen[i - mw] || !seen[i + mw]);
}

/*
 * Breadth-first search from the player's tile. The goal is the door once it
 * has been seen, otherwise the nearest tile at the edge of what it has seen.
 * So it explores like a person would: it doesn't walk into a dead end it can
 * already see, turns back at the end of a corridor, and heads straight out
 * once it spots the door. Returns the first tile of the way there, or -1.
 */
static int demo_step(void)
{
    static short prev[MAP_MAX * MAP_MAX];
    static short queue[MAP_MAX * MAP_MAX];
    int from = (py >> FP) * mw + (px >> FP), door = (mh - 2) * mw + (mw - 1);
    int head = 0, tail = 0, goal = -1;
    int dir[4] = { 1, mw, -1, -mw };

    for (int i = 0; i < mw * mh; i++)
        prev[i] = -1;
    prev[from] = (short)from;
    queue[tail++] = (short)from;
    while (head < tail && goal < 0) {
        int c = queue[head++];
        int r = (int)(rng() & 3);               /* vary the order at junctions */

        for (int k = 0; k < 4; k++) {
            int nb = c + dir[(k + r) & 3];

            if (prev[nb] >= 0 || map[nb] == T_WALL)
                continue;
            if (map[nb] == T_DOOR && !seen[door])
                continue;                       /* it doesn't know about the door yet */
            prev[nb] = (short)c;
            if (map[nb] == T_DOOR || (!seen[door] && frontier(nb))) {
                goal = nb;
                break;
            }
            queue[tail++] = (short)nb;
        }
    }
    if (goal < 0 && seen[door] == 0) {
        seen[door] = 1;                         /* explored everything: it must be the door */
        return demo_step();
    }
    if (goal < 0)
        return -1;
    while (prev[goal] != from)
        goal = prev[goal];
    return goal;
}

/* Signed shortest turn from the current angle to a. */
static int angle_diff(int a)
{
    int diff = (a - angle) & (ANG_FULL - 1);

    return diff > ANG_FULL / 2 ? diff - ANG_FULL : diff;
}

/* Turn a bit toward angle a: fast at first, easing in at the end. */
static void demo_turn(int a)
{
    int diff = angle_diff(a);
    int step;

    step = abs(diff) / 3;
    step = step < 64 ? 64 : step > ANG_FULL / 20 ? ANG_FULL / 20 : step;
    if (abs(diff) <= step)
        angle = a;
    else
        angle = (angle + (diff > 0 ? step : -step)) & (ANG_FULL - 1);
}

/* One frame of driving. Walks from tile centre to tile centre, turning
 * as it goes, and after a win waits a moment and starts the next maze. */
static void autopilot(long long now)
{
    int tx, ty, dx, dy, a;

    if (state == S_HINT) {
        state = S_PLAY;
        t_start = now;
        box_drawn = 0;
    }
    if (state == S_WON) {
        if (now - t_end >= DEMO_PAUSE) {
            level = level >= DEMO_LEVELS ? 1 : level + 1;
            state = S_PLAY;
            gen_maze();
            t_start = now;
            box_drawn = 0;
        }
        return;
    }
    visited[(py >> FP) * mw + (px >> FP)] = 1;
    if (demo_next < 0 && (demo_next = demo_step()) < 0)
        return;
    tx = ((demo_next % mw) << FP) + ONE / 2;
    ty = ((demo_next / mw) << FP) + ONE / 2;
    dx = tx - px;
    dy = ty - py;
    a = dx > 0 ? 0 : dy > 0 ? ANG_FULL / 4 : dx < 0 ? ANG_FULL / 2 : 3 * ANG_FULL / 4;
    if (dx != 0 || dy != 0)
        demo_turn(a);
    if (abs(angle_diff(a)) > ANG_FULL / 8)
        return;                                 /* more than 45 degrees off: turn first */
    dx = clamp(dx, -DEMO_SPEED, DEMO_SPEED);
    dy = clamp(dy, -DEMO_SPEED, DEMO_SPEED);
    /* Between two open tile centres nothing is in the way; walking into the
     * door is a collision, and blocked() is what notices the win. */
    if (map[demo_next] == T_DOOR && blocked(px + dx, py + dy))
        return;
    px += dx;
    py += dy;
    if (px == tx && py == ty)
        demo_next = -1;                         /* arrived: plan the next tile */
}

/* ---- rendering into pix[] ---- */

static int brick_texel(int u, int v, int near)
{
    if (tex_on && near) {
        int row = v >> 6;                       /* four courses per tile */

        if ((v & 63) < 7 || ((u + (row & 1) * 64) & 127) < 7)
            return M_MORTAR;
    }
    return M_BRICK;
}

static int door_texel(int u, int v)
{
    if (u < 32 || u > 223 || v < 24 || (u > 120 && u < 136) || (v > 120 && v < 136))
        return M_FRAME;
    return M_DOOR;
}

static void cast_floor(int dirx, int diry, int plx, int ply)
{
    int half = H / 2;

    /* The sky turns with you: about 360/67 screen widths per full turn. */
    int turn_px = W * 54 / 10;
    int shift = (int)((long long)angle * turn_px / ANG_FULL);

    for (int y = 0; y < half; y++) {
        unsigned char *p = pix + y * W;

        memset(p, sky[y * 8 / half], (size_t)W);
        for (int x = 0; x < W; x++) {
            unsigned int h = (unsigned int)((x + shift) % turn_px) * 2654435761u
                             ^ (unsigned int)y * 40503u;

            h ^= h >> 15;
            h *= 2246822519u;
            h ^= h >> 13;
            if (h % 61u == 0)
                p[x] = star[(h >> 8) & 3];
        }
    }
    for (int y = half; y < H; y++) {
        long long rd = rowdist[y];
        /* world position under the left and right edge of this row */
        int fx = px + (int)(rd * (dirx - plx) >> FP);
        int fy = py + (int)(rd * (diry - ply) >> FP);
        int sx = (int)(rd * 2 * plx / W >> FP);
        int sy = (int)(rd * 2 * ply / W >> FP);
        int l = fog_level((int)rd);
        unsigned char ca = pal[M_FLOOR_A][l], cb = pal[M_FLOOR_B][l];
        unsigned char *p = pix + y * W;

        fx += sx / 2;
        fy += sy / 2;
        for (int x = 0; x < W; x++) {
            p[x] = ((fx ^ fy) >> FP) & 1 ? cb : ca;
            fx += sx;
            fy += sy;
        }
    }
}

/* How far the ray travels between grid lines on one axis: 1 / |rd|, 16.16. */
static int ray_delta(int rd)
{
    long long d;

    if (rd == 0)
        return BIG;
    d = ((long long)1 << 32) / abs(rd);
    return d > BIG ? BIG : (int)d;
}

static void cast_walls(int dirx, int diry, int plx, int ply)
{
    long long lq = (long long)W * ONE * 50 / PLANE_PCT;   /* W / (2 * plane), 16.16 */

    for (int x = 0; x < W; x++) {
        int cam = (int)((long long)(2 * x + 1 - W) * ONE / W);
        int rdx = dirx + (int)((long long)plx * cam >> FP);
        int rdy = diry + (int)((long long)ply * cam >> FP);
        int mx = px >> FP, my = py >> FP;
        int ddx = ray_delta(rdx), ddy = ray_delta(rdy);
        int stx = rdx < 0 ? -1 : 1, sty = rdy < 0 ? -1 : 1;
        int sdx, sdy, side = 0, t = T_WALL, perp, lh, top, y0, y1, u, l;
        unsigned int v, vstep;

        sdx = (int)((long long)(rdx < 0 ? px - (mx << FP) : ((mx + 1) << FP) - px) * ddx >> FP);
        sdy = (int)((long long)(rdy < 0 ? py - (my << FP) : ((my + 1) << FP) - py) * ddy >> FP);

        for (int i = 0; i < 4 * MAP_MAX; i++) {
            if (sdx < sdy) {
                sdx += ddx;
                mx += stx;
                side = 0;
            } else {
                sdy += ddy;
                my += sty;
                side = 1;
            }
            if (mx < 0 || my < 0 || mx >= mw || my >= mh) {
                t = T_WALL;
                break;
            }
            seen[my * mw + mx] = 1;
            t = *tile(mx, my);
            if (t != T_OPEN)
                break;
        }

        perp = side == 0 ? sdx - ddx : sdy - ddy;
        if (perp < ONE / 64)
            perp = ONE / 64;
        lh = (int)(lq / perp);
        if (lh > 16 * H)
            lh = 16 * H;
        if (lh < 1)
            lh = 1;
        top = H / 2 - lh / 2;
        y0 = top < 0 ? 0 : top;
        y1 = top + lh > H ? H : top + lh;

        /* where along the wall face the ray hit: texture column */
        u = side == 0 ? py + (int)((long long)perp * rdy >> FP)
                      : px + (int)((long long)perp * rdx >> FP);
        u = (u >> 8) & 255;
        if ((side == 0 && rdx < 0) || (side == 1 && rdy > 0))
            u = 255 - u;                        /* keep textures the right way round */

        l = fog_level(perp) - side * SHADE_SIDE;
        if (l < 1)
            l = 1;
        vstep = (256u << 16) / (unsigned int)lh;
        v = (unsigned int)(y0 - top) * vstep;
        for (int y = y0; y < y1; y++, v += vstep) {
            int tv = (int)(v >> 16) & 255;
                int m = t == T_DOOR ? door_texel(u, tv) : brick_texel(u, tv, lh >= 24);

            pix[y * W + x] = pal[m][l];
        }
    }
}

/* Top-right corner, one or two pixels per tile. A map too big for a third
 * of the width or half the height becomes a window that follows you. */
static void draw_minimap(void)
{
    int s = (W >= 160 && H >= 4 * mh + 4) ? 2 : 1;
    int vw = clamp(W / 3 / s, 1, mw), vh = clamp(H / 2 / s, 1, mh);
    int ox = W - vw * s - 2, oy = 2;
    int ptx = px >> FP, pty = py >> FP;
    int lx = (px + icos(angle) * 3 / 4) >> FP, ly = (py + isin(angle) * 3 / 4) >> FP;
    int x0 = clamp(ptx - vw / 2, 0, mw - vw), y0 = clamp(pty - vh / 2, 0, mh - vh);

    if (vw < 9 || vh < 9)
        return;
    for (int y = -1; y <= vh * s; y++)
        for (int x = -1; x <= vw * s; x++) {
            unsigned char c = MM_BORDER;

            if (x >= 0 && y >= 0 && x < vw * s && y < vh * s) {
                int tx = x0 + x / s, ty = y0 + y / s, i = ty * mw + tx;

                if (tx == ptx && ty == pty)
                    c = MM_PLAYER;
                else if (tx == lx && ty == ly && map[i] == T_OPEN)
                    c = MM_LOOK;
                else if (!seen[i])
                    c = MM_UNSEEN;
                else
                    c = map[i] == T_OPEN ? MM_OPEN : map[i] == T_DOOR ? MM_DOOR : MM_WALL;
            }
            pix[(oy + y) * W + ox + x] = c;
        }
}

static void render_view(void)
{
    int dirx = icos(angle), diry = isin(angle);
    int plx = (int)((long long)-diry * PLANE_PCT / 100);
    int ply = (int)((long long)dirx * PLANE_PCT / 100);

    cast_floor(dirx, diry, plx, ply);
    cast_walls(dirx, diry, plx, ply);
    if (map_on)
        draw_minimap();
}

/* ---- sending it to the terminal ---- */

static const char *const hint_text[] = {
    "THE MAZE",
    "",
    "Find the green door.",
    "",
    "arrows / WASD   walk and turn",
    "m  minimap      t  textures",
    "r  new maze     q  quit",
    "",
    "press any key to start",
};
#define HINT_LINES ((int)(sizeof hint_text / sizeof hint_text[0]))

static char won_lines[4][48];

static int box_lines(const char *const **lines)
{
    static const char *won[4];

    if (state == S_HINT) {
        *lines = hint_text;
        return HINT_LINES;
    }
    if (state == S_WON) {
        for (int i = 0; i < 4; i++)
            won[i] = won_lines[i];
        *lines = won;
        return 4;
    }
    return 0;
}

static int box_w(int n, const char *const *lines)
{
    int w = 0;

    for (int i = 0; i < n; i++)
        if ((int)strlen(lines[i]) > w)
            w = (int)strlen(lines[i]);
    return w + 4;
}

static int under_box(int tx, int ty, int bx, int by, int bw, int bh)
{
    return bw > 0 && tx >= bx && tx < bx + bw && ty >= by && ty < by + bh;
}

static void draw_box(int bx, int by, int bw, int n, const char *const *lines)
{
    term_bg(BOX_BG);
    term_fg(BOX_FG);
    for (int i = 0; i < n + 2; i++) {
        const char *s = (i == 0 || i == n + 1) ? "" : lines[i - 1];
        int len = (int)strlen(s), pad = (bw - len) / 2;

        term_goto(by + i + 1, bx + 1);
        for (int j = 0; j < bw; j++)
            term_put(j >= pad && j < pad + len ? s + j - pad : " ", 1);
    }
    term_reset();
    cur_fg = cur_bg = -1;
}

static void put_cell(unsigned short v)
{
    int t = v >> 8, b = v & 255;

    if (t == b) {
        if (cur_bg == b) {
            term_put(" ", 1);
            return;
        }
        if (cur_fg == t) {
            term_str("\xe2\x96\x88");          /* full block */
            return;
        }
        term_bg(b);
        cur_bg = b;
        term_put(" ", 1);
        return;
    }
    if (cur_fg != t) {
        term_fg(t);
        cur_fg = t;
    }
    if (cur_bg != b) {
        term_bg(b);
        cur_bg = b;
    }
    term_str("\xe2\x96\x80");
}

static void draw_bar(long long now)
{
    char left[128], right[96], s[256];
    long long us = (state == S_WON ? t_end : now) - t_start;
    int secs = state == S_HINT ? 0 : (int)(us / 1000000);
    int nl, nr, gap;

    nl = snprintf(left, sizeof left, " %sLEVEL %d  %dx%d  %d:%02d  explored %d%%",
                  demo_on ? "DEMO  " : "", level, ncells, ncells, secs / 60, secs % 60,
                  explored_pct());
    if (fps_on)
        nl += snprintf(left + nl, sizeof left - (size_t)nl, "  %d fps", fps_value);
    nr = snprintf(right, sizeof right, "%s",
                  demo_on ? "any key: take over  q quit " : "arrows/WASD  m map  r new  q quit ");
    gap = tcols - 1 - nl - nr;
    if (gap < 2) {
        nr = 0;
        gap = tcols - 1 - nl;
    }
    if (gap < 0) {
        nl = tcols - 1;
        gap = 0;
    }
    snprintf(s, sizeof s, "%.*s%*s%.*s", nl, left, gap, "", nr, right);
    if (strcmp(s, bar_shown) == 0)
        return;
    strcpy(bar_shown, s);
    term_goto(trows, 1);
    term_bg(BAR_BG);
    term_fg(BAR_FG);
    term_str(s);
    term_reset();
    cur_fg = cur_bg = -1;
}

static void present(long long now)
{
    const char *const *lines = NULL;
    int n = box_lines(&lines);
    int bw = n ? box_w(n, lines) : 0, bh = n + 2;
    int bx = (tcols - bw) / 2, by = (trows - 1 - bh) / 2;

    if (bx < 0 || by < 0)
        bw = 0;                                 /* too small for the box */
    for (int ty = 0; ty < trows - 1; ty++) {
        const unsigned char *a = pix + (size_t)(ty * 2) * W;
        const unsigned char *b = a + W;
        unsigned short *s = screen + (size_t)ty * tcols;
        int pos = -1;                           /* terminal cursor column in this row */

        for (int tx = 0; tx < tcols; tx++) {
            unsigned short v = (unsigned short)(a[tx] << 8 | b[tx]);

            if (under_box(tx, ty, bx, by, bw, bh)) {
                s[tx] = STALE;                  /* repaint once the box closes */
                pos = -1;                       /* never gap-fill across the box */
                continue;
            }
            if (s[tx] == v)
                continue;
            if (pos < 0 || tx - pos > GAP_MAX) {
                term_goto(ty + 1, tx + 1);
            } else {
                for (int g = pos; g < tx; g++)
                    put_cell(s[g]);
            }
            put_cell(v);
            s[tx] = v;
            pos = tx + 1;
        }
    }
    if (bw > 0 && !box_drawn) {
        draw_box(bx, by, bw, n, lines);
        box_drawn = 1;
    }
    draw_bar(now);
    if (bench_mode) {
        bench_bytes += (long long)term_len;     /* counted, not sent */
        term_len = 0;
    } else {
        term_flush();
    }
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

static void new_maze(long long now)
{
    gen_maze();
    t_start = now;
    box_drawn = 0;
}

static void push(int *pending, int step, int cap)
{
    *pending += step;
    if (*pending > cap)
        *pending = cap;
    if (*pending < -cap)
        *pending = -cap;
}

/* Returns 1 if the view needs redrawing. */
static int handle_key(int k, long long now)
{
    if (k == 'q' || k == 'Q') {
        term_quit_flag = 1;
        return 0;
    }
    if (demo_on) {                              /* any other key takes over */
        demo_on = 0;
        if (state == S_WON) {
            level++;
            state = S_PLAY;
            new_maze(now);
        }
        box_drawn = 0;
        return 1;
    }
    if (state == S_HINT) {
        state = S_PLAY;
        t_start = now;
        box_drawn = 0;
        return 1;
    }
    if (state == S_WON) {
        if (k == ' ' || k == '\r' || k == '\n' || k == 'n' || k == 'N') {
            level++;
            state = S_PLAY;
            new_maze(now);
            return 1;
        }
        return 0;
    }
    switch (k) {
    case K_UP:    case 'w': case 'W': case 'k': push(&pend_walk, WALK_STEP, 2 * WALK_STEP); break;
    case K_DOWN:  case 's': case 'S': case 'j': push(&pend_walk, -WALK_STEP, 2 * WALK_STEP); break;
    case K_LEFT:  case 'a': case 'A': case 'h': push(&pend_turn, -TURN_STEP, 2 * TURN_STEP); break;
    case K_RIGHT: case 'd': case 'D': case 'l': push(&pend_turn, TURN_STEP, 2 * TURN_STEP); break;
    case 'm': case 'M': map_on = !map_on; return 1;
    case 't': case 'T': tex_on = !tex_on; return 1;
    case 'f': case 'F': fps_on = !fps_on; return 0;
    case 'r': case 'R': new_maze(now); return 1;
    }
    return 0;
}

/* ---- set-up and main ---- */

static int setup_screen(int cols, int rows)
{
    tcols = cols > MAX_COLS ? MAX_COLS : cols;
    trows = rows > MAX_ROWS ? MAX_ROWS : rows;
    W = tcols;
    H = (trows - 1) * 2;
    pix = malloc((size_t)W * (size_t)H);
    rowdist = malloc(sizeof *rowdist * (size_t)H);
    screen = malloc(sizeof *screen * (size_t)tcols * (size_t)(trows - 1));
    if (pix == NULL || rowdist == NULL || screen == NULL)
        return -1;
    for (int i = 0; i < tcols * (trows - 1); i++)
        screen[i] = STALE;
    /* The eye is half a tile up, so a floor pixel dy rows below the horizon
     * is (W / (2 * plane)) / (2 * dy) tiles away. */
    for (int y = 0; y < H; y++) {
        int dy2 = 2 * y + 1 - H;                /* twice the offset from the horizon */
        long long lq = (long long)W * ONE * 50 / PLANE_PCT;

        if (dy2 < 0)
            dy2 = -dy2;
        rowdist[y] = (int)(lq / dy2);
    }
    return 0;
}

static int bench(int frames)
{
    long long t0, t1;
    int cols, rows;

    term_size(&cols, &rows);
    if (setup_screen(cols, rows) < 0)
        return 1;
    bench_mode = 1;
    state = S_PLAY;
    gen_maze();
    t0 = term_now_us();
    for (int i = 0; i < frames; i++) {
        angle = (int)((long long)ANG_FULL * i / frames);
        render_view();
        present(t0);
    }
    t1 = term_now_us();
    printf("%dx%d pixels, %d frames: %lld ms, %.1f frames/s, %lld bytes/frame\n",
           W, H, frames, (t1 - t0) / 1000,
           t1 > t0 ? frames * 1e6 / (double)(t1 - t0) : 0.0, bench_bytes / (frames ? frames : 1));
    return 0;
}

static void main_won_text(void)
{
    int secs = (int)((t_end - t_start) / 1000000);

    snprintf(won_lines[0], sizeof won_lines[0], "YOU ESCAPED!");
    snprintf(won_lines[1], sizeof won_lines[1], "level %d in %d:%02d, %d%% explored",
             level, secs / 60, secs % 60, explored_pct());
    won_lines[2][0] = '\0';
    snprintf(won_lines[3], sizeof won_lines[3], "%s",
             demo_on ? (level >= DEMO_LEVELS ? "starting over..." : "next maze coming up...")
                     : "space: a bigger maze   q: quit");
}

int main(int argc, char **argv)
{
    long long next_frame, fps_t0;
    int max_frames = -1, frames = 0, fps_count = 0, dirty = 1, cols, rows;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            rng_state = 12345;
            make_tables();
            return bench(atoi(argv[++i]));
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-demo") == 0 || strcmp(argv[i], "-d") == 0) {
            demo_on = 1;
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            level = atoi(argv[++i]);
            if (level < 1)
                level = 1;
        } else {
            fprintf(stderr, "usage: maze [-demo] [-l LEVEL] [-b FRAMES] [-n FRAMES]\n");
            return 2;
        }
    }

    term_size(&cols, &rows);
    if (cols < 40 || rows < 12) {
        fprintf(stderr, "maze needs at least a 40x12 terminal (this one is %dx%d)\n",
                cols, rows);
        return 1;
    }
    if (setup_screen(cols, rows) < 0) {
        fprintf(stderr, "maze: out of memory\n");
        return 1;
    }
    make_tables();
    rng_state = max_frames >= 0 ? 12345u : ((unsigned int)term_now_us() | 1u);
    new_maze(term_now_us());

    term_begin();
    next_frame = fps_t0 = term_now_us();
    while (!term_quit_flag) {
        long long now = term_now_us();
        int moving, k;

        if (max_frames >= 0) {                  /* headless: one byte per frame */
            if (frames++ >= max_frames)
                break;
            if ((k = read_key()) != K_NONE)
                dirty |= handle_key(k, now);
        } else {
            struct pollfd pfd;
            long long wait = demo_on || pend_turn || pend_walk ? next_frame - now : 250000;

            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;
            pfd.revents = 0;
            poll(&pfd, 1, wait > 0 ? (int)((wait + 999) / 1000) : 0);
            now = term_now_us();
            while ((k = read_key()) != K_NONE)
                dirty |= handle_key(k, now);
            if (now < next_frame && !dirty)
                continue;                       /* too soon for another frame */
        }

        if (demo_on) {
            int was = state;

            autopilot(now);
            dirty |= state == S_PLAY;           /* it is always on the move */
            dirty |= state != was;
        }
        moving = animate();
        if (moving || dirty) {
            render_view();
            fps_count++;
        }
        if (state == S_WON && !box_drawn)
            main_won_text();
        present(now);
        dirty = 0;
        if (now - fps_t0 >= 1000000) {
            fps_value = (int)(fps_count * 1000000LL / (now - fps_t0));
            fps_count = 0;
            fps_t0 = now;
        }
        next_frame = now + FRAME_US;
    }

    term_end();
    printf("Maze: reached level %d\n", level);
    return 0;
}
