/*
 * usermenu.h - the F2 user menu: shell snippets picked from a list.
 *
 * The menu is read from the first of these that exists:
 *
 *   .mc.menu in the active panel's directory
 *   ~/.config/mc/menu
 *   ~/.mc/menu
 *
 * and otherwise a built-in default suited to the board. The format is
 * mc's (a subset of it):
 *
 *   # comment
 *   shell_patterns=1        conditions use shell wildcards (0: regexes)
 *
 *   + f *.c                 condition for the next entry (see below)
 *   c   Compile with tcc    hotkey, then the title
 *       tcc -o %b %f        indented lines: the shell commands
 *
 * Conditions are tests joined left to right by & and |, each optionally
 * negated with !:
 *
 *   f pattern   the current file name matches       (F: other panel)
 *   d pattern   the current directory matches       (D: other panel)
 *   t types     the current entry is one of: r regular file, d directory,
 *               l link, x executable, t "files are marked"  (T: other)
 *   x file      `file` is executable
 *
 * A line starting with '=' instead of '+' makes the entry the initially
 * selected one when its condition holds.
 *
 * Macros in commands (file names come shell-quoted):
 *
 *   %f %p  current file       %b  without its extension    %x  extension
 *   %d     current directory  %s  marked files, or the current one
 *   %t     marked files       %u  marked files, unmarked afterwards
 *   %F %D %S %T  the same for the other panel
 *   %{prompt}    ask for text  %%  a literal %
 */
#ifndef USERMENU_H
#define USERMENU_H

#include "panel.h"

/* Show the menu for panel `p` (`other` is the other one) and run the
 * entry chosen. Returns 1 if a command ran (panels may need a rescan). */
int usermenu_run(struct panel *p, struct panel *other);

#endif /* USERMENU_H */
