/*
 * pulse_draw.c - pulse's screen drawing: primitives and panels.
 * See pulse.c for input and the main loop, pulse_data.c for sampling.
 *
 * History graphs use braille characters (2x4 dots per cell); meters use
 * eighth-blocks for smooth fills.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pulse.h"
#include "pulse_ui.h"

/* term.h's buffer lives in pulse.c; route output through it. */
#define term_put   out_put
#define term_str   out_str
#define term_goto  out_goto
#define term_fg    out_fg
#define term_bg    out_bg
#define term_reset out_reset
#define term_flush out_flush

#define RAMP_N  10
#define MIN_COLS 64
#define MIN_ROWS 20

/* ---------- colours ---------- */

static const unsigned char ramp_heat[RAMP_N]   = { 41, 77, 113, 149, 185, 221, 215, 209, 203, 197 };
static const unsigned char ramp_cyan[RAMP_N]   = { 24, 25, 31, 32, 38, 39, 45, 51, 87, 123 };
static const unsigned char ramp_pink[RAMP_N]   = { 90, 126, 127, 163, 164, 170, 206, 212, 218, 225 };
static const unsigned char ramp_violet[RAMP_N] = { 54, 55, 56, 92, 93, 99, 135, 141, 177, 183 };
static const unsigned char ramp_gold[RAMP_N]   = { 94, 130, 136, 172, 178, 214, 220, 221, 222, 229 };
static const unsigned char ramp_green[RAMP_N]  = { 22, 28, 34, 35, 41, 42, 48, 84, 120, 157 };

#define C_BORDER    239
#define C_TEXT      252
#define C_DIM       244
#define C_FAINT     240
#define C_TRACK     236
#define C_HEADER_BG 235
#define C_SELECT_BG 237

#define A_CPU  41
#define A_MEM  141
#define A_NET  45
#define A_DISK 214
#define A_PROC 203

/* ---------- UI state ---------- */

static const char *sort_names[SORT_COUNT] = { "cpu", "mem", "pid", "name" };
static int scroll;

/* ---------- small helpers ---------- */

/* Display width of a UTF-8 string (every code point counts as 1). */
static int dwidth(const char *s)
{
    int w = 0;

    for (; *s; s++)
        if ((*s & 0xC0) != 0x80)
            w++;
    return w;
}

/* Emit at most maxw columns of s; returns the columns written. */
static int put_clip(const char *s, int maxw)
{
    int w = 0;

    while (*s && w < maxw) {
        const char *q = s + 1;

        while ((*q & 0xC0) == 0x80)
            q++;
        term_put(s, (size_t)(q - s));
        s = q;
        w++;
    }
    return w;
}

static void rep(const char *s, int n)
{
    while (n-- > 0)
        term_str(s);
}

static void textf(int r, int c, int maxw, int fg, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    if (maxw <= 0)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    term_goto(r, c);
    if (fg >= 0)
        term_fg(fg);
    put_clip(buf, maxw);
}

static int ramp_at(const unsigned char *ramp, int pm)
{
    int i = pm * RAMP_N / 1001;

    return ramp[i < 0 ? 0 : i >= RAMP_N ? RAMP_N - 1 : i];
}

/* ---------- drawing primitives ---------- */

/* A rounded box with a title on the left and optional info on the right. */
static void box(int r, int c, int h, int w, const char *title, int accent,
                const char *info)
{
    int tw = dwidth(title) + 2;
    int iw = info ? dwidth(info) + 3 : 0;
    int fill = w - 3 - tw - iw;

    if (fill < 0) {
        iw = 0;
        fill = w - 3 - tw;
    }
    term_reset();
    term_goto(r, c);
    term_fg(C_BORDER);
    term_str("╭─");
    if (fill >= 0) {
        term_str("\x1b[1m");
        term_fg(accent);
        term_put(" ", 1);
        term_str(title);
        term_put(" ", 1);
        term_str("\x1b[22m");
        term_fg(C_BORDER);
    } else {
        fill = w - 3;
    }
    rep("─", fill);
    if (iw > 0) {
        term_fg(C_DIM);
        term_put(" ", 1);
        term_str(info);
        term_put(" ", 1);
        term_fg(C_BORDER);
        term_str("─");
    }
    term_str("╮");

    for (int i = 1; i < h - 1; i++) {
        term_goto(r + i, c);
        term_str("│");
        rep(" ", w - 2);
        term_str("│");
    }
    term_goto(r + h - 1, c);
    term_str("╰");
    rep("─", w - 2);
    term_str("╯");
}

