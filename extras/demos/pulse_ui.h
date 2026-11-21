/*
 * pulse_ui.h - the seam between pulse's drawing code (pulse_draw.c) and its
 * input/main loop (pulse.c). Split in two so tcc fits in the board's RAM.
 *
 * term.h keeps its output buffer in static variables, so only pulse.c may
 * include it; pulse_draw.c writes through the out_* functions below.
 */
#ifndef PULSE_UI_H
#define PULSE_UI_H

#include <stddef.h>

extern int cols, rows;
extern const int intervals_ms[];
extern int interval_idx;

void render(void);
int proc_visible(int h);        /* process rows that fit in a panel h high */

void out_put(const char *s, size_t n);
void out_str(const char *s);
void out_goto(int row, int col);
void out_fg(int color);
void out_bg(int color);
void out_reset(void);
void out_flush(void);

#endif /* PULSE_UI_H */
