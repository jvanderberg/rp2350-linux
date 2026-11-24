// SPDX-License-Identifier: MIT
/*
 * xmaze.c - the first-person maze of maze.c, in an X11 window.
 *
 * Build:  cc -o xmaze xmaze.c
 * Run:    DISPLAY=HOST:0 ./xmaze       (see x11.h for letting the board in)
 *         ./xmaze -demo                 it explores by itself; any key takes over
 *         ./xmaze -l N                  start at level N
 *         ./xmaze -b N                  benchmark: N frames turning on the spot
 * Keys:   arrows or WASD (hold them), m = minimap, t = brick lines,
 *         f = frames/s and bytes, r = new maze, q or Escape = quit.
 *
 * Find the green door; each escape starts a maze two cells wider.
 *
 * Sending pixels is no good here: the board reaches the X server at a few
 * hundred KB/s, and a 320x200 frame is 256 KB. So the X server does the
 * drawing. A raycaster (one ray per two screen columns, 16.16 fixed point)
 * finds which wall face each column sees; each run of columns on one face
 * becomes a single filled polygon in that face's fogged colour, with the
 * mortar as line segments. The floor is a quad per tile the rays crossed,
 * the starry sky a panorama drawn once into a canvas and copied each frame,
 * and the minimap a canvas that gets only the tiles newly seen. A frame is
 * a few KB, and the picture is sharp at any window size.
 */
#include "x11.h"
#include <time.h>

#define FP          16
#define ONE         (1 << FP)
#define ANG_STEPS   1024
#define ANG_FULL    (ANG_STEPS * 16)
#define RADIUS      (ONE / 4)
#define TICK_US     33333               /* the game runs at 30 steps a second */
#define WALK_SPEED  (ONE / 12)          /* per step: 2.5 tiles/s */
#define TURN_SPEED  (ANG_FULL / 96)     /* per step: 112 degrees/s */
#define PLANE_PCT   66                  /* field of view, as in maze.c */
#define COL_PX      2                   /* screen pixels per ray */
#define MAX_W       2048
#define MAX_RAYS    (MAX_W / COL_PX + 1)
#define NEAR        (ONE / 8)           /* near clipping plane */
#define LEVELS      24                  /* fog steps */
#define FOG_N       512
#define BIG         (1 << 28)
#define MAX_CELLS   40
#define MAP_MAX     (MAX_CELLS * 2 + 1)
#define BAR_H       17
#define MAX_FLOOR   1024                /* floor tiles drawn per frame, at most */
#define DEMO_SPEED  (ONE / 10)          /* autopilot, per step: 3 tiles/s */
#define DEMO_LEVELS 4
#define DEMO_PAUSE  3000000

enum { T_OPEN, T_WALL, T_DOOR };
enum { S_HINT, S_PLAY, S_WON };
enum {
    M_BRICK, M_BRICK_DK, M_MORTAR, M_MORTAR_DK, M_DOOR, M_DOOR_DK, M_FRAME,
    M_FLOOR_A, M_FLOOR_B, M_COUNT
};

static const unsigned char material_rgb[M_COUNT][3] = {
    { 200,  96,  64 }, { 150,  70,  48 },       /* brick: lit side, shadow side */
    { 112, 104,  96 }, {  84,  78,  72 },       /* mortar */
    {  80, 235, 120 }, {  60, 185,  95 },       /* door */
    {  25, 100,  55 },                          /* door frame */
    { 104,  96,  86 }, {  64,  60,  56 },       /* floor checks */
};

static unsigned int pal[M_COUNT][LEVELS];
static unsigned char fog[FOG_N];
static int sintab[ANG_STEPS];
static unsigned int rng_state;

/* the maze */
static unsigned char map[MAP_MAX * MAP_MAX];
static unsigned char seen[MAP_MAX * MAP_MAX];
static unsigned char visited[MAP_MAX * MAP_MAX];
static int mw, mh, ncells, level = 1, open_total;

/* the player */
static int px, py, angle;
static int state = S_HINT, demo_on, demo_next = -1;
static long long t_start, t_end;
static int held_fwd, held_back, held_left, held_right;

