/*
 * nnn calls the wide-character curses functions (get_wch, unget_wch,
 * addwstr, addnwstr, mvaddnwstr), which only the ncursesw library has.
 * The device has the narrow libncurses, so these replacements sit on top
 * of getch/ungetch/addnstr. Input: UTF-8 bytes are decoded into one wide
 * character. Output: narrow curses would draw bytes above 0x7f as "M-x",
 * so each non-ASCII character is drawn as '?' repeated to its display
 * width, which keeps nnn's column arithmetic (wcswidth) right.
 */
#ifndef NARROWCURSES_H
#define NARROWCURSES_H

#include <wchar.h>
#include <string.h>

#undef get_wch
#undef unget_wch
#undef addwstr
#undef addnwstr
#undef mvaddnwstr

static int nc_get_wch(wint_t *wch)
{
	int c = getch();

	if (c == ERR)
		return ERR;
	if (c >= KEY_MIN) {
		*wch = (wint_t)c;
		return KEY_CODE_YES;
	}
	if (c >= 0xc0 && c <= 0xf7) {
		char buf[4];
		int need = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1, n = 1;
		wchar_t w;
		mbstate_t st;

		buf[0] = (char)c;
		while (n <= need) {
			int d = getch();

			if (d == ERR || (d & 0xc0) != 0x80) {
				if (d != ERR)
					ungetch(d);
				break;
			}
			buf[n++] = (char)d;
		}
		memset(&st, 0, sizeof(st));
		if (mbrtowc(&w, buf, (size_t)n, &st) == (size_t)n) {
			*wch = (wint_t)w;
			return OK;
		}
	}
	*wch = (wint_t)c;
	return OK;
}

static int nc_unget_wch(wint_t wch)
{
	char buf[MB_LEN_MAX];
	mbstate_t st;
	size_t n;

	if (wch < 0x80 || wch >= KEY_MIN)
		return ungetch((int)wch);
	memset(&st, 0, sizeof(st));
	n = wcrtomb(buf, (wchar_t)wch, &st);
	if (n == (size_t)-1)
		return ERR;
	while (n)
		ungetch((unsigned char)buf[--n]);
	return OK;
}

static int nc_addnwstr(const wchar_t *s, int n)
{
	char buf[256];
	int len = 0;

	for (; *s && n != 0; ++s, --n) {
		if (len > (int)sizeof(buf) - 4) {
			addnstr(buf, len);
			len = 0;
		}
		if (*s < 0x80) {
			buf[len++] = (char)*s;
		} else {
			int w = wcwidth(*s);

			while (w-- > 0)
				buf[len++] = '?';
		}
	}
	if (len)
		addnstr(buf, len);
	return OK;
}

#define get_wch(p)               nc_get_wch(p)
#define unget_wch(c)             nc_unget_wch(c)
#define addwstr(s)               nc_addnwstr((s), -1)
#define addnwstr(s, n)           nc_addnwstr((s), (n))
#define mvaddnwstr(y, x, s, n)   (move((y), (x)) == ERR ? ERR : nc_addnwstr((s), (n)))

#endif