/* Text set into a box's bottom border. */
static void box_footer(int r, int c, int w, const char *text)
{
    if (w < 8)
        return;
    term_goto(r, c + 2);
    term_fg(C_DIM);
    term_put(" ", 1);
    put_clip(text, w - 6);
    term_put(" ", 1);
}

/* A horizontal meter with a colour gradient and eighth-block precision. */
static void meter(int r, int c, int w, int pm, const unsigned char *ramp)
{
    static const char *eighths[8] = { "", "▏", "▎", "▍", "▌", "▋", "▊", "▉" };
    int fill8;

    if (w <= 0)
        return;
    if (pm < 0)
        pm = 0;
    if (pm > 1000)
        pm = 1000;
    fill8 = pm * w * 8 / 1000;
    term_goto(r, c);
    term_bg(C_TRACK);
    for (int i = 0; i < w; i++) {
        int v = fill8 - i * 8;

        term_fg(ramp[i * RAMP_N / w]);
        if (v >= 8)
            term_str("█");
        else if (v > 0)
            term_str(eighths[v]);
        else
            term_put(" ", 1);
    }
    term_reset();
}

/*
 * A braille history graph, newest sample at the right edge. Each cell holds
 * two samples (left and right dot columns) and four dots of height. With
 * down set, bars hang from the top instead of rising from the bottom.
 */
static void graph(int r, int c, int h, int w, const unsigned int *hist,
                  unsigned int maxv, const unsigned char *ramp, int down)
{
    static const unsigned char up_l[5] = { 0, 0x40, 0x44, 0x46, 0x47 };
    static const unsigned char up_r[5] = { 0, 0x80, 0xA0, 0xB0, 0xB8 };
    static const unsigned char dn_l[5] = { 0, 0x01, 0x03, 0x07, 0x47 };
    static const unsigned char dn_r[5] = { 0, 0x08, 0x18, 0x38, 0xB8 };
    int dots = h * 4;

    if (h <= 0 || w <= 0)
        return;
    if (w > HIST / 2)
        w = HIST / 2;
    if (maxv == 0)
        maxv = 1;

    for (int j = 0; j < h; j++) {
        int lvl = down ? j : h - 1 - j;

        term_goto(r + j, c);
        term_fg(ramp[lvl * RAMP_N / h]);
        for (int i = 0; i < w; i++) {
            int fill[2];
            unsigned int bits;

            for (int s = 0; s < 2; s++) {
                int idx = HIST - 2 * w + 2 * i + s;
                unsigned int v = idx >= HIST - h_count ? hist[idx] : 0;
                int d = (int)((unsigned long long)v * (unsigned long long)dots / maxv);

                if (v > 0 && d == 0)
                    d = 1;
                d -= lvl * 4;
                fill[s] = d < 0 ? 0 : d > 4 ? 4 : d;
            }
            bits = down ? (unsigned int)(dn_l[fill[0]] | dn_r[fill[1]])
                        : (unsigned int)(up_l[fill[0]] | up_r[fill[1]]);
            if (bits == 0) {
                term_put(" ", 1);
            } else {
                char u[3];

                u[0] = (char)0xE2;
                u[1] = (char)(0xA0 | (bits >> 6));
                u[2] = (char)(0x80 | (bits & 0x3F));
                term_put(u, 3);
            }
        }
    }
}

static unsigned int hist_max(const unsigned int *h, int w)
{
    unsigned int m = 0;
    int n = 2 * w;

    if (n > h_count)
        n = h_count;
    for (int i = HIST - n; i < HIST; i++)
        if (h[i] > m)
            m = h[i];
    return m;
}

/* ---------- panels ---------- */

static void draw_header(void)
{
    char right[160];
    unsigned long d = uptime_s / 86400, hh = uptime_s / 3600 % 24;
    unsigned long mm = uptime_s / 60 % 60, ss = uptime_s % 60;
    int rw, lw = 9 + dwidth(uts.nodename) + 3 + dwidth(uts.release) + 1 +
                 dwidth(uts.machine);

    if (d > 0)
        snprintf(right, sizeof right, "up %lud %02lu:%02lu:%02lu · load %s %s %s ",
                 d, hh, mm, ss, load1, load5, load15);
    else
        snprintf(right, sizeof right, "up %lu:%02lu:%02lu · load %s %s %s ",
                 hh, mm, ss, load1, load5, load15);

    term_reset();
    term_goto(1, 1);
    term_bg(C_HEADER_BG);
    rep(" ", cols);
    term_goto(1, 1);
    term_str("\x1b[1m");
    term_fg(A_PROC);
    term_str(" ◆ pulse ");
    term_str("\x1b[22m");
    term_fg(C_TEXT);
    term_str(uts.nodename);
    term_fg(C_DIM);
    term_str(" · ");
    term_str(uts.release);
    term_put(" ", 1);
    term_str(uts.machine);

    rw = dwidth(right);
    if (rw < cols - lw) {
        term_goto(1, cols - rw + 1);
        term_fg(C_DIM);
        term_str(right);
    }
    term_reset();
}