/* the view */
static int W, H, focal;
static int dirx, diry, rightx, righty;
static int col_face[MAX_RAYS], col_z[MAX_RAYS];
static int floor_list[MAX_FLOOR], nfloor;
static int floor_stamp[MAP_MAX * MAP_MAX], stamp;
static int newly[MAP_MAX * MAP_MAX], nnewly;
static int map_on = 1, tex_on = 1, fps_on;
static unsigned int sky, minimap;
static int sky_w, sky_h, mm_s;
static int fps_value, kb_value;

/* ---- maths and tables ---- */

static long long now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static unsigned int rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

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
    for (int m = 0; m < M_COUNT; m++)
        for (int l = 0; l < LEVELS; l++)
            pal[m][l] = x_rgb(material_rgb[m][0] * l / (LEVELS - 1),
                              material_rgb[m][1] * l / (LEVELS - 1),
                              material_rgb[m][2] * l / (LEVELS - 1));
    for (int i = 0; i < FOG_N; i++) {
        double d = i / 16.0;
        int l = (int)((LEVELS - 1) * 2.5 / (2.5 + d) + 0.5);

        fog[i] = (unsigned char)(l < 1 ? 1 : l);
    }
}

static int fog_level(int z)
{
    int i = z >> 12;

    return fog[i < 0 ? 0 : i < FOG_N ? i : FOG_N - 1];
}

/* ---- the maze (as in maze.c) ---- */

static unsigned char *tile(int x, int y) { return &map[y * mw + x]; }

static void mark_seen(int i)
{
    if (!seen[i]) {
        seen[i] = 1;
        newly[nnewly++] = i;
    }
}

static void new_minimap(void);

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
    nnewly = 0;
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

        if ((x ^ y) & 1)
            *tile(x, y) = T_OPEN;
    }
    *tile(mw - 1, mh - 2) = T_DOOR;

    open_total = 0;
    for (int i = 0; i < mw * mh; i++)
        open_total += map[i] == T_OPEN;

    px = py = ONE + ONE / 2;
    angle = *tile(2, 1) == T_OPEN ? 0 : ANG_FULL / 4;
    new_minimap();
}

static int explored_pct(void)
{
    int n = 0;

    for (int i = 0; i < mw * mh; i++)
        n += map[i] == T_OPEN && seen[i];
    return open_total ? n * 100 / open_total : 0;
}

static int blocked(int x, int y)
{
    int hit = 0;

    for (int k = 0; k < 4; k++) {
        int tx = (x + (k & 1 ? RADIUS : -RADIUS)) >> FP;
        int ty = (y + (k & 2 ? RADIUS : -RADIUS)) >> FP;
        int t = tx < 0 || ty < 0 || tx >= mw || ty >= mh ? T_WALL : *tile(tx, ty);

        if (t == T_DOOR && state == S_PLAY) {
            state = S_WON;
            t_end = now_us();
        }
        if (t != T_OPEN)
            hit = 1;
    }
    return hit;
}

static void walk(int dx, int dy)
{
    if (!blocked(px + dx, py))
        px += dx;
    if (!blocked(px, py + dy))
        py += dy;
}

/* ---- the autopilot (as in maze.c) ---- */

static int frontier(int i)
{
    return map[i] == T_OPEN && !visited[i] &&
           (!seen[i - 1] || !seen[i + 1] || !seen[i - mw] || !seen[i + mw]);
}

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
        int r = (int)(rng() & 3);

        for (int k = 0; k < 4; k++) {
            int nb = c + dir[(k + r) & 3];

            if (prev[nb] >= 0 || map[nb] == T_WALL)
                continue;
            if (map[nb] == T_DOOR && !seen[door])
                continue;
            prev[nb] = (short)c;
            if (map[nb] == T_DOOR || (!seen[door] && frontier(nb))) {
                goal = nb;
                break;
            }
            queue[tail++] = (short)nb;
        }
    }
    if (goal < 0 && seen[door] == 0) {
        mark_seen(door);                /* explored everything: it must be the door */
        return demo_step();
    }
    if (goal < 0)
        return -1;
    while (prev[goal] != from)
        goal = prev[goal];
    return goal;
}

static int angle_diff(int a)
{
    int diff = (a - angle) & (ANG_FULL - 1);

    return diff > ANG_FULL / 2 ? diff - ANG_FULL : diff;
}

static void demo_turn(int a)
{
    int diff = angle_diff(a);
    int step = clamp(abs(diff) / 3, 64, ANG_FULL / 24);

    if (abs(diff) <= step)
        angle = a;
    else
        angle = (angle + (diff > 0 ? step : -step)) & (ANG_FULL - 1);
}

