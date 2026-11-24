// SPDX-License-Identifier: MIT
/*
 * xpulse.c - pulse, the system monitor, in an X11 window.
 *
 * Build:  ./build.sh xpulse     (xpulse.c and pulse_data.c, the sampler
 *                                the terminal pulse uses too)
 * Run:    DISPLAY=HOST:0 ./xpulse
 * Mouse:  click a column heading to sort by it, again to reverse; click a
 *         process to select it, right-click it for a menu of signals to
 *         send; the "every 1 s" button sets the update interval; the wheel
 *         scrolls
 * Keys:   up/down or k/j = select process   PgUp/PgDn = page
 *         s = next column to sort by        r = reverse sort
 *         + / - = update interval           q or Esc = quit
 *
 * Nothing is drawn twice. The panels, titles and grids are drawn once (and
 * again only when the window is resized); after that each update sends
 * only what changed: a number whose text changed, a meter whose length
 * changed, the process rows that differ. Each graph lives on the X server
 * as a pixmap: an update moves it one step left with a single copy and
 * draws the newest sample at the right, so a graph costs about 150 bytes
 * per update whatever its size. While the window is being resized it
 * keeps its picture; it is laid out again once, when the size settles,
 * each graph drawn whole as one filled polygon and one line.
 */
#include "x11.h"
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include "pulse.h"

#define STEP 3                  /* pixels per sample in the graphs */
#define MAXCOLS 512             /* samples across a graph at most (HIST) */
#define SETTLE_MS 150           /* a resize is done when it stops this long */
#define M 12                    /* window margin */
#define G 10                    /* gap between panels */
#define P 12                    /* padding inside a panel */
#define RADIUS 7                /* panel corners */
#define CPU_ROW 30              /* a row of CPU meters: name and value, meter */

static const int intervals_ms[] = { 250, 500, 1000, 2000, 5000 };
static int interval_idx = 2;

/* ---------- colours and fonts ---------- */

typedef struct { int r, g, b; } rgb;

static const rgb RGB_BG = { 13, 17, 23 }, RGB_PANEL = { 22, 27, 34 };
static const rgb ACC_CPU = { 255, 146, 87 }, ACC_MEM = { 188, 140, 255 };
static const rgb ACC_RX = { 63, 185, 80 }, ACC_TX = { 88, 166, 255 };
static const rgb ACC_DISK = { 227, 179, 65 }, ACC_HOST = { 63, 185, 80 };

static unsigned c_bg, c_panel, c_grid, c_track, c_text, c_dim, c_faint, c_sel;
static int f_big, f_head, f_bold, f_text, f_mono;

static unsigned px(rgb c) { return x_rgb(c.r, c.g, c.b); }

/* a + (b - a) * t / 1000 */
static rgb mix(rgb a, rgb b, int t)
{
    rgb c = { a.r + (b.r - a.r) * t / 1000, a.g + (b.g - a.g) * t / 1000,
              a.b + (b.b - a.b) * t / 1000 };
    return c;
}

static void setup_style(void)
{
    rgb text = { 230, 237, 243 }, dim = { 139, 148, 158 }, faint = { 72, 79, 88 };

    c_bg = px(RGB_BG);
    c_panel = px(RGB_PANEL);
    c_grid = px(mix(RGB_PANEL, text, 60));
    c_track = px(mix(RGB_PANEL, text, 90));
    c_text = px(text);
    c_dim = px(dim);
    c_faint = px(faint);
    c_sel = px(mix(RGB_PANEL, ACC_TX, 220));

    /* Bitmap fonts that XQuartz and most X servers have; "fixed" if not. */
    f_big = x_font("-adobe-helvetica-bold-r-normal--25-*-*-*-p-*-iso8859-1");
    if (!f_big)
        f_big = x_font("-*-helvetica-bold-r-normal--24-*-*-*-p-*-iso8859-1");
    f_head = x_font("-adobe-helvetica-bold-r-normal--17-*-*-*-p-*-iso8859-1");
    f_bold = x_font("-adobe-helvetica-bold-r-normal--12-*-*-*-p-*-iso8859-1");
    f_text = x_font("-adobe-helvetica-medium-r-normal--12-*-*-*-p-*-iso8859-1");
    f_mono = x_font("-b&h-lucidatypewriter-medium-r-normal-sans-12-*-*-*-m-*-iso8859-1");
}

static int font_h(int f) { return x_font_ascent(f) + x_font_descent(f); }

/* ---------- shapes ---------- */

static void round_rect(int x, int y, int w, int h, int r)
{
    if (w < 2 * r || h < 2 * r) {
        x_fill_rect(x, y, w, h);
        return;
    }
    x_fill_rect(x + r, y, w - 2 * r, h);
    x_fill_rect(x, y + r, w, h - 2 * r);
    x_fill_ellipse(x, y, 2 * r, 2 * r);
    x_fill_ellipse(x + w - 2 * r, y, 2 * r, 2 * r);
    x_fill_ellipse(x, y + h - 2 * r, 2 * r, 2 * r);
    x_fill_ellipse(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r);
}

/* ---------- text and meters that redraw only when they change ---------- */

#define SLOTS 96
static struct { char s[96]; unsigned color; int x, w; } slot[SLOTS];
static int meter_px[SLOTS];

static void forget_all(void)
{
    memset(slot, 0, sizeof slot);
    for (int i = 0; i < SLOTS; i++)
        meter_px[i] = -1;
}

/* Text in [x, x + w) with its top at y: align 0 left, 1 right, 2 centre. */
static void label(int id, int x, int y, int w, int align, int font, unsigned color,
                  unsigned bg, const char *fmt, ...)
{
    char s[96];
    va_list ap;
    int tw, tx;

    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    if (strcmp(s, slot[id].s) == 0 && slot[id].color == color && slot[id].x == x &&
        slot[id].w == w)
        return;
    strcpy(slot[id].s, s);
    slot[id].color = color;
    slot[id].x = x;
    slot[id].w = w;
    x_color(bg);
    x_fill_rect(x, y, w, font_h(font));
    tw = x_text_width(font, s);
    tx = align == 1 ? x + w - tw : align == 2 ? x + (w - tw) / 2 : x;
    x_text_font(font);
    x_color(color);
    x_text(tx, y + x_font_ascent(font), s);
}

