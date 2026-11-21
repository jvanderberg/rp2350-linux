/*
 * pulse_data.c - pulse's sampler: reads /proc and keeps the history.
 * See pulse.c for the user interface.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "pulse.h"

/* ---------- shared state (see pulse.h) ---------- */

struct utsname uts;
int ncpu = 1;

int cpu_pm, usr_pm, sys_pm, irq_pm, io_pm;
int core_pm[MAXCPU];
int ncore;
unsigned long ctxt_rate, intr_rate;
char load1[8], load5[8], load15[8];
int tasks_running, tasks_total;
unsigned long uptime_s;

unsigned long mem_total, mem_free, mem_avail, mem_buffers, mem_cached;
unsigned long mem_used;
int mem_used_pm;

char net_name[24];
unsigned long long rx_cur, tx_cur;
unsigned long rx_rate, tx_rate;

char disk_name[32];
unsigned long rd_rate, wr_rate;
int disk_busy_pm;
unsigned long long fs_total, fs_used;

unsigned int *h_cpu, *h_mem, *h_rx, *h_tx, *h_disk;
unsigned int *h_core;
int h_count;

Proc *procs;
int nprocs;
long page_kb = 4;
int sort_key = SORT_CPU, sort_rev, sel, sel_pid = -1;

/* ---------- sampler-private state ---------- */

static unsigned long long cpu_prev[8], cpu_cur[8], cpu_delta;
static unsigned long long core_busy[MAXCPU], core_tot[MAXCPU];
static unsigned long long ctxt_prev, ctxt_cur, intr_prev, intr_cur;
static unsigned long long rx_prev, tx_prev;
static unsigned long long rd_prev, wr_prev, busy_prev, rd_cur, wr_cur, busy_cur;
static Proc *prevp;
static int nprev;
static int primed;
static long long last_sample_us, sample_dt_us;

int pulse_init(void)
{
    h_cpu = calloc(HIST * (5 + MAXCPU), sizeof *h_cpu);
    procs = calloc(MAXPROC, sizeof *procs);
    prevp = calloc(MAXPROC, sizeof *prevp);
    if (h_cpu == NULL || procs == NULL || prevp == NULL)
        return -1;
    h_mem = h_cpu + HIST;
    h_rx = h_mem + HIST;
    h_tx = h_rx + HIST;
    h_disk = h_tx + HIST;
    h_core = h_disk + HIST;
    return 0;
}

static long long now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int read_file(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    size_t len = 0;

    buf[0] = 0;
    if (fd < 0)
        return -1;
    while (len < n - 1) {
        ssize_t r = read(fd, buf + len, n - 1 - len);
        if (r <= 0)
            break;
        len += (size_t)r;
    }
    close(fd);
    buf[len] = 0;
    return (int)len;
}

static void push(unsigned int *h, unsigned int v)
{
    memmove(h, h + 1, (HIST - 1) * sizeof *h);
    h[HIST - 1] = v;
}

static unsigned long rate(unsigned long long cur, unsigned long long prev)
{
    if (sample_dt_us <= 0 || cur < prev)
        return 0;
    return (unsigned long)((cur - prev) * 1000000ULL / (unsigned long long)sample_dt_us);
}

void fmt_bytes(char *out, size_t n, unsigned long long b)
{
    const char *units = "BKMGT";
    unsigned long long v = b * 10;      /* tenths of a unit */
    int u = 0;

    while (v >= 10240 && u < 4) {
        v /= 1024;
        u++;
    }
    /* v < 10240 here unless u hit the top unit, so 32 bits are enough. */
    unsigned int t = v > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (unsigned int)v;

    if (u == 0)
        snprintf(out, n, "%uB", t / 10);
    else if (t < 100)
        snprintf(out, n, "%u.%u%c", t / 10, t % 10, units[u]);
    else
        snprintf(out, n, "%u%c", t / 10, units[u]);
}


/* ---------- sampling ---------- */