/* One step of driving; returns 1 if anything moved. */
static int autopilot(long long now)
{
    int tx, ty, dx, dy, a;

    if (state == S_HINT) {
        state = S_PLAY;
        t_start = now;
        return 1;
    }
    if (state == S_WON) {
        if (now - t_end < DEMO_PAUSE)
            return 0;
        level = level >= DEMO_LEVELS ? 1 : level + 1;
        state = S_PLAY;
        gen_maze();
        t_start = now;
        return 1;
    }
    visited[(py >> FP) * mw + (px >> FP)] = 1;
    if (demo_next < 0 && (demo_next = demo_step()) < 0)
        return 0;
    tx = ((demo_next % mw) << FP) + ONE / 2;
    ty = ((demo_next / mw) << FP) + ONE / 2;
    dx = tx - px;
    dy = ty - py;
    a = dx > 0 ? 0 : dy > 0 ? ANG_FULL / 4 : dx < 0 ? ANG_FULL / 2 : 3 * ANG_FULL / 4;
    if (dx != 0 || dy != 0)
        demo_turn(a);
    if (abs(angle_diff(a)) > ANG_FULL / 8)
        return 1;
    dx = clamp(dx, -DEMO_SPEED, DEMO_SPEED);
    dy = clamp(dy, -DEMO_SPEED, DEMO_SPEED);
    if (map[demo_next] == T_DOOR && blocked(px + dx, py + dy))
        return 1;
    px += dx;
    py += dy;
    if (px == tx && py == ty)
        demo_next = -1;
    return 1;
}

/* ---- projection ---- */

/* World point (16.16 tiles) to camera space: lat across, z ahead. */
static void to_camera(int wx, int wy, int *lat, int *z)
{
    long long rx = wx - px, ry = wy - py;

    *z = (int)((rx * dirx + ry * diry) >> FP);
    *lat = (int)((rx * rightx + ry * righty) >> FP);
}

static int screen_x(int lat, int z)
{
    return clamp(W / 2 + (int)((long long)focal * lat / z), -16000, 16000);
}

/* Half the height on screen of a wall z away (the eye is half a tile up). */
static int half_h(int z)
{
    long long h = (long long)focal * (ONE / 2) / (z < NEAR ? NEAR : z);

    return h > 16000 ? 16000 : (int)h;
}

/* Distance to face (code: 0 west, 1 east, 2 north, 3 south of tile t)
 * along the view ray through screen column edge x. */
static int face_z(int code, int t, int x)
{
    int kq = (int)((long long)(x - W / 2) * ONE / focal);
    int rdx = dirx + (int)((long long)rightx * kq >> FP);
    int rdy = diry + (int)((long long)righty * kq >> FP);
    int line, num, den;
    long long z;

    if (code < 2) {
        line = (t % mw + (code == 1)) << FP;
        num = line - px;
        den = rdx;
    } else {
        line = (t / mw + (code == 3)) << FP;
        num = line - py;
        den = rdy;
    }
    if (den == 0)
        return BIG;
    z = (long long)num * ONE / den;
    return z < NEAR ? NEAR : z > BIG ? BIG : (int)z;
}

/* ---- casting ---- */

static int ray_delta(int rd)
{
    long long d;

    if (rd == 0)
        return BIG;
    d = ((long long)1 << 32) / abs(rd);
    return d > BIG ? BIG : (int)d;
}

static void add_floor(int i)
{
    if (floor_stamp[i] != stamp && nfloor < MAX_FLOOR) {
        floor_stamp[i] = stamp;
        floor_list[nfloor++] = i;
    }
}