/* A meter: a track, filled pm/1000 of the way, with rounded ends. */
static void meter(int id, int x, int y, int w, int h, int pm, unsigned color)
{
    int fill = pm <= 0 ? 0 : pm >= 1000 ? w : (int)((long)w * pm / 1000);

    if (fill > 0 && fill < h)
        fill = h;                       /* a dot, so something shows */
    if (fill == meter_px[id])
        return;
    meter_px[id] = fill;
    x_color(c_panel);
    x_fill_rect(x, y, w, h);
    x_color(c_track);
    round_rect(x, y, w, h, h / 2);
    if (fill > 0) {
        x_color(color);
        round_rect(x, y, fill, h, h / 2);
    }
}

/* ---------- graphs ---------- */

typedef struct {
    int x, y, w, h;             /* the plot, in the window */
    int n;                      /* samples across */
    unsigned pix;               /* the plot, kept on the server */
    unsigned empty;             /* one STEP-wide column: background and grid */
    unsigned grad, grad2;       /* gradient columns, for each series */
    unsigned hi, hi2;           /* line colours */
    const unsigned *a, *b;      /* series; b, if set, mirrored below the middle */
    unsigned long scale;        /* full scale; 0: permille */
    int ya, yb;                 /* where the lines ended, for the next sample */
} graph;

static graph g_cpu, g_mem, g_net, g_disk;

/* A STEP x h column fading from c (top) into the panel. */
static unsigned make_gradient(rgb c, int h, int flip)
{
    unsigned *img = malloc((size_t)h * STEP * sizeof *img), pix;

    if (img == NULL)
        return 0;
    for (int y = 0; y < h; y++) {
        int t = h > 1 ? (flip ? h - 1 - y : y) * 1000 / (h - 1) : 0;
        unsigned p = px(mix(mix(c, RGB_PANEL, 350), RGB_PANEL, t * 6 / 10));

        for (int x = 0; x < STEP; x++)
            img[y * STEP + x] = p;
    }
    pix = x_canvas(STEP, h);
    x_target(pix);
    x_image(0, 0, STEP, h, img, STEP);
    x_target(0);
    free(img);
    return pix;
}

static void graph_free(graph *g)
{
    x_free_canvas(g->pix);
    x_free_canvas(g->empty);
    x_free_canvas(g->grad);
    x_free_canvas(g->grad2);
    g->pix = g->empty = g->grad = g->grad2 = 0;
}

static void graph_setup(graph *g, int x, int y, int w, int h, rgb ca, const unsigned *a,
                        rgb cb, const unsigned *b, unsigned long scale)
{
    graph_free(g);
    g->x = x;
    g->y = y;
    g->w = w = w < STEP ? STEP : w;
    g->h = h = h < 8 ? 8 : h;
    g->n = w / STEP > MAXCOLS ? MAXCOLS : w / STEP;
    g->a = a;
    g->b = b;
    g->scale = scale;
    g->hi = px(ca);
    g->hi2 = px(cb);
    g->grad = make_gradient(ca, b ? h / 2 : h, 0);
    g->grad2 = b ? make_gradient(cb, h - h / 2, 1) : 0;
    g->empty = x_canvas(STEP, h);
    x_target(g->empty);
    x_color(c_panel);
    x_fill_rect(0, 0, STEP, h);
    x_color(c_grid);
    for (int i = 1; i < 4; i++)
        x_fill_rect(0, h * i / 4, STEP, 1);
    g->pix = x_canvas(g->n * STEP, h);
    x_target(0);
}

static unsigned long graph_value(const unsigned *s, int age)
{
    return age < h_count ? s[HIST - 1 - age] : 0;
}

/* Bar height in pixels for v, out of room. */
static int graph_px(const graph *g, unsigned long v, int room)
{
    unsigned long full = g->scale ? g->scale : 1000;
    unsigned long long p = (unsigned long long)v * (unsigned long long)room / full;

    return p > (unsigned long long)room ? room : (int)p;
}

/* Draw the sample of the given age in column col of the plot pixmap. */
static void graph_column(graph *g, int col, int age, int first)
{
    int x = col * STEP, half = g->b ? g->h / 2 : g->h;
    int ha = graph_px(g, graph_value(g->a, age), g->b ? half - 1 : g->h - 1);
    int ya = half - 1 - ha;

    x_target(g->pix);
    x_blit(g->empty, 0, 0, STEP, g->h, x, 0);
    if (age >= h_count) {               /* before the first sample: nothing */
        g->ya = g->yb = -1;
        x_target(0);
        return;
    }
    first = first || g->ya < 0;
    if (ha > 0)                         /* the gradient is fixed to the plot */
        x_blit(g->grad, 0, half - ha, STEP, ha, x, half - ha);
    x_color(g->hi);
    if (first)
        x_fill_rect(x, ya, STEP, 2);
    else
        x_line(x - 1, g->ya, x + STEP - 1, ya);
    g->ya = ya;
    if (g->b) {
        int hb = graph_px(g, graph_value(g->b, age), g->h - half - 1);
        int yb = half + hb;

        if (hb > 0)
            x_blit(g->grad2, 0, 0, STEP, hb, x, half);
        x_color(g->hi2);
        if (first)
            x_fill_rect(x, yb, STEP, 2);
        else
            x_line(x - 1, g->yb, x + STEP - 1, yb);
        g->yb = yb;
        x_color(c_faint);
        x_fill_rect(x, half, STEP, 1);
    }
    x_target(0);
}

static void graph_show(const graph *g)
{
    x_blit(g->pix, 0, 0, g->n * STEP, g->h, g->x, g->y);
}

/* One series for graph_redraw: its area as a polygon filled with the
 * gradient, then its line. down: the mirrored series, below the middle. */
static void graph_series(graph *g, const unsigned *s, int down, int *last_y)
{
    static short area[2 * (2 * MAXCOLS + 2)], line[2 * (MAXCOLS + 1)];
    int half = g->b ? g->h / 2 : g->h, room = down ? g->h - half - 1 : g->b ? half - 1 : g->h - 1;
    int base = half, na = 0, nl = 0, first = -1, y = 0;

    for (int col = 0; col < g->n; col++) {
        int age = g->n - 1 - col, x = col * STEP, hgt;

        if (age >= h_count)
            continue;
        hgt = graph_px(g, graph_value(s, age), room);
        y = down ? half + hgt : half - 1 - hgt;
        if (first < 0) {
            first = x;
            area[na++] = (short)x;
            area[na++] = (short)base;
            line[nl++] = (short)x;
            line[nl++] = (short)y;
        }
        area[na++] = (short)x;
        area[na++] = (short)(down ? half + hgt : half - hgt);
        area[na++] = (short)(x + STEP);
        area[na++] = (short)(down ? half + hgt : half - hgt);
        line[nl++] = (short)(x + STEP - 1);
        line[nl++] = (short)y;
    }
    *last_y = first < 0 ? -1 : y;
    if (first < 0)
        return;
    area[na++] = (short)(g->n * STEP);
    area[na++] = (short)base;
    x_fill_poly_tiled(down ? g->grad2 : g->grad, 0, down ? half : 0, area, na / 2);
    x_color(down ? g->hi2 : g->hi);
    x_polyline(line, nl / 2);
}

