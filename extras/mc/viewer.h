/*
 * viewer.h - the built-in file viewer (F3), modelled on mc's.
 *
 *   /path/to/file                     814/4295               18%   status
 *   text of the file ...                                            body
 *    1Help  2UnWrap 3Quit  4Hex  5Goto  6  7Search 8  9Format 10Quit
 *
 * Text is shown wrapped (F2 toggles), or as a hex dump (F4). F9 renders
 * Markdown: headings, emphasis, code, lists, quotes, rules, links and
 * tables. It is on by default for *.md files.
 */
#ifndef VIEWER_H
#define VIEWER_H

/* Show a file until the user quits. Returns -1 with errno set if it
 * cannot be opened (the caller reports the error). */
int view_file(const char *path);

#endif /* VIEWER_H */