/* One "cpuN ..." line of /proc/stat (s points just past "cpu"). */
static void sample_core(int i, const char *s)
{
    unsigned long long v[8] = { 0 }, tot = 0, busy, dt, db;

    while (isdigit((unsigned char)*s))
        s++;
    sscanf(s, "%llu %llu %llu %llu %llu %llu %llu %llu",
           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    for (int k = 0; k < 8; k++)
        tot += v[k];
    busy = tot - v[3] - v[4];
    dt = tot - core_tot[i];
    db = busy - core_busy[i];
    core_pm[i] = core_tot[i] && tot > core_tot[i] && busy >= core_busy[i]
               ? (int)(db * 1000 / dt) : 0;
    if (core_pm[i] > 1000)
        core_pm[i] = 1000;
    core_tot[i] = tot;
    core_busy[i] = busy;
}

static void sample_cpu(void)
{
    char buf[8192];
    unsigned long long d[8], tot = 0;
    char *line = buf;

    memcpy(cpu_prev, cpu_cur, sizeof cpu_cur);
    ctxt_prev = ctxt_cur;
    intr_prev = intr_cur;
    read_file("/proc/stat", buf, sizeof buf);

    ncpu = 0;
    while (line != NULL && *line) {
        char *nl = strchr(line, '\n');

        if (nl != NULL)
            *nl = 0;
        if (strncmp(line, "cpu ", 4) == 0)
            sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &cpu_cur[0], &cpu_cur[1], &cpu_cur[2], &cpu_cur[3],
                   &cpu_cur[4], &cpu_cur[5], &cpu_cur[6], &cpu_cur[7]);
        else if (strncmp(line, "cpu", 3) == 0 && isdigit((unsigned char)line[3])) {
            if (ncpu < MAXCPU)
                sample_core(ncpu, line + 3);
            ncpu++;
        }
        else if (strncmp(line, "intr ", 5) == 0)
            sscanf(line + 5, "%llu", &intr_cur);
        else if (strncmp(line, "ctxt ", 5) == 0)
            sscanf(line + 5, "%llu", &ctxt_cur);
        line = nl != NULL ? nl + 1 : NULL;
    }
    if (ncpu == 0)
        ncpu = 1;
    ncore = ncpu < MAXCPU ? ncpu : MAXCPU;

    for (int i = 0; i < 8; i++) {
        d[i] = cpu_cur[i] - cpu_prev[i];
        tot += d[i];
    }
    cpu_delta = tot;
    if (tot > 0) {
        cpu_pm = (int)((tot - d[3] - d[4]) * 1000 / tot);
        usr_pm = (int)((d[0] + d[1]) * 1000 / tot);
        sys_pm = (int)(d[2] * 1000 / tot);
        irq_pm = (int)((d[5] + d[6]) * 1000 / tot);
        io_pm = (int)(d[4] * 1000 / tot);
    }
    ctxt_rate = rate(ctxt_cur, ctxt_prev);
    intr_rate = rate(intr_cur, intr_prev);
}

static void sample_misc(void)
{
    char buf[256];

    if (read_file("/proc/loadavg", buf, sizeof buf) > 0)
        sscanf(buf, "%7s %7s %7s %d/%d", load1, load5, load15,
               &tasks_running, &tasks_total);
    if (read_file("/proc/uptime", buf, sizeof buf) > 0)
        sscanf(buf, "%lu", &uptime_s);
}

static void sample_mem(void)
{
    char buf[2048];
    char *line = buf;

    read_file("/proc/meminfo", buf, sizeof buf);
    while (line != NULL && *line) {
        char *nl = strchr(line, '\n');
        unsigned long v = 0;

        if (nl != NULL)
            *nl = 0;
        sscanf(strchr(line, ':') ? strchr(line, ':') + 1 : line, "%lu", &v);
        if (strncmp(line, "MemTotal:", 9) == 0)
            mem_total = v;
        else if (strncmp(line, "MemFree:", 8) == 0)
            mem_free = v;
        else if (strncmp(line, "MemAvailable:", 13) == 0)
            mem_avail = v;
        else if (strncmp(line, "Buffers:", 8) == 0)
            mem_buffers = v;
        else if (strncmp(line, "Cached:", 7) == 0)
            mem_cached = v;
        line = nl != NULL ? nl + 1 : NULL;
    }
    mem_used = mem_total > mem_free + mem_buffers + mem_cached
             ? mem_total - mem_free - mem_buffers - mem_cached : 0;
    mem_used_pm = mem_total ? (int)(mem_used * 1000ULL / mem_total) : 0;
}