/* Everything again, oldest on the left: a dozen requests, whatever the size. */
static void graph_redraw(graph *g)
{
    if (!g->pix)
        return;
    x_target(g->pix);
    x_color(c_panel);
    x_fill_rect(0, 0, g->n * STEP, g->h);
    x_color(c_grid);
    for (int i = 1; i < 4; i++)
        x_fill_rect(0, g->h * i / 4, g->n * STEP, 1);
    graph_series(g, g->a, 0, &g->ya);
    if (g->b) {
        graph_series(g, g->b, 1, &g->yb);
        x_color(c_faint);
        x_fill_rect(0, g->h / 2, g->n * STEP, 1);
    }
    x_target(0);
    graph_show(g);
}

/* One more sample: move the plot left a step, draw the new one. */
static void graph_push(graph *g)
{
    if (!g->pix)
        return;
    x_target(g->pix);
    x_blit(g->pix, STEP, 0, (g->n - 1) * STEP, g->h, 0, 0);
    x_target(0);
    graph_column(g, g->n - 1, 0, 0);    /* its line starts where the last one ended */
    graph_show(g);
}

/* For rates: the full scale, a 1-2-5 step above the biggest value shown.
 * It goes up at once, and down only when the values fall below a quarter
 * of it, so the plot (which then has to be drawn again) rarely changes. */
static int graph_rescale(graph *g)
{
    unsigned long most = 0, want = 1024;  /* 1K, 2K, 5K, 10K, 20K, 50K, ... */
    int k = 0;

    for (int age = 0; age < g->n; age++) {
        unsigned long v = graph_value(g->a, age);

        if (v > most)
            most = v;
        if (g->b && (v = graph_value(g->b, age)) > most)
            most = v;
    }
    while (want < most && want < 0x40000000ul) {
        want = k == 1 ? want * 5 / 2 : want * 2;
        k = (k + 1) % 3;
    }
    if (want > g->scale || want * 4 <= g->scale) {
        g->scale = want;
        return 1;
    }
    return 0;
}

/* ---------- layout ---------- */

typedef struct { int x, y, w, h; } box;
static box b_cpu, b_mem, b_net, b_disk, b_procs, b_rate;
static int layout_gen;          /* counts layouts: everything was drawn again */
static void layout_table(void);
static int top_plot;            /* y of a plot below a panel's title row */
static int row_h, rows_visible, scroll;

static void panel(box b, const char *title, rgb accent, int id)
{
    x_color(c_panel);
    round_rect(b.x, b.y, b.w, b.h, RADIUS);
    x_color(px(accent));
    round_rect(b.x + P, b.y + P + 2, 4, x_font_ascent(f_bold) - 2, 2);
    slot[id].s[0] = 0;
    label(id, b.x + P + 10, b.y + P, x_text_width(f_bold, title) + 2, 0, f_bold, c_text,
          c_panel, "%s", title);
}

/* Lay out the window and draw everything that doesn't change. */
static void layout(void)
{
    int W = x_width, H = x_height;
    int head = font_h(f_head) + 8;
    int y0 = M + head + G, rest = H - y0 - M - 2 * G;
    int half = (W - 2 * M - G) / 2;
    int r1 = rest * 32 / 100, r2 = rest * 26 / 100, foot;

    if (rest < 120)
        rest = 120;
    b_cpu = (box){ M, y0, half, r1 };
    b_mem = (box){ M + half + G, y0, W - 2 * M - G - half, r1 };
    b_net = (box){ M, y0 + r1 + G, half, r2 };
    b_disk = (box){ M + half + G, y0 + r1 + G, W - 2 * M - G - half, r2 };
    b_procs = (box){ M, y0 + r1 + r2 + 2 * G, W - 2 * M, H - M - (y0 + r1 + r2 + 2 * G) };

    forget_all();
    layout_gen++;
    x_color(c_bg);
    x_fill_rect(0, 0, W, H);
    x_line_width(2);

    top_plot = P + font_h(f_big) + 6;
    panel(b_cpu, "CPU", ACC_CPU, 0);
    panel(b_mem, "MEMORY", ACC_MEM, 1);
    panel(b_net, "NETWORK", ACC_RX, 2);
    panel(b_disk, "DISK", ACC_DISK, 3);
    panel(b_procs, "PROCESSES", ACC_TX, 4);

    foot = (ncore > 1 ? 2 : 1) * CPU_ROW + 4;
    graph_setup(&g_cpu, b_cpu.x + P, b_cpu.y + top_plot, b_cpu.w - 2 * P,
                b_cpu.h - top_plot - P - foot, ACC_CPU, h_cpu, ACC_CPU, NULL, 0);
    foot = 36;
    graph_setup(&g_mem, b_mem.x + P, b_mem.y + top_plot, b_mem.w - 2 * P,
                b_mem.h - top_plot - P - foot, ACC_MEM, h_mem, ACC_MEM, NULL, 0);
    foot = 22;
    graph_setup(&g_net, b_net.x + P, b_net.y + top_plot, b_net.w - 2 * P,
                b_net.h - top_plot - P - foot, ACC_RX, h_rx, ACC_TX, h_tx, 1024);
    graph_setup(&g_disk, b_disk.x + P, b_disk.y + top_plot, b_disk.w - 2 * P,
                b_disk.h - top_plot - P - foot, ACC_DISK, h_disk, ACC_DISK, NULL, 1024);
    graph_rescale(&g_net);
    graph_rescale(&g_disk);
    graph_redraw(&g_cpu);
    graph_redraw(&g_mem);
    graph_redraw(&g_net);
    graph_redraw(&g_disk);

    row_h = font_h(f_mono) + 5;
    rows_visible = (b_procs.h - P - font_h(f_bold) - 10 - row_h - P) / row_h;
    if (rows_visible < 1)
        rows_visible = 1;
    layout_table();
}

/* ---------- panels' contents ---------- */

