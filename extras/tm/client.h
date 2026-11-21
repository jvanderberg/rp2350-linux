/*
 * client.h - the attached side of tm: puts the terminal in raw mode,
 * sends keystrokes and size changes to the server, and copies its
 * output to the screen until the server detaches or exits.
 */
#ifndef CLIENT_H
#define CLIENT_H

/* Attach through an already connected socket. Returns the exit status
 * for the program: 0 when detached or the session ended normally. */
int client_run(int fd, const char *session);

#endif /* CLIENT_H */