static void sample_net(void)
{
    char buf[4096];
    char *line = buf;
    int n = 0, lineno = 0;

    rx_prev = rx_cur;
    tx_prev = tx_cur;
    rx_cur = tx_cur = 0;
    read_file("/proc/net/dev", buf, sizeof buf);
    while (line != NULL && *line) {
        char *nl = strchr(line, '\n');
        char *colon;

        if (nl != NULL)
            *nl = 0;
        colon = strchr(line, ':');
        if (lineno++ >= 2 && colon != NULL) {
            unsigned long long rx = 0, tx = 0;
            char *name = line;

            *colon = 0;
            while (*name == ' ')
                name++;
            if (strcmp(name, "lo") != 0 &&
                sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu",
                       &rx, &tx) == 2) {
                rx_cur += rx;
                tx_cur += tx;
                if (n == 0)
                    snprintf(net_name, sizeof net_name, "%s", name);
                n++;
            }
        }
        line = nl != NULL ? nl + 1 : NULL;
    }
    if (n == 0)
        snprintf(net_name, sizeof net_name, "none");
    else if (n > 1 && strlen(net_name) < sizeof net_name - 2)
        strcat(net_name, "+");
    rx_rate = rate(rx_cur, rx_prev);
    tx_rate = rate(tx_cur, tx_prev);
}

static void sample_disk(void)
{
    char buf[4096];
    char *line = buf;

    rd_prev = rd_cur;
    wr_prev = wr_cur;
    busy_prev = busy_cur;
    read_file("/proc/diskstats", buf, sizeof buf);
    disk_name[0] = 0;
    while (line != NULL && *line) {
        char *nl = strchr(line, '\n');
        char name[32];
        unsigned long long rd, wr, busy;

        if (nl != NULL)
            *nl = 0;
        if (sscanf(line, "%*u %*u %31s %*u %*u %llu %*u %*u %*u %llu %*u %*u %llu",
                   name, &rd, &wr, &busy) == 4 &&
            strncmp(name, "loop", 4) != 0 && strncmp(name, "ram", 3) != 0) {
            snprintf(disk_name, sizeof disk_name, "%s", name);
            rd_cur = rd * 512;
            wr_cur = wr * 512;
            busy_cur = busy;
            break;
        }
        line = nl != NULL ? nl + 1 : NULL;
    }
    rd_rate = rate(rd_cur, rd_prev);
    wr_rate = rate(wr_cur, wr_prev);
    disk_busy_pm = 0;
    if (sample_dt_us > 0 && busy_cur >= busy_prev) {
        unsigned long long pm = (busy_cur - busy_prev) * 1000000ULL
                              / (unsigned long long)sample_dt_us;
        disk_busy_pm = pm > 1000 ? 1000 : (int)pm;
    }
}

static void sample_fs(void)
{
    struct statvfs s;

    if (statvfs("/", &s) == 0) {
        fs_total = (unsigned long long)s.f_blocks * s.f_frsize;
        fs_used = (unsigned long long)(s.f_blocks - s.f_bfree) * s.f_frsize;
    }
}