static void fmt_rate(char *out, size_t n, unsigned long b)
{
    fmt_bytes(out, n, b);
    strncat(out, "/s", n - strlen(out) - 1);
}

static void draw_header(void)
{
    char up[32], ld[64], rate[24];
    unsigned long d = uptime_s / 86400, h = uptime_s / 3600 % 24, m = uptime_s / 60 % 60;
    int y = M + 4, hostw, kernw, free_x;
    static char rate_key[48];

    x_color(px(ACC_HOST));
    x_fill_ellipse(M, y + (font_h(f_head) - 10) / 2, 10, 10);
    hostw = x_text_width(f_head, uts.nodename) + 4;
    label(10, M + 18, y, hostw, 0, f_head, c_text, c_bg, "%s", uts.nodename);
    snprintf(ld, sizeof ld, "%s %s  %s", uts.sysname, uts.release, uts.machine);
    kernw = x_text_width(f_text, ld) + 2;
    label(11, M + 18 + hostw + 10, y + x_font_ascent(f_head) - x_font_ascent(f_text), kernw, 0,
          f_text, c_dim, c_bg, "%s", ld);
    free_x = M + 18 + hostw + 10 + kernw + 20;
    if (d > 0)
        snprintf(up, sizeof up, "%lud %luh %02lum", d, h, m);
    else
        snprintf(up, sizeof up, "%luh %02lum", h, m);
    snprintf(ld, sizeof ld, "up %s    load %s %s %s    %d/%d tasks", up, load1, load5, load15,
             tasks_running, tasks_total);

    /* the update interval, a button that opens a menu of them */
    snprintf(rate, sizeof rate, "every %g s", intervals_ms[interval_idx] / 1000.0);
    b_rate.w = x_text_width(f_text, rate) + 32;
    b_rate.h = font_h(f_text) + 8;
    b_rate.x = x_width - M - b_rate.w;
    b_rate.y = y + (font_h(f_head) - b_rate.h) / 2;
    {
        char key[48];

        snprintf(key, sizeof key, "%s|%d|%d", rate, b_rate.x, layout_gen);
        if (strcmp(key, rate_key) != 0) {
            int ty = b_rate.y + (b_rate.h - font_h(f_text)) / 2 + x_font_ascent(f_text);
            int ax = b_rate.x + b_rate.w - 18, ay = b_rate.y + b_rate.h / 2 - 2;
            short tri[6] = { (short)ax, (short)ay, (short)(ax + 8), (short)ay,
                             (short)(ax + 4), (short)(ay + 4) };

            strcpy(rate_key, key);
            x_color(c_bg);
            x_fill_rect(b_rate.x - 40, b_rate.y, b_rate.w + 40, b_rate.h);
            x_color(c_track);
            round_rect(b_rate.x, b_rate.y, b_rate.w, b_rate.h, 5);
            x_color(c_panel);
            round_rect(b_rate.x + 1, b_rate.y + 1, b_rate.w - 2, b_rate.h - 2, 4);
            x_text_font(f_text);
            x_color(c_text);
            x_text(b_rate.x + 10, ty, rate);
            x_color(c_dim);
            x_fill_poly(tri, 3);
        }
    }
    label(12, free_x, y + x_font_ascent(f_head) - x_font_ascent(f_text), b_rate.x - 14 - free_x,
          1, f_text, c_dim, c_bg, "%s", ld);
}

/* "37%" in the big font at a panel's top right. */
static void big_value(int id, box b, unsigned color, const char *fmt, ...)
{
    char s[32];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    label(id, b.x + b.w - P - 120, b.y + P - 4, 120, 1, f_big, color, c_panel, "%s", s);
}

static void draw_cpu(void)
{
    box b = b_cpu;
    int y = g_cpu.y + g_cpu.h + 8, cellw = (b.w - 2 * P + 16) / 4;
    static const char *names[4] = { "user", "system", "irq", "io wait" };
    int vals[4] = { usr_pm, sys_pm, irq_pm, io_pm };

    big_value(20, b, px(ACC_CPU), "%d%%", (cpu_pm + 5) / 10);
    label(21, b.x + P + 70, b.y + P, b.w - 2 * P - 200, 0, f_text, c_dim, c_panel,
          "%d core%s", ncpu, ncpu == 1 ? "" : "s");
    /* a name and value over a meter, four to a row */
    for (int i = 0; i < 4; i++) {
        int x = b.x + P + i * cellw;

        label(22 + i, x, y, 60, 0, f_text, c_dim, c_panel, "%s", names[i]);
        label(26 + i, x + 60, y, cellw - 76, 1, f_bold, c_text, c_panel, "%d%%",
              (vals[i] + 5) / 10);
        meter(22 + i, x, y + 17, cellw - 16, 5, vals[i], px(ACC_CPU));
    }
    if (ncore > 1) {
        int per = ncore < 4 ? ncore : 4, cw = (b.w - 2 * P + 16) / per;

        y += CPU_ROW;
        for (int i = 0; i < ncore && i < per; i++) {
            int x = b.x + P + i * cw;

            label(30 + i, x, y, 60, 0, f_text, c_dim, c_panel, "core %d", i);
            label(34 + i, x + 60, y, cw - 76, 1, f_bold, c_text, c_panel, "%d%%",
                  (core_pm[i] + 5) / 10);
            meter(30 + i, x, y + 17, cw - 16, 5, core_pm[i], px(mix(ACC_CPU, RGB_PANEL, 250)));
        }
    }
}