static void cast(int rays)
{
    stamp++;
    nfloor = 0;
    add_floor((py >> FP) * mw + (px >> FP));
    for (int c = 0; c < rays; c++) {
        int xc = c * COL_PX + COL_PX / 2;
        int kq = (int)((long long)(xc - W / 2) * ONE / focal);
        int rdx = dirx + (int)((long long)rightx * kq >> FP);
        int rdy = diry + (int)((long long)righty * kq >> FP);
        int mx = px >> FP, my = py >> FP;
        int ddx = ray_delta(rdx), ddy = ray_delta(rdy);
        int stx = rdx < 0 ? -1 : 1, sty = rdy < 0 ? -1 : 1;
        int sdx = (int)((long long)(rdx < 0 ? px - (mx << FP) : ((mx + 1) << FP) - px) * ddx >> FP);
        int sdy = (int)((long long)(rdy < 0 ? py - (my << FP) : ((my + 1) << FP) - py) * ddy >> FP);
        int side = 0, hit = 0;

        col_face[c] = -1;
        for (int i = 0; i < 4 * MAP_MAX && !hit; i++) {
            int t;

            if (sdx < sdy) {
                sdx += ddx;
                mx += stx;
                side = 0;
            } else {
                sdy += ddy;
                my += sty;
                side = 1;
            }
            if (mx < 0 || my < 0 || mx >= mw || my >= mh)
                break;
            t = my * mw + mx;
            mark_seen(t);
            if (map[t] == T_OPEN) {
                add_floor(t);
                continue;
            }
            hit = 1;
            col_z[c] = side == 0 ? sdx - ddx : sdy - ddy;
            col_face[c] = t << 2 | (side == 0 ? (stx > 0 ? 0 : 1) : (sty > 0 ? 2 : 3));
        }
    }
}

/* ---- drawing ---- */

static void draw_sky(void)
{
    int sx = (int)((long long)angle * sky_w / ANG_FULL) - W / 2;
    int first;

    sx %= sky_w;
    if (sx < 0)
        sx += sky_w;
    first = sky_w - sx < W ? sky_w - sx : W;
    x_blit(sky, sx, 0, first, sky_h, 0, 0);
    if (first < W)
        x_blit(sky, 0, 0, W - first, sky_h, first, 0);
}

/* The floor: the tiles the rays crossed, a quad each, clipped at NEAR. */
static void draw_floor(void)
{
    static int key_of[MAX_FLOOR], order[MAX_FLOOR], count[2 * LEVELS + 1];
    static short polys[MAX_FLOOR][12];
    static unsigned char npts[MAX_FLOOR];

    x_color(pal[M_FLOOR_B][1]);
    x_fill_rect(0, H / 2, W, H - H / 2);
    for (int i = 0; i < nfloor; i++) {
        int t = floor_list[i], tx = t % mw, ty = t / mw;
        int in[8], out[12], n = 0, cl, cz;
        static const int cx[4] = { 0, 1, 1, 0 }, cy[4] = { 0, 0, 1, 1 };

        for (int k = 0; k < 4; k++)
            to_camera((tx + cx[k]) << FP, (ty + cy[k]) << FP, &in[2 * k], &in[2 * k + 1]);
        /* Sutherland-Hodgman against z >= NEAR */
        for (int k = 0; k < 4; k++) {
            const int *a = &in[2 * ((k + 3) & 3)], *b = &in[2 * k];

            if ((a[1] >= NEAR) != (b[1] >= NEAR)) {
                out[2 * n] = a[0] + (int)((long long)(b[0] - a[0]) * (NEAR - a[1]) / (b[1] - a[1]));
                out[2 * n + 1] = NEAR;
                n++;
            }
            if (b[1] >= NEAR) {
                out[2 * n] = b[0];
                out[2 * n + 1] = b[1];
                n++;
            }
        }
        npts[i] = (unsigned char)n;
        for (int k = 0; k < n; k++) {
            polys[i][2 * k] = (short)screen_x(out[2 * k], out[2 * k + 1]);
            polys[i][2 * k + 1] = (short)clamp(H / 2 + half_h(out[2 * k + 1]), -16000, 16000);
        }
        to_camera((tx << FP) + ONE / 2, (ty << FP) + ONE / 2, &cl, &cz);
        key_of[i] = ((tx ^ ty) & 1) * LEVELS + fog_level(cz);
    }
    /* draw grouped by colour, so the colour changes as rarely as it can */
    memset(count, 0, sizeof count);
    for (int i = 0; i < nfloor; i++)
        count[key_of[i] + 1]++;
    for (int k = 1; k <= 2 * LEVELS; k++)
        count[k] += count[k - 1];
    for (int i = 0; i < nfloor; i++)
        order[count[key_of[i]]++] = i;
    for (int j = 0; j < nfloor; j++) {
        int i = order[j], k = key_of[i];

        if (npts[i] < 3)
            continue;
        x_color(pal[k >= LEVELS ? M_FLOOR_B : M_FLOOR_A][k % LEVELS]);
        x_fill_poly(polys[i], npts[i]);
    }
}