/*
 * One history strip per core, stacked top to bottom, each with a small
 * gutter on the left naming the core and showing its current load.
 */
static void draw_cores(int r, int c, int h, int w)
{
    int gut = 6, row = r;

    for (int i = 0; i < ncore; i++) {
        int sh = h / ncore + (i < h % ncore);
        char pct[8];

        snprintf(pct, sizeof pct, "%3d%%", (core_pm[i] + 5) / 10);
        textf(row, c, gut - 1, C_DIM, "cpu%d", i);
        term_str("\x1b[1m");
        textf(row + 1, c, gut - 1, ramp_at(ramp_heat, core_pm[i]), "%s", pct);
        term_str("\x1b[22m");
        for (int j = 0; j < sh; j++) {
            term_goto(row + j, c + gut - 1);
            term_fg(C_TRACK);
            term_str("▕");
        }
        graph(row, c + gut, sh, w - gut, h_core + i * HIST, 1000, ramp_heat, 0);
        row += sh;
    }
}

static void draw_cpu(int r, int c, int h, int w)
{
    char info[64];
    int ir = r + 1, ic = c + 1, ih = h - 2, iw = w - 2;
    int pw = iw >= 70 ? 28 : iw / 3;
    int gw = iw - pw - 1;
    int pc = ic + gw + 1;
    int mw = pw - 10;
    static const char *labels[4] = { "usr", "sys", "irq", "iow" };
    int vals[4];
    const unsigned char *ramps[4] = { ramp_green, ramp_heat, ramp_gold, ramp_violet };
    int row = 0;

    /* Per-core strips need two rows each (label + percentage). */
    int stacked = ncore > 1 && ih >= 2 * ncore && gw > 16;

    snprintf(info, sizeof info, "%s · %d core%s", uts.machine, ncpu,
             ncpu == 1 ? "" : "s");
    box(r, c, h, w, "cpu", A_CPU, info);
    if (stacked)
        draw_cores(ir, ic, ih, gw);
    else
        graph(ir, ic, ih, gw, h_cpu, 1000, ramp_heat, 0);

    /* Big total, then the per-category meters. */
    textf(ir + row, pc, pw, C_DIM, "total ");
    term_str("\x1b[1m");
    term_fg(ramp_at(ramp_heat, cpu_pm));
    {
        char pct[16];
        snprintf(pct, sizeof pct, "%d.%d%%", cpu_pm / 10, cpu_pm % 10);
        put_clip(pct, pw - 6);
    }
    term_str("\x1b[22m");
    row++;

    /* Without per-core graphs, list each core's load on one line. */
    if (!stacked && ncore > 1 && row < ih) {
        int col = pc + 5;

        textf(ir + row, pc, 5, C_DIM, "core");
        for (int i = 0; i < ncore && col + 4 <= pc + pw; i++, col += 5)
            textf(ir + row, col, 4, ramp_at(ramp_heat, core_pm[i]), "%3d%%",
                  (core_pm[i] + 5) / 10);
        row++;
    }

    vals[0] = usr_pm;
    vals[1] = sys_pm;
    vals[2] = irq_pm;
    vals[3] = io_pm;
    for (int i = 0; i < 4 && row < ih; i++, row++) {
        textf(ir + row, pc, 4, C_DIM, "%s", labels[i]);
        meter(ir + row, pc + 4, mw, vals[i], ramps[i]);
        textf(ir + row, pc + 4 + mw + 1, 5, C_TEXT, "%3d%%", (vals[i] + 5) / 10);
    }
    if (row < ih) {
        textf(ir + row, pc, pw, C_DIM, "load ");
        term_fg(C_TEXT);
        term_str(load1);
        term_fg(C_DIM);
        term_put(" ", 1);
        term_str(load5);
        term_put(" ", 1);
        term_str(load15);
        row++;
    }
    if (row < ih)
        textf(ir + row++, pc, pw, C_DIM, "ctx %lu/s  int %lu/s", ctxt_rate, intr_rate);
    if (row < ih)
        textf(ir + row++, pc, pw, C_DIM, "tasks %d  running %d", tasks_total, tasks_running);
}