static void draw_mem(void)
{
    box b = b_mem;
    char used[16], total[16], buf[16], cache[16], avail[16];
    int y = g_mem.y + g_mem.h + 10, w = b.w - 2 * P, x = b.x + P;
    unsigned long total_kb = mem_total ? mem_total : 1;
    int pu = (int)(mem_used * 1000 / total_kb), pb = (int)(mem_buffers * 1000 / total_kb);
    int pc = (int)(mem_cached * 1000 / total_kb);
    static int last_key = -1;
    int key;

    fmt_bytes(used, sizeof used, (unsigned long long)mem_used * 1024);
    fmt_bytes(total, sizeof total, (unsigned long long)mem_total * 1024);
    fmt_bytes(buf, sizeof buf, (unsigned long long)mem_buffers * 1024);
    fmt_bytes(cache, sizeof cache, (unsigned long long)mem_cached * 1024);
    fmt_bytes(avail, sizeof avail, (unsigned long long)mem_avail * 1024);
    big_value(40, b, px(ACC_MEM), "%d%%", (mem_used_pm + 5) / 10);
    label(41, b.x + P + 90, b.y + P, b.w - 2 * P - 220, 0, f_text, c_dim, c_panel,
          "%s of %s", used, total);

    /* used, buffers and cache side by side in one bar, the rest empty */
    key = (int)((long)w * pu / 1000) * 1000003 + (int)((long)w * pb / 1000) * 1009 +
          (int)((long)w * pc / 1000);
    if (key != last_key) {
        int xu = (int)((long)w * pu / 1000), xb = (int)((long)w * pb / 1000);
        int xc = (int)((long)w * pc / 1000);

        last_key = key;
        x_color(c_panel);
        x_fill_rect(x, y, w, 8);
        x_color(c_track);
        round_rect(x, y, w, 8, 4);
        x_color(px(mix(ACC_MEM, RGB_PANEL, 650)));
        if (xu + xb + xc > 0)
            round_rect(x, y, xu + xb + xc < 8 ? 8 : xu + xb + xc, 8, 4);
        x_color(px(mix(ACC_MEM, RGB_PANEL, 400)));
        if (xu + xb > 0)
            round_rect(x, y, xu + xb < 8 ? 8 : xu + xb, 8, 4);
        x_color(px(ACC_MEM));
        if (xu > 0)
            round_rect(x, y, xu < 8 ? 8 : xu, 8, 4);
    }
    y += 16;
    label(42, x, y, w, 0, f_text, c_dim, c_panel, "used %s    buffers %s    cache %s    available %s",
          used, buf, cache, avail);
}

static void draw_net(void)
{
    box b = b_net;
    char rx[16], tx[16], rt[16], tt[16], sc[16];

    if (graph_rescale(&g_net))
        graph_redraw(&g_net);
    fmt_rate(rx, sizeof rx, rx_rate);
    fmt_rate(tx, sizeof tx, tx_rate);
    fmt_bytes(rt, sizeof rt, rx_cur);
    fmt_bytes(tt, sizeof tt, tx_cur);
    fmt_rate(sc, sizeof sc, g_net.scale);
    label(50, b.x + P + 80, b.y + P, 120, 0, f_text, c_dim, c_panel, "%s", net_name);
    label(51, b.x + b.w - P - 200, b.y + P, 95, 1, f_bold, px(ACC_RX), c_panel, "in %s", rx);
    label(52, b.x + b.w - P - 100, b.y + P, 100, 1, f_bold, px(ACC_TX), c_panel, "out %s", tx);
    label(53, b.x + P, g_net.y + g_net.h + 6, b.w - 2 * P, 0, f_text, c_dim, c_panel,
          "received %s    sent %s    scale %s", rt, tt, sc);
}

static void draw_disk(void)
{
    box b = b_disk;
    char rd[16], wr[16], used[16], total[16], sc[16];
    int y = g_disk.y + g_disk.h + 6;

    if (graph_rescale(&g_disk))
        graph_redraw(&g_disk);
    fmt_rate(rd, sizeof rd, rd_rate);
    fmt_rate(wr, sizeof wr, wr_rate);
    fmt_bytes(used, sizeof used, fs_used);
    fmt_bytes(total, sizeof total, fs_total);
    fmt_rate(sc, sizeof sc, g_disk.scale);
    label(60, b.x + P + 60, b.y + P, 140, 0, f_text, c_dim, c_panel, "%s  busy %d%%",
          disk_name[0] ? disk_name : "-", (disk_busy_pm + 5) / 10);
    label(61, b.x + b.w - P - 200, b.y + P, 200, 1, f_bold, px(ACC_DISK), c_panel,
          "read %s  write %s", rd, wr);
    label(62, b.x + P, y, 150, 0, f_text, c_dim, c_panel, "/  %s of %s", used, total);
    meter(62, b.x + P + 150, y + 4, b.w - 2 * P - 150 - 110, 6,
          fs_total ? (int)(fs_used * 1000 / fs_total) : 0, px(ACC_DISK));
    label(63, b.x + b.w - P - 100, y, 100, 1, f_text, c_dim, c_panel, "scale %s", sc);
}

/* ---------- processes ---------- */

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct { const char *title; int chars, right, key; } column;
static const column cols[] = {
    { "PID", 5, 1, SORT_PID },   { "PPID", 5, 1, SORT_PPID }, { "NAME", 16, 0, SORT_NAME },
    { "S", 1, 0, SORT_STATE },   { "NI", 3, 1, SORT_NICE },   { "THR", 3, 1, SORT_THR },
    { "VIRT", 6, 1, SORT_VIRT }, { "RES", 6, 1, SORT_MEM },   { "MEM%", 5, 1, SORT_MEM },
    { "TIME", 8, 1, SORT_TIME }, { "CPU%", 5, 1, SORT_CPU },
};
#define NCOLS (int)(sizeof cols / sizeof cols[0])
#define GAP 2                   /* characters between columns */
#define MAXROWS 64

static int col_at[NCOLS + 1];   /* where each column starts, in characters; then the end */
static int sort_col = NCOLS - 1;        /* the column sorted by: CPU% */
static int tbl_x, head_y, rows_y, char_w;
static char row_key[MAXROWS][160], head_key[32];
static long clk_tck = 100;
static char status[96];         /* what the last signal did, for a few seconds */
static long long status_until;
static int status_bad;

static void forget_rows(void)
{
    memset(row_key, 0, sizeof row_key);
    head_key[0] = 0;
}

/* Where things are in the process table; called from layout. */
static void layout_table(void)
{
    int at = 0;

    for (int c = 0; c < NCOLS; c++) {
        col_at[c] = at;
        at += cols[c].chars + GAP;
    }
    col_at[NCOLS] = at - GAP;
    char_w = x_text_width(f_mono, "0");
    tbl_x = b_procs.x + P;
    head_y = b_procs.y + P + font_h(f_bold) + 10;
    rows_y = head_y + row_h + 2;
}

/* Does the column sort largest first, before r reverses it? */
static int sorts_down(int key)
{
    return key == SORT_CPU || key == SORT_MEM || key == SORT_THR || key == SORT_VIRT ||
           key == SORT_TIME;
}

static void cell(char *row, int c, const char *s)
{
    int n = (int)strlen(s), w = cols[c].chars;

    if (n > w)
        n = w;
    memcpy(row + col_at[c] + (cols[c].right ? w - n : 0), s, (size_t)n);
}

