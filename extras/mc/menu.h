/*
 * menu.h - the top menu bar, its pull-down menus (F9), and the function
 * key bar at the bottom of the screen.
 *
 * Menus are static tables. Choosing an item yields its command number;
 * the application decides what that command does.
 */
#ifndef MENU_H
#define MENU_H

struct menu_item {
    const char *label;      /* '&' marks the hotkey; NULL = separator */
    const char *keys;       /* shortcut shown on the right, or NULL */
    int cmd;
};

struct menu {
    const char *title;      /* '&' marks the hotkey */
    const struct menu_item *items;
    int n;
};

/* Draw the bar on row 0. `open` is the index of the pulled-down menu
 * (highlighted), or -1 for the idle black-on-cyan bar. */
void menubar_draw(const struct menu *menus, int n, int open);

/* Run the menus modally, starting with menu *which pulled down. Returns
 * the chosen command (0 if cancelled) and leaves *which set to the menu
 * it came from, so "Left" and "Right" can act on their own panel. */
int menu_run(const struct menu *menus, int n, int *which);

/* Index of the menu whose title is at screen column x on row 0, or -1. */
int menubar_hit(const struct menu *menus, int n, int x);

/* The " 1Help  2Menu ..." bar on row y; labels[i] is for F(i+1). */
void keybar_draw(int y, const char *const labels[10]);

/* Which F key (1..10) the key bar shows at column x. */
int keybar_hit(int x);

#endif /* MENU_H */