static int cmp_proc(const void *a, const void *b)
{
    const Proc *p = a, *q = b;
    int r = 0;

    switch (sort_key) {
    case SORT_CPU:
        r = q->cpu_pm - p->cpu_pm;
        if (r == 0)
            r = (q->rss_kb > p->rss_kb) - (q->rss_kb < p->rss_kb);
        break;
    case SORT_MEM:
        r = (q->rss_kb > p->rss_kb) - (q->rss_kb < p->rss_kb);
        break;
    case SORT_PID:
        r = p->pid - q->pid;
        break;
    case SORT_NAME:
        r = strcmp(p->name, q->name);
        break;
    }
    if (r == 0)
        r = p->pid - q->pid;
    return sort_rev ? -r : r;
}

void sort_procs(void)
{
    qsort(procs, (size_t)nprocs, sizeof procs[0], cmp_proc);
    if (sel_pid >= 0) {
        for (int i = 0; i < nprocs; i++) {
            if (procs[i].pid == sel_pid) {
                sel = i;
                return;
            }
        }
    }
    if (sel >= nprocs)
        sel = nprocs > 0 ? nprocs - 1 : 0;
    sel_pid = nprocs > 0 ? procs[sel].pid : -1;
}

static void sample_procs(void)
{
    DIR *dir = opendir("/proc");
    struct dirent *e;

    if (dir == NULL)
        return;
    memcpy(prevp, procs, (size_t)nprocs * sizeof procs[0]);
    nprev = nprocs;
    nprocs = 0;

    while ((e = readdir(dir)) != NULL && nprocs < MAXPROC) {
        char path[64], buf[512];
        char *lp, *rp, state = '?';
        unsigned long ut = 0, st = 0;
        long rss = 0;
        int thr = 0;
        size_t nl;
        Proc *p;

        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        if (read_file(path, buf, sizeof buf) <= 0)
            continue;
        lp = strchr(buf, '(');
        rp = strrchr(buf, ')');
        if (lp == NULL || rp == NULL || rp < lp)
            continue;
        if (sscanf(rp + 2,
                   "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu "
                   "%*d %*d %*d %*d %d %*d %*u %*u %ld",
                   &state, &ut, &st, &thr, &rss) != 5)
            continue;

        p = &procs[nprocs];
        p->pid = atoi(buf);
        nl = (size_t)(rp - lp - 1);
        if (nl >= sizeof p->name)
            nl = sizeof p->name - 1;
        memcpy(p->name, lp + 1, nl);
        p->name[nl] = 0;
        p->state = state;
        p->threads = thr;
        p->rss_kb = rss > 0 ? (unsigned long)rss * (unsigned long)page_kb : 0;
        p->ticks = ut + st;
        p->cpu_pm = 0;
        for (int j = 0; j < nprev; j++) {
            if (prevp[j].pid == p->pid) {
                if (cpu_delta > 0 && p->ticks >= prevp[j].ticks) {
                    /* cpu_delta spans every core; scale so 1000 = one core. */
                    unsigned long long pm = (unsigned long long)(p->ticks - prevp[j].ticks)
                                          * 1000ULL * (unsigned long long)ncpu / cpu_delta;
                    unsigned long long cap = 1000ULL * (unsigned long long)ncpu;

                    p->cpu_pm = (int)(pm > cap ? cap : pm);
                }
                break;
            }
        }
        nprocs++;
    }
    closedir(dir);
    sort_procs();
}

void sample(void)
{
    long long now = now_us();

    sample_dt_us = last_sample_us ? now - last_sample_us : 0;
    last_sample_us = now;

    sample_cpu();
    sample_misc();
    sample_mem();
    sample_net();
    sample_disk();
    sample_fs();
    sample_procs();

    if (primed) {
        push(h_cpu, (unsigned int)cpu_pm);
        push(h_mem, (unsigned int)mem_used_pm);
        push(h_rx, (unsigned int)rx_rate);
        push(h_tx, (unsigned int)tx_rate);
        push(h_disk, (unsigned int)(rd_rate + wr_rate));
        for (int i = 0; i < ncore; i++)
            push(h_core + i * HIST, (unsigned int)core_pm[i]);
        if (h_count < HIST)
            h_count++;
    }
    primed = 1;
}

