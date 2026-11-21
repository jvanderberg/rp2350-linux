/*
 * proto.h - the messages between the tm client and server.
 *
 * Both directions use frames over a Unix socket:
 *
 *   type (1 byte), length (2 bytes, big-endian), data (length bytes)
 *
 * client -> server   MSG_INPUT   keystrokes, passed on as they are
 *                    MSG_RESIZE  rows and cols, 2 bytes each
 * server -> client   MSG_OUTPUT  bytes for the terminal
 *                    MSG_DETACH  you are detached; exit
 *                    MSG_EXIT    the session has ended; exit
 */
#ifndef PROTO_H
#define PROTO_H

#include <stddef.h>

#define MSG_INPUT   'I'
#define MSG_RESIZE  'R'
#define MSG_OUTPUT  'O'
#define MSG_DETACH  'D'
#define MSG_EXIT    'X'

#define MSG_MAX     65535

/* Send one frame (blocking). Returns 0, or -1 if the peer is gone. */
int msg_send(int fd, int type, const void *data, size_t n);

/* Send any amount of data as MSG_OUTPUT frames. */
int msg_send_all(int fd, int type, const void *data, size_t n);

/* Take the first complete frame out of buf[0, *len): sets *type, *data,
 * *n and returns the frame's total size, or 0 if it isn't all there. */
size_t msg_parse(const unsigned char *buf, size_t len, int *type,
                 const unsigned char **data, size_t *n);

#endif /* PROTO_H */
