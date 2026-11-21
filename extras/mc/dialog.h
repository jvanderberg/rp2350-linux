/*
 * dialog.h - mc-style modal dialogs.
 *
 * A dialog is a grey (or, for errors and deletes, red) box with an inset
 * frame, a centred title and a drop shadow:
 *
 *    ┌──────── Create a new Directory ────────┐
 *    │ Enter directory name:                  │   W_LABEL
 *    │ [input field.......................]   │   W_INPUT
 *    ├────────────────────────────────────────┤   W_HLINE
 *    │          [< OK >] [ Cancel ]           │   W_BUTTON x2
 *    └────────────────────────────────────────┘
 *
 * Build one with dlg_init() and the dlg_* adders (positions are relative
 * to the dialog's top-left corner), then dlg_run() it. The helpers at the
 * bottom cover the common cases in one call.
 *
 * Open dialogs form a stack so that one shown on top of another (an
 * "overwrite?" question over a copy progress box) repaints correctly.
 */
#ifndef DIALOG_H
#define DIALOG_H

enum { W_LABEL, W_INPUT, W_BUTTON, W_HLINE, W_GAUGE, W_LIST };

/* Colour schemes (the `scheme` argument of dlg_init()). */
#define DLG_NORMAL  0   /* black on grey */
#define DLG_ERROR   1   /* white on red: errors, deletes */
#define DLG_MENU    2   /* white on cyan, like a menu: the user menu */

#define WF_DEFAULT  1   /* button: chosen by Enter, drawn as "[< OK >]" */
#define WF_CENTER   2   /* label: centred between the frame sides */

struct widget {
    int type;
    int y, x, w;            /* relative to the dialog's corner */
    const char *text;       /* label text or button caption; '&' marks
                               the hotkey letter of a button */
    int flags;
    int id;                 /* button: the value dlg_run() returns */
    char *buf;              /* input: the text being edited */
    int cap;                /* input: size of buf */
    int pos, scroll;        /* input: cursor and first shown byte */
    int changed;            /* input: edited yet? (else typing replaces) */
    int value;              /* gauge: 0..1000 */
    const char *const *items;   /* list: the rows (count in cap) */
    const char *hotkeys;    /* list: one hotkey character per row */
    int h;                  /* list: visible rows; pos = selection */
};

#define DLG_MAX_WIDGETS 24

struct dialog {
    const char *title;
    int y, x, h, w;         /* placed centred by dlg_run()/dlg_show() */
    int scheme;             /* DLG_NORMAL, DLG_ERROR or DLG_MENU */
    int n, focus;
    struct widget wd[DLG_MAX_WIDGETS];
};

/* Supplied by the application: paint the main screen underneath. */
void app_draw(void);

/* What dialogs are drawn over: app_draw() unless a full-screen mode such
 * as the viewer installs its own painter. Returns the previous one. */
typedef void (*dlg_painter)(void);
dlg_painter dlg_set_painter(dlg_painter paint);

void dlg_init(struct dialog *d, const char *title, int h, int w, int scheme);
struct widget *dlg_label(struct dialog *d, int y, const char *text,
                         int center);
struct widget *dlg_input(struct dialog *d, int y, char *buf, int cap);
struct widget *dlg_gauge(struct dialog *d, int y);
/* A scrolling list of `n` rows, `h` of them visible; Enter or a row's
 * hotkey ends dlg_run() with the list's id, the row is in ->pos. */
struct widget *dlg_list_box(struct dialog *d, int y, int h,
                            const char *const *items, const char *hotkeys,
                            int n);
void dlg_hline(struct dialog *d, int y);

/* A centred row of buttons, ids 0..n-1, `gap` spaces apart. */
void dlg_buttons(struct dialog *d, int y, int gap, const char *const *labels,
                 int n, int def);

/* Run modally. Returns the id of the button pressed (Enter in an input
 * field presses the default button), or -1 for Esc. */
int dlg_run(struct dialog *d);

/* Non-modal display for progress boxes: show, redraw, hide. */
void dlg_show(struct dialog *d);
void dlg_hide(struct dialog *d);
void dlg_paint(void);       /* app_draw() + every shown dialog, no flush */
void dlg_redraw(void);      /* dlg_paint() and send it to the terminal */

/* Message with buttons, e.g. {"&Yes", "&No"}; returns the index chosen
 * or -1. `msg` may hold several lines separated by '\n'. */
int dlg_query(const char *title, int error, int def, const char *msg,
              const char *const *buttons, int n);

/* Pick one of `items` from a list box (cyan, like mc's user menu).
 * hotkeys[i] selects row i directly. Returns the row, or -1. */
int dlg_list(const char *title, const char *const *items,
             const char *hotkeys, int n);

/* Error box with a single OK button; printf-style. */
void dlg_message(const char *title, int error, const char *fmt, ...);

/* One-line text entry with OK / Cancel. `prompt` may be NULL; width 0
 * picks a default. Returns 1 if confirmed. */
int dlg_input_box(const char *title, const char *prompt, char *buf, int cap,
                  int width);

#endif /* DIALOG_H */
