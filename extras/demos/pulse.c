/*
 * pulse.c - a colourful live system monitor for ANSI 256-colour terminals.
 *
 * Build:  ./build.sh pulse     (or see build.sh: three files, then link)
 * Keys:   up/down or k/j = select process   PgUp/PgDn = page
 *         s = sort (cpu, mem, pid, name)    r = reverse sort
 *         + / - = update interval           q = quit
 *
 * This file holds input and the main loop. Drawing lives in pulse_draw.c,
 * sampling in pulse_data.c.
 */
#include "term.h"
#include <ctype.h>
#include <poll.h>
#include <stdio.h>
#include "pulse.h"
#include "pulse_ui.h"

/* ---------- output hooks for pulse_draw.c (see pulse_ui.h) ---------- */

void out_put(const char *s, size_t n) { term_put(s, n); }
void out_str(const char *s) { term_str(s); }
void out_goto(int row, int col) { term_goto(row, col); }
void out_fg(int color) { term_fg(color); }
void out_bg(int color) { term_bg(color); }
void out_reset(void) { term_reset(); }
void out_flush(void) { term_flush(); }

/* ---------- UI state ---------- */

const int intervals_ms[] = { 250, 500, 1000, 2000, 5000 };
int interval_idx = 2;

int cols, rows;
static volatile sig_atomic_t resized;

/* ---------- input ---------- */

static int esc_state, esc_num;

static void move_sel(int d)
{
    sel += d;
    if (sel >= nprocs)
        sel = nprocs - 1;
    if (sel < 0)
        sel = 0;
    sel_pid = nprocs > 0 ? procs[sel].pid : -1;
}

/* Returns 1 if the screen needs redrawing. */
static int handle_input(void)
{
    int c, changed = 0;
    int page = proc_visible(rows - 1 - (rows >= 30 ? rows * 36 / 100 : 8));

    while ((c = term_read_byte()) >= 0) {
        if (esc_state == 1) {
            esc_state = (c == '[' || c == 'O') ? 2 : 0;
            esc_num = 0;
            continue;
        }
        if (esc_state == 2) {
            if (isdigit(c)) {
                esc_num = esc_num * 10 + (c - '0');
                continue;
            }
            esc_state = 0;
            if (c == 'A')
                move_sel(-1);
            else if (c == 'B')
                move_sel(1);
            else if (c == '~' && esc_num == 5)
                move_sel(-page);
            else if (c == '~' && esc_num == 6)
                move_sel(page);
            changed = 1;
            continue;
        }
        switch (c) {
        case 27:
            esc_state = 1;
            break;
        case 'k':
            move_sel(-1);
            changed = 1;
            break;
        case 'j':
            move_sel(1);
            changed = 1;
            break;
        case 's':
            sort_key = (sort_key + 1) % SORT_COUNT;
            sort_procs();
            changed = 1;
            break;
        case 'r':
            sort_rev = !sort_rev;
            sort_procs();
            changed = 1;
            break;
        case '+': case '=':
            if (interval_idx > 0)
                interval_idx--;
            changed = 1;
            break;
        case '-': case '_':
            if (interval_idx < (int)(sizeof intervals_ms / sizeof intervals_ms[0]) - 1)
                interval_idx++;
            changed = 1;
            break;
        case 'q': case 'Q': case 3:
            term_quit_flag = 1;
            break;
        default:
            break;
        }
    }
    return changed;
}

static void on_winch(int sig)
{
    (void)sig;
    resized = 1;
}

int main(void)
{
    struct sigaction sa;
    struct timespec ts = { 0, 300000000L };
    long long next;
    long pg = sysconf(_SC_PAGESIZE);

    if (pulse_init() != 0) {
        fprintf(stderr, "pulse: out of memory\n");
        return 1;
    }
    if (pg > 0)
        page_kb = pg / 1024;
    uname(&uts);
    term_size(&cols, &rows);

    sample();
    nanosleep(&ts, NULL);       /* a short first interval so rates show */
    sample();

    term_begin();
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);

    render();
    next = term_now_us() + intervals_ms[interval_idx] * 1000LL;

    while (!term_quit_flag) {
        struct pollfd pfd;
        long long now = term_now_us();
        int wait_ms = (int)((next - now + 999) / 1000);

        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll(&pfd, 1, wait_ms < 0 ? 0 : wait_ms);

        if (resized) {
            resized = 0;
            term_size(&cols, &rows);
            term_reset();
            term_str("\x1b[2J");
            render();
        }
        if (handle_input() && !term_quit_flag)
            render();

        now = term_now_us();
        if (now >= next) {
            sample();
            render();
            next = now + intervals_ms[interval_idx] * 1000LL;
        }
    }

    term_end();
    return 0;
}