/* Mortar or door-frame lines across one run of a wall face. */
static void face_lines(int code, int t, int x0, int x1, int h0, int h1, int door)
{
    static short seg[4 * 32];
    int n = 0, cy = H / 2;
    int ax = (t % mw + (code == 1)) << FP, ay = (t / mw + (code == 3)) << FP;
    int ux = code < 2 ? 0 : ONE, uy = code < 2 ? ONE : 0;   /* along the face */
    static const int brick_v[3] = { 64, 128, 192 };          /* course lines, /256 */
    static const int door_v[2] = { 20, 128 };

    for (int i = 0; i < (door ? 2 : 3); i++) {
        int v = door ? door_v[i] : brick_v[i];

        seg[n++] = (short)x0;
        seg[n++] = (short)(cy - h0 + 2 * h0 * v / 256);
        seg[n++] = (short)x1;
        seg[n++] = (short)(cy - h1 + 2 * h1 * v / 256);
    }
    /* vertical joints: brick courses are staggered; the door has a frame */
    for (int course = 0; course < (door ? 1 : 4); course++)
        for (int j = 0; j < (door ? 3 : 2); j++) {
            static const int door_u[3] = { 20, 128, 236 };
            int u = door ? door_u[j] : (course & 1 ? 64 : 0) + 128 * j;
            int lat, z, sx, h, v0 = door ? 0 : course * 64, v1 = door ? 256 : v0 + 64;

            to_camera(ax + (int)((long long)ux * u / 256), ay + (int)((long long)uy * u / 256),
                      &lat, &z);
            if (z < NEAR)
                continue;
            sx = screen_x(lat, z);
            if (sx < x0 || sx >= x1)
                continue;
            h = half_h(z);
            seg[n++] = (short)sx;
            seg[n++] = (short)(cy - h + 2 * h * v0 / 256);
            seg[n++] = (short)sx;
            seg[n++] = (short)(cy - h + 2 * h * v1 / 256);
        }
    x_segments(seg, n / 4);
}

static void draw_walls(int rays)
{
    int lw = 1;

    for (int c0 = 0; c0 < rays;) {
        int face = col_face[c0], c1 = c0, code, t, x0, x1, h0, h1, l, door, dark;
        short quad[8];

        while (c1 + 1 < rays && col_face[c1 + 1] == face)
            c1++;
        if (face < 0) {
            c0 = c1 + 1;
            continue;
        }
        code = face & 3;
        t = face >> 2;
        x0 = c0 * COL_PX;
        x1 = (c1 + 1) * COL_PX;
        if (x1 > W)
            x1 = W;
        h0 = half_h(face_z(code, t, x0));
        h1 = half_h(face_z(code, t, x1));
        l = fog_level(col_z[(c0 + c1) / 2]);
        door = map[t] == T_DOOR;
        dark = code >= 2;               /* north and south faces are in shadow */
        quad[0] = (short)x0;
        quad[1] = (short)(H / 2 - h0);
        quad[2] = (short)x1;
        quad[3] = (short)(H / 2 - h1);
        quad[4] = (short)x1;
        quad[5] = (short)(H / 2 + h1);
        quad[6] = (short)x0;
        quad[7] = (short)(H / 2 + h0);
        x_color(pal[door ? M_DOOR + dark : M_BRICK + dark][l]);
        x_fill_poly(quad, 4);
        if ((tex_on || door) && (h0 > 10 || h1 > 10)) {
            int want = clamp((h0 > h1 ? h0 : h1) / 60 + 1, 1, 4);

            if (want != lw) {
                x_line_width(want);
                lw = want;
            }
            x_color(pal[door ? M_FRAME : M_MORTAR + dark][l]);
            face_lines(code, t, x0, x1, h0, h1, door);
        }
        c0 = c1 + 1;
    }
    if (lw != 1)
        x_line_width(1);
}

/* ---- the canvases: sky and minimap ---- */

static const unsigned char mm_rgb[4][3] = {
    { 22, 22, 30 }, { 72, 72, 84 }, { 150, 150, 162 }, { 60, 230, 110 },   /* unseen open wall door */
};

