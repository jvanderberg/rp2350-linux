/*
 * keys.c - see keys.h.
 */
#include "keys.h"

int key_next(const unsigned char *s, size_t n, size_t *used)
{
    size_t i;
    int p = 0;

    *used = 1;
    if (s[0] == 8)
        return K_BS;
    if (s[0] == '\n')
        return K_ENTER;
    if (s[0] != 27 || n == 1)
        return s[0];

    if (s[1] == 'O' && n >= 3) {            /* SS3: ESC O x */
        *used = 3;
        switch (s[2]) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        }
        return K_IGNORE;
    }
    if (s[1] != '[')
        return K_ESC;                       /* Esc (and the next key) */

    /* CSI: ESC [ params final */
    for (i = 2; i < n; i++) {
        unsigned char c = s[i];

        if (c >= '0' && c <= '9') {
            p = p * 10 + (c - '0');
            continue;
        }
        if (c == ';' || c == '<' || c == '?') {
            p = 0;
            continue;
        }
        *used = i + 1;
        switch (c) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        case '~':
            switch (p) {
            case 1: case 7: return K_HOME;
            case 2: return K_INS;
            case 3: return K_DEL;
            case 4: case 8: return K_END;
            case 5: return K_PGUP;
            case 6: return K_PGDN;
            }
            return K_IGNORE;
        }
        return K_IGNORE;                    /* mouse reports etc. */
    }
    *used = n;                              /* cut off: drop it */
    return K_IGNORE;
}

int mouse_parse(const unsigned char *s, size_t n, struct mouse *m,
                size_t *used)
{
    int v[3] = { 0, 0, 0 }, k = 0;
    size_t i;

    /* A lone Esc (or Esc [) at the end of the input is a key, not the
     * start of a report: terminals send a report in one piece. */
    if (n < 3 || s[0] != 27 || s[1] != '[')
        return 0;
    if (s[2] == 'M') {                      /* X10: three bytes follow */
        if (n < 6)
            return -1;
        m->b = s[3] - 32;
        m->x = s[4] - 33 < 0 ? 0 : s[4] - 33;
        m->y = s[5] - 33 < 0 ? 0 : s[5] - 33;
        /* a release is "button 3" (no button); treat it as the left's */
        m->release = (m->b & 3) == 3 && !(m->b & (MB_MOTION | MB_WHEEL));
        if (m->release)
            m->b &= ~3;
        *used = 6;
        return 1;
    }
    if (s[2] != '<')
        return 0;
    i = 3;
    for (; i < n && i < 32; i++) {
        unsigned char c = s[i];

        if (c >= '0' && c <= '9') {
            v[k] = v[k] * 10 + (c - '0');
        } else if (c == ';' && k < 2) {
            k++;
        } else if ((c == 'M' || c == 'm') && k == 2) {
            m->b = v[0];
            m->x = v[1] > 0 ? v[1] - 1 : 0;
            m->y = v[2] > 0 ? v[2] - 1 : 0;
            m->release = c == 'm';
            *used = i + 1;
            return 1;
        } else {
            *used = i + 1;                  /* malformed: skip it */
            m->b = -1;
            return 1;
        }
    }
    if (i >= 32) {
        *used = i;
        m->b = -1;
        return 1;
    }
    return -1;
}
