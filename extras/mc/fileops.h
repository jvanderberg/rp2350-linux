/*
 * fileops.h - copy, move, delete and mkdir, with mc's dialogs.
 *
 * Each operation works on the panel's selection (the marked files, or the
 * one under the cursor), asks for confirmation/target the way mc does,
 * shows a progress box while it runs and reports errors with Skip/Abort.
 * Esc during the progress box aborts. Files that were processed
 * successfully are unmarked. The caller rescans the panels afterwards.
 */
#ifndef FILEOPS_H
#define FILEOPS_H

#include "panel.h"

/* F5 / F6: copy or move the selection of `src`; the target defaults to
 * the directory shown in `dst`. Returns 1 if anything may have changed. */
int fop_copy(struct panel *src, struct panel *dst, int move);

/* F8: delete the selection, asking before recursing into directories. */
int fop_delete(struct panel *p);

/* F7: ask for a name and create it (with parents, like mkdir -p). The
 * panel is reloaded with the cursor on the new directory. */
int fop_mkdir(struct panel *p);

#endif /* FILEOPS_H */