static void new_sky(void)
{
    unsigned int st = 0x2545f491u;

    x_free_canvas(sky);
    sky_w = focal * 6283 / 1000;        /* 2 pi focal: one full turn */
    sky_h = H / 2;
    sky = x_canvas(sky_w, sky_h);
    x_target(sky);
    for (int b = 0; b < 16; b++) {      /* black at the top to deep blue at the horizon */
        int y0 = sky_h * b / 16, y1 = sky_h * (b + 1) / 16;

        x_color(x_rgb(2 + b * 22 / 15, 2 + b * 32 / 15, 12 + b * 78 / 15));
        x_fill_rect(0, y0, sky_w, y1 - y0);
    }
    for (int k = 0; k < 3; k++) {       /* stars, three brightnesses */
        static short r[4 * 200];
        int n = 0;

        for (int i = 0; i < sky_w * sky_h / 6000 && n < 200; i++, n++) {
            st ^= st << 13;
            st ^= st >> 17;
            st ^= st << 5;
            r[4 * n] = (short)(st % (unsigned int)sky_w);
            r[4 * n + 1] = (short)((st >> 12) % (unsigned int)(sky_h * 7 / 8));
            r[4 * n + 2] = r[4 * n + 3] = (short)(k == 2 ? 2 : 1);
        }
        x_color(x_rgb(120 + 60 * k, 120 + 60 * k, 130 + 60 * k));
        x_fill_rects(r, n);
    }
    x_target(0);
}

static void new_minimap(void)
{
    if (W == 0 || mw == 0)
        return;                         /* no window or no maze yet */
    x_free_canvas(minimap);
    mm_s = clamp((H / 2) / mh < (W / 3) / mw ? (H / 2) / mh : (W / 3) / mw, 2, 8);
    minimap = x_canvas(mw * mm_s, mh * mm_s);
    x_target(minimap);
    x_color(x_rgb(mm_rgb[0][0], mm_rgb[0][1], mm_rgb[0][2]));
    x_fill_rect(0, 0, mw * mm_s, mh * mm_s);
    x_target(0);
    nnewly = 0;
    for (int i = 0; i < mw * mh; i++)   /* everything already seen, after a resize */
        if (seen[i])
            newly[nnewly++] = i;
}

/* Paint the newly seen tiles into the minimap canvas, a request per colour. */
static void update_minimap(void)
{
    static short r[4 * 512];

    if (nnewly == 0)
        return;
    x_target(minimap);
    for (int kind = 1; kind <= 3; kind++) {
        int n = 0;

        x_color(x_rgb(mm_rgb[kind][0], mm_rgb[kind][1], mm_rgb[kind][2]));
        for (int j = 0; j < nnewly; j++) {
            int i = newly[j];
            int k = map[i] == T_OPEN ? 1 : map[i] == T_WALL ? 2 : 3;

            if (k != kind)
                continue;
            r[4 * n] = (short)(i % mw * mm_s);
            r[4 * n + 1] = (short)(i / mw * mm_s);
            r[4 * n + 2] = r[4 * n + 3] = (short)mm_s;
            if (++n == 512) {
                x_fill_rects(r, n);
                n = 0;
            }
        }
        x_fill_rects(r, n);
    }
    x_target(0);
    nnewly = 0;
}

static void draw_minimap(void)
{
    int vw = clamp(mw * mm_s, 1, W / 3), vh = clamp(mh * mm_s, 1, H / 2);
    int ox = W - vw - 8, oy = 8;
    int ppx = (int)((long long)px * mm_s >> FP), ppy = (int)((long long)py * mm_s >> FP);
    int sx = clamp(ppx - vw / 2, 0, mw * mm_s - vw), sy = clamp(ppy - vh / 2, 0, mh * mm_s - vh);
    int d = mm_s * 3 / 2 + 2;

    update_minimap();
    if (!map_on || ox < W / 2)
        return;
    x_color(x_rgb(110, 110, 124));
    x_fill_rect(ox - 2, oy - 2, vw + 4, vh + 4);
    x_blit(minimap, sx, sy, vw, vh, ox, oy);
    x_color(x_rgb(255, 220, 40));
    x_line_width(mm_s > 4 ? 2 : 1);
    x_line(ox + ppx - sx, oy + ppy - sy, ox + ppx - sx + (icos(angle) * d >> FP),
           oy + ppy - sy + (isin(angle) * d >> FP));
    x_line_width(1);
    x_color(x_rgb(255, 40, 40));
    x_fill_rect(ox + ppx - sx - mm_s / 2 - 1, oy + ppy - sy - mm_s / 2 - 1, mm_s + 2, mm_s + 2);
}