static void fmt_time(char *out, size_t n, unsigned long ticks)
{
    unsigned long long cs = (unsigned long long)ticks * 100 / (unsigned long long)clk_tck;
    unsigned long s = (unsigned long)(cs / 100);

    if (s >= 100UL * 3600)
        snprintf(out, n, "%lud%02luh", s / 86400, s / 3600 % 24);
    else if (s >= 3600)
        snprintf(out, n, "%lu:%02lu:%02lu", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, n, "%lu:%02lu.%02u", s / 60, s % 60, (unsigned)(cs % 100));
}

static void proc_row(char *row, const Proc *p)
{
    char v[32];
    unsigned long mem_pm = mem_total ? p->rss_kb * 1000 / mem_total : 0;

    memset(row, ' ', (size_t)col_at[NCOLS]);
    row[col_at[NCOLS]] = 0;
    snprintf(v, sizeof v, "%d", p->pid);
    cell(row, 0, v);
    snprintf(v, sizeof v, "%d", p->ppid);
    cell(row, 1, v);
    cell(row, 2, p->name);
    v[0] = p->state;
    v[1] = 0;
    cell(row, 3, v);
    snprintf(v, sizeof v, "%d", p->nice);
    cell(row, 4, v);
    snprintf(v, sizeof v, "%d", p->threads);
    cell(row, 5, v);
    fmt_bytes(v, sizeof v, (unsigned long long)p->vsize_kb * 1024);
    cell(row, 6, v);
    fmt_bytes(v, sizeof v, (unsigned long long)p->rss_kb * 1024);
    cell(row, 7, v);
    snprintf(v, sizeof v, "%lu.%lu", mem_pm / 10, mem_pm % 10);
    cell(row, 8, v);
    fmt_time(v, sizeof v, p->ticks);
    cell(row, 9, v);
    snprintf(v, sizeof v, "%d.%d", p->cpu_pm / 10, p->cpu_pm % 10);
    cell(row, 10, v);
}

static void move_sel(int d)
{
    sel += d;
    if (sel >= nprocs)
        sel = nprocs - 1;
    if (sel < 0)
        sel = 0;
    sel_pid = nprocs > 0 ? procs[sel].pid : -1;
}

/* Sort by column c; again by the same one, the other way. */
static void sort_by(int c)
{
    if (c == sort_col) {
        sort_rev = !sort_rev;
    } else {
        sort_col = c;
        sort_key = cols[c].key;
        sort_rev = 0;
    }
    sort_procs();
}

/* The column headings; the one sorted by is bright, with an arrow. */
static void draw_table_head(void)
{
    char key[32];
    int asc = x_font_ascent(f_mono);

    snprintf(key, sizeof key, "%d|%d|%d", sort_col, sort_rev, layout_gen);
    if (strcmp(key, head_key) == 0)
        return;
    strcpy(head_key, key);
    x_color(c_panel);
    x_fill_rect(tbl_x - 4, head_y, b_procs.w - 2 * P + 8, row_h);
    x_text_font(f_mono);
    for (int c = 0; c < NCOLS; c++) {
        int tw = x_text_width(f_mono, cols[c].title), x0 = tbl_x + col_at[c] * char_w;
        int tx = cols[c].right ? x0 + cols[c].chars * char_w - tw : x0;

        x_color(c == sort_col ? c_text : c_faint);
        x_text(tx, head_y + 2 + asc, cols[c].title);
        if (c == sort_col) {
            int ax = cols[c].right ? tx - 11 : tx + tw + 4, ay = head_y + 2 + asc / 2 - 1;
            int down = sorts_down(sort_key) != sort_rev;
            short tri[6] = { (short)ax, (short)(down ? ay : ay + 4), (short)(ax + 8),
                             (short)(down ? ay : ay + 4), (short)(ax + 4),
                             (short)(down ? ay + 4 : ay) };

            x_color(px(ACC_TX));
            x_fill_poly(tri, 3);
        }
    }
}

static void draw_procs(void)
{
    box b = b_procs;
    int x = tbl_x, w = b.w - 2 * P, n = rows_visible < MAXROWS ? rows_visible : MAXROWS;
    int barx = x + (col_at[NCOLS] + GAP) * char_w, barw = x + w - barx - 8;

    if (status[0] && now_ms() < status_until)
        label(70, b.x + P + 100, b.y + P, w - 100, 1, f_bold, status_bad ? px(ACC_CPU) : px(ACC_RX),
              c_panel, "%s", status);
    else
        label(70, b.x + P + 100, b.y + P, w - 100, 1, f_text, c_dim, c_panel,
              "%d processes",
              nprocs);
    draw_table_head();

    if (sel < scroll)
        scroll = sel;
    if (sel >= scroll + n)
        scroll = sel - n + 1;
    if (scroll > nprocs - n)
        scroll = nprocs - n;
    if (scroll < 0)
        scroll = 0;

    for (int r = 0; r < n; r++) {
        int i = scroll + r, yy = rows_y + r * row_h, bar = 0;
        char key[160], text[128];

        if (i < nprocs) {
            Proc *p = &procs[i];

            proc_row(text, p);
            bar = p->cpu_pm >= 1000 ? barw : (int)((long)barw * p->cpu_pm / 1000);
            snprintf(key, sizeof key, "%s|%d|%d|%d", text, i == sel, bar, layout_gen);
        } else {
            text[0] = 0;
            snprintf(key, sizeof key, "|%d", layout_gen);
        }
        if (strcmp(key, row_key[r]) == 0)
            continue;
        strcpy(row_key[r], key);
        x_color(i == sel && i < nprocs ? c_sel : c_panel);
        x_fill_rect(x - 4, yy, w + 8, row_h);
        if (!text[0])
            continue;
        x_text_font(f_mono);
        x_color(i == sel ? c_text : c_dim);
        x_text(x, yy + 2 + x_font_ascent(f_mono), text);
        if (bar > 0 && barw > 24) {
            x_color(px(mix(ACC_CPU, RGB_PANEL, i == sel ? 0 : 200)));
            round_rect(barx, yy + row_h / 2 - 2, bar < 4 ? 4 : bar, 5, 2);
        }
    }
}

/* The process row at (x, y), or -1. */
static int row_at(int x, int y)
{
    int r;

    if (x < b_procs.x || x >= b_procs.x + b_procs.w || y < rows_y)
        return -1;
    r = (y - rows_y) / row_h;
    return r < rows_visible && scroll + r < nprocs ? scroll + r : -1;
}