static void mem_row(int r, int c, int w, const char *label, unsigned long kb,
                    const unsigned char *ramp)
{
    char size[16];
    int pm = mem_total ? (int)(kb * 1000ULL / mem_total) : 0;
    int mw = w - 6 - 12;

    fmt_bytes(size, sizeof size, (unsigned long long)kb * 1024);
    textf(r, c, 6, C_DIM, "%s", label);
    meter(r, c + 6, mw, pm, ramp);
    textf(r, c + 6 + mw + 1, 11, C_TEXT, "%5s %3d%%", size, (pm + 5) / 10);
}

static void draw_mem(int r, int c, int h, int w)
{
    char info[32];
    int ir = r + 1, ic = c + 1, ih = h - 2, iw = w - 2;

    fmt_bytes(info, sizeof info, (unsigned long long)mem_total * 1024);
    box(r, c, h, w, "mem", A_MEM, info);
    if (ih > 0)
        mem_row(ir, ic, iw, "used", mem_used, ramp_violet);
    if (ih > 1)
        mem_row(ir + 1, ic, iw, "cache", mem_cached + mem_buffers, ramp_cyan);
    if (ih > 2)
        mem_row(ir + 2, ic, iw, "avail", mem_avail, ramp_green);
    if (ih > 3)
        mem_row(ir + 3, ic, iw, "free", mem_free, ramp_gold);
    if (ih > 5)
        graph(ir + 4, ic, ih - 4, iw, h_mem, 1000, ramp_violet, 0);
}

static void draw_net(int r, int c, int h, int w)
{
    char a[16], b[16];
    int ir = r + 1, ic = c + 1, ih = h - 2, iw = w - 2;
    int tw = 13, gw = iw - tw - 1, tc = ic + gw + 1;
    int rh = (ih + 1) / 2, th = ih - rh;
    unsigned int maxv;

    box(r, c, h, w, "net", A_NET, net_name);
    if (ih <= 0)
        return;
    maxv = hist_max(h_rx, gw);
    if (hist_max(h_tx, gw) > maxv)
        maxv = hist_max(h_tx, gw);
    if (maxv < 1024)
        maxv = 1024;
    graph(ir, ic, rh, gw, h_rx, maxv, ramp_cyan, 0);
    graph(ir + rh, ic, th, gw, h_tx, maxv, ramp_pink, 1);

    fmt_bytes(a, sizeof a, rx_rate);
    textf(ir, tc, tw, A_NET, "▼ %s/s", a);
    if (ih >= 4) {
        fmt_bytes(b, sizeof b, rx_cur);
        textf(ir + 1, tc, tw, C_DIM, "  %s", b);
        fmt_bytes(b, sizeof b, tx_cur);
        textf(ir + ih - 2, tc, tw, C_DIM, "  %s", b);
    }
    if (ih == 3 || ih >= 6) {
        fmt_bytes(b, sizeof b, maxv);
        textf(ir + ih / 2, tc, tw, C_FAINT, "  max %s", b);
    }
    fmt_bytes(a, sizeof a, tx_rate);
    textf(ir + ih - 1, tc, tw, 212, "▲ %s/s", a);
}

static void draw_disk(int r, int c, int h, int w)
{
    char a[16], b[16];
    int ir = r + 1, ic = c + 1, ih = h - 2, iw = w - 2;
    int pm = fs_total ? (int)(fs_used * 1000 / fs_total) : 0;
    int mw = iw - 3 - 12;

    box(r, c, h, w, "disk", A_DISK, disk_name[0] ? disk_name : "none");
    if (ih <= 0)
        return;
    fmt_bytes(a, sizeof a, rd_rate);
    fmt_bytes(b, sizeof b, wr_rate);
    textf(ir, ic, iw, C_TEXT, "rd %s/s  wr %s/s  busy %d%%", a, b,
          (disk_busy_pm + 5) / 10);
    if (ih > 1) {
        fmt_bytes(a, sizeof a, fs_total);
        textf(ir + 1, ic, 3, C_DIM, "/");
        meter(ir + 1, ic + 2, mw, pm, ramp_gold);
        textf(ir + 1, ic + 2 + mw + 1, 12, C_TEXT, "%3d%% of %s", (pm + 5) / 10, a);
    }
    if (ih > 2) {
        unsigned int maxv = hist_max(h_disk, iw);

        graph(ir + 2, ic, ih - 2, iw, h_disk, maxv < 4096 ? 4096 : maxv, ramp_gold, 0);
    }
}