static void draw_box(const char *const *lines, int n)
{
    int w = 0, lh = 16, bx, by, bw, bh;

    for (int i = 0; i < n; i++)
        if ((int)strlen(lines[i]) > w)
            w = (int)strlen(lines[i]);
    bw = w * 6 + 40;
    bh = n * lh + 24;
    bx = (W - bw) / 2;
    by = (H - bh) / 2;
    x_color(x_rgb(235, 235, 240));
    x_fill_rect(bx, by, bw, bh);
    x_color(x_rgb(40, 40, 50));
    x_rect(bx + 3, by + 3, bw - 7, bh - 7);
    for (int i = 0; i < n; i++)
        x_text(bx + (bw - (int)strlen(lines[i]) * 6) / 2, by + 12 + lh * i + 12, lines[i]);
}

static void draw_hud(long long now)
{
    char left[128], right[96];
    long long us = (state == S_WON ? t_end : now) - t_start;
    int secs = state == S_HINT ? 0 : (int)(us / 1000000);

    snprintf(left, sizeof left, "%sLEVEL %d   %dx%d   %d:%02d   explored %d%%",
             demo_on ? "DEMO   " : "", level, ncells, ncells, secs / 60, secs % 60,
             explored_pct());
    if (fps_on) {
        size_t n = strlen(left);

        snprintf(left + n, sizeof left - n, "   %d fps  %d KB/s", fps_value, kb_value);
    }
    snprintf(right, sizeof right, "%s", demo_on ? "any key: take over   q: quit"
                                                : "arrows/WASD   m map   r new   q quit");
    x_color(x_rgb(30, 30, 38));
    x_fill_rect(0, H - BAR_H, W, BAR_H);
    x_color(x_rgb(220, 220, 228));
    x_text(8, H - 5, left);
    /* the right end keeps clear of a resize grip in the corner */
    if ((int)(strlen(left) + strlen(right)) * 6 + 60 < W)
        x_text(W - 24 - (int)strlen(right) * 6, H - 5, right);

    if (state == S_HINT) {
        static const char *const hint[] = {
            "THE MAZE", "", "Find the green door.", "",
            "arrows / WASD   walk and turn (hold them)",
            "m minimap   t brick lines   r new maze   q quit", "",
            "press any key to start",
        };

        draw_box(hint, (int)(sizeof hint / sizeof hint[0]));
    } else if (state == S_WON) {
        char l1[64];
        const char *lines[4];
        int s = (int)((t_end - t_start) / 1000000);

        snprintf(l1, sizeof l1, "level %d in %d:%02d, %d%% explored", level, s / 60, s % 60,
                 explored_pct());
        lines[0] = "YOU ESCAPED!";
        lines[1] = l1;
        lines[2] = "";
        lines[3] = demo_on ? (level >= DEMO_LEVELS ? "starting over..." : "next maze coming up...")
                           : "space: a bigger maze   q: quit";
        draw_box(lines, 4);
    }
}

static void render(long long now)
{
    int rays = (W + COL_PX - 1) / COL_PX;

    dirx = icos(angle);
    diry = isin(angle);
    rightx = -diry;
    righty = dirx;
    cast(rays);
    draw_sky();
    draw_floor();
    draw_walls(rays);
    draw_minimap();
    draw_hud(now);
    x_present();
}

static void resized(void)
{
    W = clamp(x_width, 1, MAX_W);
    H = x_height > 1 ? x_height : 1;
    focal = W * 50 / PLANE_PCT;
    new_sky();
    new_minimap();
}

/* ---- input ---- */

static void set_held(int key, int down)
{
    switch (key) {
    case X_KEY_UP:    case 'w': case 'W': held_fwd = down; break;
    case X_KEY_DOWN:  case 's': case 'S': held_back = down; break;
    case X_KEY_LEFT:  case 'a': case 'A': held_left = down; break;
    case X_KEY_RIGHT: case 'd': case 'D': held_right = down; break;
    }
}