/* The column heading at (x, y), or -1. */
static int column_at(int x, int y)
{
    int ch;

    if (y < head_y || y >= head_y + row_h || x < tbl_x - 4)
        return -1;
    ch = (x - tbl_x) / char_w;
    for (int c = 0; c < NCOLS; c++)
        if (ch >= col_at[c] - GAP / 2 && ch < col_at[c] + cols[c].chars + GAP / 2 + 1)
            return c;
    return -1;
}

/* ---------- menus ---------- */

enum { MENU_NONE, MENU_RATE, MENU_PROC };
#define ITEM_H 22
#define MAXITEMS 8

static struct {
    int kind, x, y, w, h, n, hover, pid;
    char title[48];
    struct { char text[24], side[12]; int arg, enabled; } it[MAXITEMS];
} menu;

static const struct { const char *text, *name; int sig; } signals[] = {
    { "Terminate", "TERM", SIGTERM }, { "Interrupt", "INT", SIGINT },
    { "Hang up", "HUP", SIGHUP },     { "Stop", "STOP", SIGSTOP },
    { "Continue", "CONT", SIGCONT },  { "Kill", "KILL", SIGKILL },
};
#define NSIGNALS (int)(sizeof signals / sizeof signals[0])

static long long next_ms;       /* when the next sample is due */

static int menu_top(void) { return menu.y + 4 + (menu.title[0] ? ITEM_H + 4 : 0); }

/* Size the menu for its items and put it at (x, y), inside the window. */
static void menu_place(int x, int y)
{
    int w = x_text_width(f_bold, menu.title) + 24;

    for (int i = 0; i < menu.n; i++) {
        int iw = 28 + x_text_width(f_text, menu.it[i].text) + 24 + x_text_width(f_text, menu.it[i].side) + 12;

        if (iw > w)
            w = iw;
    }
    menu.w = w < 140 ? 140 : w;
    menu.y = y;
    menu.h = menu_top() - menu.y + menu.n * ITEM_H + 4;
    menu.x = x + menu.w > x_width - 4 ? x_width - 4 - menu.w : x;
    if (y + menu.h > x_height - 4)
        menu.y = x_height - 4 - menu.h;
    menu.hover = -1;
}

static void menu_paint(void)
{
    int top = menu_top();

    x_color(c_track);
    round_rect(menu.x - 1, menu.y - 1, menu.w + 2, menu.h + 2, 7);
    x_color(px(mix(RGB_PANEL, RGB_BG, 400)));
    round_rect(menu.x, menu.y, menu.w, menu.h, 6);
    if (menu.title[0]) {
        x_text_font(f_bold);
        x_color(c_dim);
        x_text(menu.x + 12, menu.y + 4 + (ITEM_H - font_h(f_bold)) / 2 + x_font_ascent(f_bold),
               menu.title);
        x_color(c_grid);
        x_fill_rect(menu.x + 8, top - 3, menu.w - 16, 1);
    }
    for (int i = 0; i < menu.n; i++) {
        int iy = top + i * ITEM_H, ty = iy + (ITEM_H - font_h(f_text)) / 2 + x_font_ascent(f_text);

        if (i == menu.hover && menu.it[i].enabled) {
            x_color(c_sel);
            round_rect(menu.x + 4, iy, menu.w - 8, ITEM_H, 4);
        }
        if (menu.kind == MENU_RATE && menu.it[i].arg == interval_idx) {
            x_color(px(ACC_TX));
            x_fill_ellipse(menu.x + 11, iy + ITEM_H / 2 - 3, 7, 7);
        }
        x_text_font(f_text);
        x_color(menu.it[i].enabled ? c_text : c_faint);
        x_text(menu.x + 28, ty, menu.it[i].text);
        x_color(menu.it[i].enabled ? c_dim : c_faint);
        x_text(menu.x + menu.w - 12 - x_text_width(f_text, menu.it[i].side), ty, menu.it[i].side);
    }
}

static int menu_inside(int x, int y)
{
    return menu.kind && x >= menu.x && x < menu.x + menu.w && y >= menu.y && y < menu.y + menu.h;
}

static int menu_hit(int x, int y)
{
    int i;

    if (!menu_inside(x, y) || y < menu_top())
        return -1;
    i = (y - menu_top()) / ITEM_H;
    return i < menu.n ? i : -1;
}

static void menu_open_rate(void)
{
    int n = (int)(sizeof intervals_ms / sizeof intervals_ms[0]);

    menu.kind = MENU_RATE;
    menu.title[0] = 0;
    menu.n = n;
    for (int i = 0; i < n; i++) {
        snprintf(menu.it[i].text, sizeof menu.it[i].text, "every %g s", intervals_ms[i] / 1000.0);
        snprintf(menu.it[i].side, sizeof menu.it[i].side, "%s",
                 i == 0 ? "fastest" : i == n - 1 ? "lightest" : "");
        menu.it[i].arg = i;
        menu.it[i].enabled = 1;
    }
    menu_place(b_rate.x, b_rate.y + b_rate.h + 4);
    menu.x = b_rate.x + b_rate.w - menu.w;     /* under the button, right edges lined up */
}

static void menu_open_proc(const Proc *p, int x, int y)
{
    /* Not init (the kernel stops if it ends) or kernel threads (they ignore signals). */
    int ok = p->pid > 2 && p->ppid != 2;

    menu.kind = MENU_PROC;
    menu.pid = p->pid;
    snprintf(menu.title, sizeof menu.title, "%d  %s", p->pid, p->name);
    menu.n = NSIGNALS;
    for (int i = 0; i < NSIGNALS; i++) {
        snprintf(menu.it[i].text, sizeof menu.it[i].text, "%s", signals[i].text);
        snprintf(menu.it[i].side, sizeof menu.it[i].side, "%s", signals[i].name);
        menu.it[i].arg = i;
        menu.it[i].enabled = ok;
    }
    menu_place(x + 2, y + 2);
}

/* Close the menu: everything under it is drawn again. */
static void menu_close(void)
{
    menu.kind = MENU_NONE;
    layout();
    forget_rows();
}

static void menu_choose(int i)
{
    if (menu.kind == MENU_RATE) {
        interval_idx = menu.it[i].arg;
        next_ms = now_ms() + intervals_ms[interval_idx];
    } else if (menu.kind == MENU_PROC) {
        const char *name = "";

        for (int j = 0; j < nprocs; j++)
            if (procs[j].pid == menu.pid)
                name = procs[j].name;
        if (kill(menu.pid, signals[menu.it[i].arg].sig) == 0) {
            snprintf(status, sizeof status, "sent %s to %d %s", signals[menu.it[i].arg].name,
                     menu.pid, name);
            status_bad = 0;
        } else {
            snprintf(status, sizeof status, "%s to %d: %s", signals[menu.it[i].arg].name, menu.pid,
                     strerror(errno));
            status_bad = 1;
        }
        status_until = now_ms() + 4000;
        next_ms = now_ms() + 300;       /* show what it did soon */
    }
    menu_close();
}

