/*
 * client.c - the attached side of tm. See client.h.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "client.h"
#include "proto.h"

/* Put back everything the server may have switched on: main screen,
 * auto-wrap, cursor, colours, cursor keys and keypad, mouse, paste. */
#define RESTORE "\x1b[?1049l\x1b[?7h\x1b[?25h\x1b[0m\x1b[?1l\x1b>" \
                "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?2004l"

static volatile sig_atomic_t got_winch;

static void on_winch(int sig) { (void)sig; got_winch = 1; }

static int write_out(const unsigned char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(1, p, n);

        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int send_size(int fd)
{
    struct winsize ws;
    unsigned char b[4];
    int rows = 24, cols = 80;

    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
    b[0] = (unsigned char)(rows >> 8);
    b[1] = (unsigned char)rows;
    b[2] = (unsigned char)(cols >> 8);
    b[3] = (unsigned char)cols;
    return msg_send(fd, MSG_RESIZE, b, 4);
}

int client_run(int fd, const char *session)
{
    static unsigned char in[4096], buf[MSG_MAX + 3 + 4096];
    struct termios saved, raw;
    struct sigaction sa;
    size_t len = 0;
    int ended = -1;         /* MSG_DETACH, MSG_EXIT, or -1: connection lost */
    int have_tty = tcgetattr(0, &saved) == 0;

    if (have_tty) {
        raw = saved;
        cfmakeraw(&raw);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (send_size(fd) != 0)
        goto done;
    for (;;) {
        struct pollfd p[2];

        if (got_winch) {
            got_winch = 0;
            if (send_size(fd) != 0)
                break;
        }
        p[0].fd = 0;
        p[0].events = POLLIN;
        p[1].fd = fd;
        p[1].events = POLLIN;
        if (poll(p, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (p[0].revents & (POLLIN | POLLHUP)) {
            ssize_t r = read(0, in, sizeof in);

            if (r <= 0 && !(r < 0 && errno == EINTR))
                break;      /* our terminal went away: just leave */
            if (r > 0 && msg_send(fd, MSG_INPUT, in, (size_t)r) != 0)
                break;
        }
        if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t r = read(fd, buf + len, sizeof buf - len);
            const unsigned char *data;
            size_t off = 0, size, n;
            int type;

            if (r <= 0 && !(r < 0 && errno == EINTR))
                break;
            if (r > 0)
                len += (size_t)r;
            while ((size = msg_parse(buf + off, len - off, &type, &data,
                                     &n)) != 0) {
                off += size;
                if (type == MSG_OUTPUT)
                    write_out(data, n);
                else if (type == MSG_DETACH || type == MSG_EXIT)
                    ended = type;
            }
            memmove(buf, buf + off, len - off);
            len -= off;
            if (ended >= 0)
                break;
        }
    }
done:
    write_out((const unsigned char *)RESTORE, sizeof RESTORE - 1);
    if (have_tty)
        tcsetattr(0, TCSANOW, &saved);
    if (ended == MSG_DETACH)
        printf("[detached (from session %s)]\n", session);
    else if (ended == MSG_EXIT)
        printf("[exited]\n");
    else
        printf("[lost server]\n");
    return ended >= 0 ? 0 : 1;
}