/* Returns 1 to quit. */
static int key_down(int key, long long now, int *dirty)
{
    if (key == 'q' || key == 'Q' || key == X_KEY_ESCAPE)
        return 1;
    *dirty = 1;
    if (demo_on) {                      /* any other key takes over */
        demo_on = 0;
        if (state == S_WON) {
            level++;
            state = S_PLAY;
            gen_maze();
            t_start = now;
        }
        return 0;
    }
    if (state == S_HINT) {
        state = S_PLAY;
        t_start = now;
        return 0;
    }
    if (state == S_WON) {
        if (key == ' ' || key == X_KEY_RETURN || key == 'n' || key == 'N') {
            level++;
            state = S_PLAY;
            gen_maze();
            t_start = now;
        }
        return 0;
    }
    switch (key) {
    case 'm': case 'M': map_on = !map_on; break;
    case 't': case 'T': tex_on = !tex_on; break;
    case 'f': case 'F': fps_on = !fps_on; break;
    case 'r': case 'R':
        gen_maze();
        t_start = now;
        break;
    default:
        set_held(key, 1);
    }
    return 0;
}

/* One game step; returns 1 if the view changed. */
static int step(long long now)
{
    int turn, move;

    if (demo_on)
        return autopilot(now);
    if (state != S_PLAY)
        return 0;
    turn = (held_right - held_left) * TURN_SPEED;
    move = (held_fwd - held_back) * WALK_SPEED;
    if (turn != 0)
        angle = (angle + turn) & (ANG_FULL - 1);
    if (move != 0)
        walk((int)((long long)icos(angle) * move >> FP), (int)((long long)isin(angle) * move >> FP));
    return turn != 0 || move != 0;
}

static int bench(int frames)
{
    unsigned long long b0 = x_sent;
    long long t0 = now_us(), t1;

    state = S_PLAY;
    for (int i = 0; i < frames; i++) {
        angle = (int)((long long)ANG_FULL * i / frames);
        render(t0);
        x_sync();
    }
    t1 = now_us();
    printf("%dx%d, %d frames: %.1f frames/s, %llu bytes/frame\n", W, H, frames,
           frames * 1e6 / (double)(t1 - t0), (x_sent - b0) / (unsigned long long)frames);
    return 0;
}

int main(int argc, char **argv)
{
    const char *display = NULL;
    long long next_tick, fps_t0;
    unsigned long long fps_b0;
    int bench_frames = 0, dirty = 1, frames = 0, last_sec = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-demo") == 0 || strcmp(argv[i], "-d") == 0)
            demo_on = 1;
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
            level = clamp(atoi(argv[++i]), 1, 100);
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
            bench_frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "-display") == 0 && i + 1 < argc)
            display = argv[++i];
        else {
            fprintf(stderr, "usage: xmaze [-demo] [-l LEVEL] [-b FRAMES] [-display HOST:0]\n");
            return 2;
        }
    }
    if (x_open(display) < 0)
        return 1;
    make_tables();
    x_window("maze", 640, 400);
    resized();
    rng_state = bench_frames ? 12345u : ((unsigned int)now_us() | 1u);
    gen_maze();
    if (bench_frames > 0)
        return bench(bench_frames);

    next_tick = fps_t0 = now_us();
    fps_b0 = x_sent;
    for (;;) {
        struct x_event e;
        long long now = now_us();
        int ticks = 0, sec;

        while (x_event(&e)) {
            if (e.type == XE_CLOSE)
                return 0;
            if (e.type == XE_RESIZE) {
                resized();
                dirty = 1;
            } else if (e.type == XE_KEY_DOWN) {
                if (key_down(e.key, now, &dirty)) {
                    x_close();
                    return 0;
                }
            } else if (e.type == XE_KEY_UP) {
                set_held(e.key, 0);
            }
        }
        while (now >= next_tick && ticks++ < 5) {
            dirty |= step(now);
            next_tick += TICK_US;
        }
        if (now - next_tick > 200000)
            next_tick = now;            /* fell far behind: don't try to catch up */
        sec = (int)((now - t_start) / 1000000);
        if (sec != last_sec) {          /* the clock in the status bar */
            last_sec = sec;
            dirty = 1;
        }
        if (now - fps_t0 >= 1000000) {
            fps_value = (int)(frames * 1000000LL / (now - fps_t0));
            kb_value = (int)((x_sent - fps_b0) * 1000000ULL / (unsigned long long)(now - fps_t0) / 1024);
            frames = 0;
            fps_t0 = now;
            fps_b0 = x_sent;
        }
        if (dirty) {
            render(now);
            x_sync();                   /* never more than a frame in flight */
            frames++;
            dirty = 0;
        } else {
            long long wait = (next_tick - now_us()) / 1000;

            x_wait(wait < 1 ? 1 : (int)wait);
        }
    }
}
