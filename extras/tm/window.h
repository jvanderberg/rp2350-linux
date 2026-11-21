/*
 * window.h - one window: a shell on a pseudo-terminal, its emulated
 * screen and scrollback, and its copy-mode state.
 */
#ifndef WINDOW_H
#define WINDOW_H

#include <sys/types.h>

#include "vt.h"

/* Copy mode views the window's history plus its current screen as one
 * list of lines: 0 is the oldest history line, and the screen's rows
 * follow the newest one. */
struct copy {
    int active;
    long top;               /* first line shown */
    long cy;                /* cursor line */
    int cx;                 /* cursor column */
    int selecting;
    long sy;                /* selection start */
    int sx;
    long dropped;           /* history lines dropped when last adjusted */
    char search[64];        /* last search text */
    int search_back;        /* last search went up */
};

struct window {
    struct window *next;
    int id;                 /* the number shown in the status line */
    int fd;                 /* pty master */
    pid_t pid;              /* the shell */
    struct vt vt;
    char name[32];
    int renamed;            /* named by the user: stop automatic naming */
    int bell;               /* a bell rang while it wasn't shown */
    int dead;
    struct copy cm;
};

#endif /* WINDOW_H */