/* A mouse button; returns whether anything changed. */
static int on_button(int button, int x, int y)
{
    int c, i;

    if (menu.kind) {
        if (button > 3)
            return 0;
        if (menu_inside(x, y)) {
            i = menu_hit(x, y);
            if (i >= 0 && menu.it[i].enabled) {
                menu_choose(i);
                return 1;
            }
            return 0;
        }
        menu_close();
        if (button != 3)                /* a right click elsewhere opens another */
            return 1;
    }
    if (button == 4 || button == 5) {
        move_sel(button == 4 ? -3 : 3);
        return 1;
    }
    if (x >= b_rate.x && x < b_rate.x + b_rate.w && y >= b_rate.y && y < b_rate.y + b_rate.h) {
        menu_open_rate();
        return 1;
    }
    if (button == 1 && (c = column_at(x, y)) >= 0) {
        sort_by(c);
        return 1;
    }
    if ((i = row_at(x, y)) >= 0) {
        move_sel(i - sel);
        if (button == 3)
            menu_open_proc(&procs[i], x, y);
        return 1;
    }
    return 1;
}

/* A key while a menu is open. */
static void menu_key(int key)
{
    int d = key == X_KEY_UP || key == 'k' ? -1 : key == X_KEY_DOWN || key == 'j' ? 1 : 0;

    if (d != 0) {
        for (int k = 0, i = menu.hover; k < menu.n; k++) {
            i = i < 0 ? (d > 0 ? 0 : menu.n - 1) : (i + d + menu.n) % menu.n;
            if (menu.it[i].enabled) {
                menu.hover = i;
                break;
            }
        }
    } else if (key == X_KEY_RETURN || key == ' ') {
        if (menu.hover >= 0 && menu.it[menu.hover].enabled)
            menu_choose(menu.hover);
    } else {
        menu_close();
    }
}

/* ---------- main ---------- */

static void update(int sampled)
{
    if (sampled) {
        graph_push(&g_cpu);
        graph_push(&g_mem);
        graph_push(&g_net);
        graph_push(&g_disk);
    }
    draw_header();
    draw_cpu();
    draw_mem();
    draw_net();
    draw_disk();
    draw_procs();
    if (menu.kind)
        menu_paint();                   /* on top of anything just drawn */
    x_present();
    x_sync();
}

static void on_key(int key, int *quit)
{
    int page = rows_visible > 1 ? rows_visible - 1 : 1;

    if (menu.kind) {
        menu_key(key);
        return;
    }
    switch (key) {
    case 'q': case 'Q': case X_KEY_ESCAPE:
        *quit = 1;
        break;
    case 'k': case X_KEY_UP:
        move_sel(-1);
        break;
    case 'j': case X_KEY_DOWN:
        move_sel(1);
        break;
    case X_KEY_PAGE_UP:
        move_sel(-page);
        break;
    case X_KEY_PAGE_DOWN:
        move_sel(page);
        break;
    case 's':
        sort_col = (sort_col + 1) % NCOLS;
        sort_key = cols[sort_col].key;
        sort_rev = 0;
        sort_procs();
        break;
    case 'r':
        sort_rev = !sort_rev;
        sort_procs();
        break;
    case '+': case '=':
        if (interval_idx > 0)
            interval_idx--;
        break;
    case '-': case '_':
        if (interval_idx < (int)(sizeof intervals_ms / sizeof intervals_ms[0]) - 1)
            interval_idx++;
        break;
    }
}

int main(int argc, char **argv)
{
    struct timespec ts = { 0, 300000000L };
    long pg = sysconf(_SC_PAGESIZE), tck = sysconf(_SC_CLK_TCK);
    long long resize_at = 0;
    int quit = 0;

    if (argc > 2 && strcmp(argv[1], "-display") == 0)
        setenv("DISPLAY", argv[2], 1);
    if (pulse_init() != 0) {
        fprintf(stderr, "xpulse: out of memory\n");
        return 1;
    }
    if (pg > 0)
        page_kb = pg / 1024;
    if (tck > 0)
        clk_tck = tck;
    uname(&uts);
    sample();
    nanosleep(&ts, NULL);           /* a short first interval so rates show */
    sample();
    sort_procs();

    if (x_open(NULL) < 0)
        return 1;
    x_background(x_rgb(RGB_BG.r, RGB_BG.g, RGB_BG.b));
    x_window("pulse", 980, 680);
    x_min_size(760, 520);
    setup_style();
    layout();
    forget_rows();
    update(0);

    next_ms = now_ms() + intervals_ms[interval_idx];
    while (!quit) {
        struct x_event e;
        long long now = now_ms(), until = resize_at && resize_at < next_ms ? resize_at : next_ms;
        int changed = 0;

        if (until > now)
            x_wait((int)(until - now));
        while (x_event(&e)) {
            switch (e.type) {
            case XE_CLOSE:
                quit = 1;
                break;
            case XE_RESIZE:
                /* Not while the window is being dragged: it keeps its last
                 * picture, and is laid out again once the size settles. */
                resize_at = now_ms() + SETTLE_MS;
                if (menu.kind)
                    menu.kind = MENU_NONE;
                break;
            case XE_MOTION:
                if (menu.kind) {
                    int h = menu_hit(e.x, e.y);

                    if (h >= 0 && !menu.it[h].enabled)
                        h = -1;
                    if (h != menu.hover) {
                        menu.hover = h;
                        menu_paint();
                        x_present();
                    }
                }
                break;
            case XE_BUTTON_DOWN:
                changed |= on_button(e.button, e.x, e.y);
                break;
            case XE_KEY_DOWN:
                on_key(e.key, &quit);
                changed = 1;
                break;
            }
        }
        now = now_ms();
        if (resize_at && now >= resize_at && !quit) {
            resize_at = 0;
            layout();
            forget_rows();
            changed = 1;
        }
        if (now >= next_ms) {
            sample();
            sort_procs();
            update(1);
            next_ms += intervals_ms[interval_idx];
            if (next_ms <= now)
                next_ms = now + intervals_ms[interval_idx];
        } else if (changed && !quit) {
            update(0);
        }
    }
    x_close();
    return 0;
}
