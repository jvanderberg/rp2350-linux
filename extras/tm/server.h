/*
 * server.h - the tm server: owns the windows and serves one client.
 *
 * The server runs in the background. It holds every window's shell (on a
 * pseudo-terminal), feeds their output through the terminal emulator,
 * and draws the active window plus a status line for the attached
 * client. Detaching just drops the client; the shells keep running until
 * they exit. The server exits when its last window closes.
 *
 * Keys, after the prefix Ctrl-B:
 *
 *   c  new window          n / p  next / previous     0-9  go to window
 *   l  last window         ,      rename window       &    kill window
 *   d  detach              [      copy mode           ]    paste
 *   ?  list keys           r      redraw              C-b  send C-b
 *   m  mouse on / off
 *
 * Mouse (on unless TM_MOUSE=0): click a window name in the status line
 * to switch to it, or roll the wheel there to cycle. In a window, the
 * wheel scrolls back through history (copy mode; scrolling to the bottom
 * leaves it) and a left drag selects text, copied on release - kept for
 * C-b ] and sent to the terminal's clipboard with OSC 52. Programs that
 * turn on mouse reporting (mc, vim) get the events instead, unless Shift
 * is held; full-screen programs without it (less) get arrow keys from
 * the wheel.
 */
#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>

struct server_config {
    const char *session;        /* name shown in the status line */
    const char *history_dir;    /* scrollback ring files, or NULL */
    size_t history_disk;        /* bytes per window on disk */
    size_t history_ram;         /* bytes per window in RAM (fallback) */
};

/* Run the server on an already listening socket; first window rows x
 * cols. Does not return. */
void server_run(int listen_fd, const char *socket_path,
                const struct server_config *cfg, int rows, int cols);

#endif /* SERVER_H */