int proc_visible(int h)
{
    int v = h - 3;      /* borders and the column header */

    return v < 1 ? 1 : v;
}

static void draw_procs(int r, int c, int h, int w)
{
    char info[32], footer[128];
    int ir = r + 1, ic = c + 1, ih = h - 2, iw = w - 2;
    int barw = iw >= 60 ? 10 : 0;
    int namew = iw - 26 - (barw ? barw + 1 : 0);
    int vis = proc_visible(h);

    snprintf(info, sizeof info, "%d", nprocs);
    box(r, c, h, w, "proc", A_PROC, info);
    snprintf(footer, sizeof footer,
             "↑↓ · s sort:%s%s · r rev · +- %dms · q quit",
             sort_names[sort_key], sort_rev ? "↑" : "",
             intervals_ms[interval_idx]);
    box_footer(r + h - 1, c, w, footer);
    if (ih <= 0 || namew < 4)
        return;

    term_str("\x1b[1m");
    textf(ir, ic, iw, C_DIM, "%5s %-*s %s %3s %6s %5s", "PID", namew, "NAME",
          "S", "THR", "MEM", "CPU%");
    term_str("\x1b[22m");

    if (sel < scroll)
        scroll = sel;
    if (sel >= scroll + vis)
        scroll = sel - vis + 1;
    if (scroll > nprocs - vis)
        scroll = nprocs - vis;
    if (scroll < 0)
        scroll = 0;

    for (int i = 0; i < vis && i + 1 < ih; i++) {
        int idx = scroll + i;
        int rr = ir + 1 + i;
        const Proc *p;
        char mem[16];
        int sc;

        if (idx >= nprocs)
            break;
        p = &procs[idx];
        if (idx == sel) {
            term_goto(rr, ic);
            term_bg(C_SELECT_BG);
            rep(" ", iw);
        }
        fmt_bytes(mem, sizeof mem, (unsigned long long)p->rss_kb * 1024);
        sc = p->state == 'R' ? 41 : p->state == 'D' ? 203 :
             p->state == 'Z' ? 170 : C_FAINT;

        textf(rr, ic, 5, C_DIM, "%5d", p->pid);
        textf(rr, ic + 6, namew, p->rss_kb ? C_TEXT : C_DIM, "%s", p->name);
        textf(rr, ic + 7 + namew, 1, sc, "%c", p->state);
        textf(rr, ic + 9 + namew, 3, C_DIM, "%3d", p->threads);
        textf(rr, ic + 13 + namew, 6, p->rss_kb ? A_MEM : C_FAINT, "%6s",
              p->rss_kb ? mem : "-");
        textf(rr, ic + 20 + namew, 5,
              p->cpu_pm ? ramp_at(ramp_heat, p->cpu_pm * 4) : C_FAINT,
              "%3d.%d", p->cpu_pm / 10, p->cpu_pm % 10);
        if (barw) {
            meter(rr, ic + 26 + namew, barw, p->cpu_pm, ramp_heat);
            if (idx == sel)
                term_bg(C_SELECT_BG);
        }
        term_reset();
    }
}

void render(void)
{
    int top, ch, bh, lw, mh, nh, dh;

    if (cols < MIN_COLS || rows < MIN_ROWS) {
        term_reset();
        term_str("\x1b[2J");
        textf(rows / 2, 1, cols, C_TEXT, "pulse needs %dx%d (this is %dx%d)",
              MIN_COLS, MIN_ROWS, cols, rows);
        term_flush();
        return;
    }

    ch = rows >= 30 ? rows * 36 / 100 : 8;
    top = 2 + ch;
    bh = rows - 1 - ch;
    lw = cols * 45 / 100;
    mh = bh * 40 / 100;
    nh = bh * 34 / 100;
    dh = bh - mh - nh;

    draw_header();
    draw_cpu(2, 1, ch, cols);
    draw_mem(top, 1, mh, lw);
    draw_net(top + mh, 1, nh, lw);
    draw_disk(top + mh + nh, 1, dh, lw);
    draw_procs(top, lw + 1, bh, cols - lw);
    term_reset();
    term_flush();
}
