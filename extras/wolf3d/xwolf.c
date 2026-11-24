// SPDX-License-Identifier: MIT
/*
 * xwolf.c - the X11 back end for this Wolf4SDL port: the SDL calls the
 * engine makes (see SDL.h), and its screen, in a window on an X server.
 *
 * The engine draws at 320x200 into an 8-bit buffer; the window shows it at
 * a whole-number scale. Sending that as pixels would take 256 KB a frame,
 * so the screen goes to the X server in two ways:
 *
 *   2D   menus, title, status bar, intermissions: the pixels that changed
 *        since the last update go as runs of one colour, each a rectangle,
 *        and runs that line up on the rows below merge. Unchanged rows
 *        are found with one memcmp each.
 *   3D   the play view never reaches the buffer. Hooks in wl_draw.c and
 *        wl_scale.c hand this file what the raycaster found for each
 *        column, and the X server draws it from textures it keeps (see the
 *        3D part below). A frame of it is 5-8 KB.
 *
 * The red and gold flashes are a dither drawn over the shown frame, not a
 * palette change, which would mean sending everything again (XW_Tint).
 *
 * Frames are pipelined: the board draws the next frame while the last one
 * travels, and waits only for that one (x_fence, in x11.h).
 *
 * XW_STATS=1 prints the frame rate, bytes and time per frame at the end;
 * XW_SOFT=1 draws the 3D view with the engine's own renderer instead, as
 * pixels (slow; for comparing).
 *
 * x11.h keeps its connection in file statics, so it is included here only.
 */
#include "wl_def.h"
#define X11_TIMING
#include "x11.h"
#include <time.h>

#define XW_W 320                        /* the engine's screen */
#define XW_H 200

#ifndef XW_SCALE
#define XW_SCALE 2                      /* the window's first size: 320x200 times this */
#endif

/* The picture is drawn at a whole-number scale, centred in the window;
 * xw_layout picks the largest that fits whenever the window is resized. */
static int xw_s = XW_SCALE, xw_ox, xw_oy;
#define S xw_s
static void xw_layout (int w, int h);
#define XW_BYTES() ((unsigned long)(x_sent + xi_out_len))   /* sent or queued */

/* ======================================================================
 * SDL: time, errors
 * ====================================================================== */

static long long xw_t0;

static long long xw_now_ms (void)
{
    struct timespec ts;

    clock_gettime (CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int SDL_Init (Uint32 flags)
{
    extern int xw3d_on;

    (void)flags;
    xw_t0 = xw_now_ms ();
    if (getenv ("XW_SOFT"))
        xw3d_on = 0;
    return 0;
}
static unsigned long xw_frames3d, xw_bytes3d, xw_t3d;
static unsigned long xw_bwall, xw_bsprite, xw_bweapon, xw_bbuild, xw_nblits;
/* microseconds over the timed 3D frames: */
static unsigned long long xw_us_walls, xw_us_sprites, xw_us_2d, xw_us_sync, xw_us_write, xw_us_all;
static unsigned long long xw_acc_walls, xw_acc_sprites;   /* this frame, so far */
static unsigned long xw_us_worst, xw_hitches;               /* slowest frame; frames over 40 ms */
static unsigned long xw_builds;

void SDL_Quit (void)
{
    if (getenv ("XW_STATS") && xw_frames3d)
    {
        fprintf (stderr, "xwolf: %lu 3D frames, %lu bytes each, %.1f fps, %lu textures built\n",
                 xw_frames3d, xw_bytes3d / xw_frames3d,
                 xw_frames3d * 1000.0 / (xw_t3d ? xw_t3d : 1), xw_builds);
        fprintf (stderr, "  per frame: walls %lu (%lu blits), sprites %lu, weapon %lu; building %lu\n",
                 xw_bwall / xw_frames3d, xw_nblits / xw_frames3d, xw_bsprite / xw_frames3d,
                 xw_bweapon / xw_frames3d, xw_bbuild / xw_frames3d);
        {
            double n = (double)xw_frames3d * 1000.0;
            double all = xw_us_all / n, w = xw_us_walls / n, sp = xw_us_sprites / n;
            double d2 = xw_us_2d / n, sy = xw_us_sync / n, wr = xw_us_write / n;

            fprintf (stderr, "  slowest frame %.1f ms; %lu over 40 ms\n", xw_us_worst / 1000.0, xw_hitches);
            fprintf (stderr, "  ms per frame: %.1f = engine %.1f + walls %.1f + sprites %.1f"
                     " + 2D %.1f + wait %.1f (write() inside these: %.1f)\n",
                     all, all - w - sp - d2 - sy, w, sp, d2, sy, wr);
        }
    }
    x_close ();
}
const char *SDL_GetError (void) { return "(no detail)"; }
Uint32 SDL_GetTicks (void) { return (Uint32)(xw_now_ms () - xw_t0); }
int SDL_SetHint (const char *name, const char *value) { (void)name; (void)value; return 1; }

void SDL_Delay (Uint32 ms)
{
    struct timespec ts;

    x_flush ();
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep (&ts, NULL);
}

int SDL_ShowSimpleMessageBox (Uint32 flags, const char *title, const char *message, SDL_Window *w)
{
    (void)flags;
    (void)w;
    fprintf (stderr, "%s: %s\n", title, message);
    return 0;
}

/* ======================================================================
 * SDL: surfaces
 * ====================================================================== */

SDL_Surface *SDL_CreateRGBSurface (Uint32 flags, int w, int h, int depth,
                                   Uint32 rmask, Uint32 gmask, Uint32 bmask, Uint32 amask)
{
    SDL_Surface *s = calloc (1, sizeof *s);
    int bpp = depth <= 8 ? 1 : 4;

    (void)flags;
    if (s == NULL || (s->format = calloc (1, sizeof *s->format)) == NULL)
        return NULL;
    s->format->BitsPerPixel = (Uint8)(bpp * 8);
    s->format->BytesPerPixel = (Uint8)bpp;
    s->format->Rmask = rmask;
    s->format->Gmask = gmask;
    s->format->Bmask = bmask;
    s->format->Amask = amask;
    if (bpp == 1 && (s->format->palette = calloc (1, sizeof *s->format->palette)) == NULL)
        return NULL;
    if (bpp == 1)
        s->format->palette->ncolors = 256;
    s->w = w;
    s->h = h;
    s->pitch = w * bpp;
    s->pixels = calloc ((size_t)h, (size_t)s->pitch);
    return s->pixels != NULL ? s : NULL;
}

void SDL_FreeSurface (SDL_Surface *s)
{
    if (s == NULL)
        return;
    if (s->format != NULL)
        free (s->format->palette);
    free (s->format);
    free (s->pixels);
    free (s);
}

int SDL_SetPaletteColors (SDL_Palette *p, const SDL_Color *colors, int first, int n)
{
    if (p == NULL || first < 0 || first + n > 256)
        return -1;
    memcpy (p->colors + first, colors, (size_t)n * sizeof *colors);
    return 0;
}

/* Clip r to the surface; returns 0 if nothing is left. */
static int xw_clip (const SDL_Surface *s, SDL_Rect *r)
{
    if (r->x < 0) { r->w += r->x; r->x = 0; }
    if (r->y < 0) { r->h += r->y; r->y = 0; }
    if (r->x + r->w > s->w) r->w = s->w - r->x;
    if (r->y + r->h > s->h) r->h = s->h - r->y;
    return r->w > 0 && r->h > 0;
}

int SDL_FillRect (SDL_Surface *s, const SDL_Rect *rect, Uint32 color)
{
    SDL_Rect r = rect ? *rect : (SDL_Rect){ 0, 0, s->w, s->h };

    if (!xw_clip (s, &r))
        return 0;
    for (int y = r.y; y < r.y + r.h; y++) {
        byte *row = (byte *)s->pixels + (size_t)y * s->pitch;

        if (s->format->BytesPerPixel == 1) {
            memset (row + r.x, (int)color, (size_t)r.w);
        } else {
            for (int x = r.x; x < r.x + r.w; x++)
                ((Uint32 *)row)[x] = color;
        }
    }
    return 0;
}

/* 8-bit to 8-bit copies, which is all the engine blits. */
int SDL_BlitSurface (SDL_Surface *src, const SDL_Rect *sr, SDL_Surface *dst, SDL_Rect *dr)
{
    SDL_Rect a = sr ? *sr : (SDL_Rect){ 0, 0, src->w, src->h };
    SDL_Rect b = { dr ? dr->x : 0, dr ? dr->y : 0, a.w, a.h };

    if (src->format->BytesPerPixel != 1 || dst->format->BytesPerPixel != 1)
        return -1;
    if (!xw_clip (src, &a))
        return 0;
    b.w = a.w;
    b.h = a.h;
    if (!xw_clip (dst, &b))
        return 0;
    for (int y = 0; y < b.h; y++)
        memmove ((byte *)dst->pixels + (size_t)(b.y + y) * dst->pitch + b.x,
                 (byte *)src->pixels + (size_t)(a.y + y) * src->pitch + a.x, (size_t)b.w);
    return 0;
}

int SDL_LockSurface (SDL_Surface *s) { (void)s; return 0; }
void SDL_UnlockSurface (SDL_Surface *s) { (void)s; }

Uint32 SDL_MapRGBA (const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    (void)f;
    return (Uint32)a << 24 | (Uint32)r << 16 | (Uint32)g << 8 | b;
}

int SDL_PixelFormatEnumToMasks (Uint32 format, int *bpp, Uint32 *r, Uint32 *g, Uint32 *b, Uint32 *a)
{
    (void)format;
    *bpp = 32;
    *r = 0xff0000;
    *g = 0xff00;
    *b = 0xff;
    *a = 0xff000000u;
    return 1;
}

int SDL_SaveBMP (SDL_Surface *s, const char *file) { (void)s; (void)file; return -1; }

/* ======================================================================
 * SDL: window, renderer, texture. The window is opened here; the rest
 * is xwolf's own drawing, so these are placeholders.
 * ====================================================================== */

static char xw_dummy;

SDL_Window *SDL_CreateWindow (const char *title, int x, int y, int w, int h, Uint32 flags)
{
    (void)x;
    (void)y;
    (void)flags;
    if (w != XW_W || h != XW_H) {       /* --res: resize the window instead */
        fprintf (stderr, "xwolf draws at %dx%d only; make the window bigger instead\n", XW_W, XW_H);
        exit (1);
    }
    if (x_open (NULL) < 0)
        exit (1);
    x_window (title, w * S, h * S);
    return (SDL_Window *)&xw_dummy;
}

void SDL_DestroyWindow (SDL_Window *w) { (void)w; }
SDL_Renderer *SDL_CreateRenderer (SDL_Window *w, int i, Uint32 f) { (void)w; (void)i; (void)f; return (SDL_Renderer *)&xw_dummy; }
void SDL_DestroyRenderer (SDL_Renderer *r) { (void)r; }
int SDL_SetRenderDrawBlendMode (SDL_Renderer *r, int m) { (void)r; (void)m; return 0; }
SDL_Texture *SDL_CreateTexture (SDL_Renderer *r, Uint32 f, int a, int w, int h)
{
    (void)r; (void)f; (void)a; (void)w; (void)h;
    return (SDL_Texture *)&xw_dummy;
}
void SDL_DestroyTexture (SDL_Texture *t) { (void)t; }
int SDL_UpdateTexture (SDL_Texture *t, const SDL_Rect *r, const void *p, int pitch)
{
    (void)t; (void)r; (void)p; (void)pitch;
    return 0;
}
int SDL_RenderCopy (SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *a, const SDL_Rect *b)
{
    (void)r; (void)t; (void)a; (void)b;
    return 0;
}
void SDL_RenderPresent (SDL_Renderer *r) { (void)r; }
int SDL_ShowCursor (int toggle) { (void)toggle; return 0; }
void SDL_SetWindowGrab (SDL_Window *w, int g) { (void)w; (void)g; }
int SDL_SetRelativeMouseMode (int e) { (void)e; return 0; }
void SDL_WarpMouseInWindow (SDL_Window *w, int x, int y) { (void)w; (void)x; (void)y; }

/* ======================================================================
 * SDL: input
 * ====================================================================== */

static SDL_Event xw_queue[256];
static int xw_qhead, xw_qlen;
static Uint32 xw_buttons;
static int xw_mouse_x, xw_mouse_y;

static void xw_push (const SDL_Event *e)
{
    if (xw_qlen < 256)
        xw_queue[(xw_qhead + xw_qlen++) % 256] = *e;
}

static int xw_scancode (int k)
{
    static const char shifted[] = "!@#$%^&*()";
    const char *p;

    if (k >= 'a' && k <= 'z') return SDL_SCANCODE_A + k - 'a';
    if (k >= 'A' && k <= 'Z') return SDL_SCANCODE_A + k - 'A';
    if (k >= '1' && k <= '9') return SDL_SCANCODE_1 + k - '1';
    if (k == '0') return SDL_SCANCODE_0;
    if (k > 0 && k < 127 && (p = strchr (shifted, k)) != NULL)
        return p - shifted == 9 ? SDL_SCANCODE_0 : SDL_SCANCODE_1 + (int)(p - shifted);
    if (k >= X_KEY_F1 && k < X_KEY_F1 + 12) return SDL_SCANCODE_F1 + k - X_KEY_F1;
    switch (k) {
    case ' ':  return SDL_SCANCODE_SPACE;
    case '-': case '_': return SDL_SCANCODE_MINUS;
    case '=': case '+': return SDL_SCANCODE_EQUALS;
    case '[': case '{': return SDL_SCANCODE_LEFTBRACKET;
    case ']': case '}': return SDL_SCANCODE_RIGHTBRACKET;
    case '\\': case '|': return SDL_SCANCODE_BACKSLASH;
    case ';': case ':': return SDL_SCANCODE_SEMICOLON;
    case '\'': case '"': return SDL_SCANCODE_APOSTROPHE;
    case '`': case '~': return SDL_SCANCODE_GRAVE;
    case ',': case '<': return SDL_SCANCODE_COMMA;
    case '.': case '>': return SDL_SCANCODE_PERIOD;
    case '/': case '?': return SDL_SCANCODE_SLASH;
    case X_KEY_RETURN: case 0xff8d: return SDL_SCANCODE_RETURN;   /* Return, KP_Enter */
    case X_KEY_ESCAPE:    return SDL_SCANCODE_ESCAPE;
    case X_KEY_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
    case X_KEY_TAB:       return SDL_SCANCODE_TAB;
    case X_KEY_LEFT:      return SDL_SCANCODE_LEFT;
    case X_KEY_RIGHT:     return SDL_SCANCODE_RIGHT;
    case X_KEY_UP:        return SDL_SCANCODE_UP;
    case X_KEY_DOWN:      return SDL_SCANCODE_DOWN;
    case X_KEY_HOME:      return SDL_SCANCODE_HOME;
    case X_KEY_END:       return SDL_SCANCODE_END;
    case X_KEY_PAGE_UP:   return SDL_SCANCODE_PAGEUP;
    case X_KEY_PAGE_DOWN: return SDL_SCANCODE_PAGEDOWN;
    case X_KEY_DELETE:    return SDL_SCANCODE_DELETE;
    case 0xff63:          return SDL_SCANCODE_INSERT;
    case 0xff13:          return SDL_SCANCODE_PAUSE;
    case 0xff14:          return SDL_SCANCODE_SCROLLLOCK;
    case 0xffe5:          return SDL_SCANCODE_CAPSLOCK;
    case X_KEY_SHIFT_L:   return SDL_SCANCODE_LSHIFT;
    case X_KEY_SHIFT_R:   return SDL_SCANCODE_RSHIFT;
    case X_KEY_CONTROL_L: return SDL_SCANCODE_LCTRL;
    case X_KEY_CONTROL_R: return SDL_SCANCODE_RCTRL;
    case 0xffe9: case 0xff7e: return SDL_SCANCODE_LALT;  /* Alt_L; Mode_switch: a Mac's Option */
    case 0xffea:          return SDL_SCANCODE_RALT;
    case 0xffe7: case 0xffeb: return SDL_SCANCODE_LGUI;  /* Meta_L, Super_L: Command */
    }
    return SDL_SCANCODE_UNKNOWN;
}

static void xw_pump (void)
{
    struct x_event e;

    while (x_event (&e)) {
        SDL_Event s;

        memset (&s, 0, sizeof s);
        switch (e.type) {
        case XE_CLOSE:
            s.type = SDL_QUIT;
            xw_push (&s);
            break;
        case XE_KEY_DOWN:
        case XE_KEY_UP:
            s.type = e.type == XE_KEY_DOWN ? SDL_KEYDOWN : SDL_KEYUP;
            s.key.keysym.scancode = xw_scancode (e.key);
            s.key.keysym.sym = e.key;
            if (s.key.keysym.scancode != SDL_SCANCODE_UNKNOWN)
                xw_push (&s);
            if (e.type == XE_KEY_DOWN && e.key >= 32 && e.key < 127) {
                memset (&s, 0, sizeof s);
                s.type = SDL_TEXTINPUT;
                s.text.text[0] = (char)e.key;
                xw_push (&s);
            }
            break;
        case XE_BUTTON_DOWN:
        case XE_BUTTON_UP:
            if (e.button >= 1 && e.button <= 3) {
                if (e.type == XE_BUTTON_DOWN)
                    xw_buttons |= SDL_BUTTON (e.button);
                else
                    xw_buttons &= ~(Uint32)SDL_BUTTON (e.button);
            }
            /* fall through */
        case XE_MOTION:
            xw_mouse_x = (e.x - xw_ox) / S;
            xw_mouse_y = (e.y - xw_oy) / S;
            break;
        case XE_RESIZE:
            xw_layout (e.x, e.y);
            break;
        }
    }
}

int SDL_PollEvent (SDL_Event *e)
{
    xw_pump ();
    if (xw_qlen == 0)
        return 0;
    if (e != NULL) {
        *e = xw_queue[xw_qhead];
        xw_qhead = (xw_qhead + 1) % 256;
        xw_qlen--;
    }
    return 1;
}

int SDL_WaitEvent (SDL_Event *e)
{
    for (;;) {
        if (SDL_PollEvent (e))
            return 1;
        x_wait (50);
    }
}

Uint8 SDL_EventState (Uint32 type, int state) { (void)type; (void)state; return 0; }

Uint32 SDL_GetMouseState (int *x, int *y)
{
    xw_pump ();
    if (x) *x = xw_mouse_x;
    if (y) *y = xw_mouse_y;
    return xw_buttons;
}

Uint32 SDL_GetRelativeMouseState (int *x, int *y)
{
    if (x) *x = 0;
    if (y) *y = 0;
    return xw_buttons;
}

int SDL_GetModState (void) { return KMOD_NUM; }

int SDL_NumJoysticks (void) { return 0; }
SDL_Joystick *SDL_JoystickOpen (int i) { (void)i; return NULL; }
void SDL_JoystickClose (SDL_Joystick *j) { (void)j; }
void SDL_JoystickUpdate (void) {}
int SDL_JoystickNumButtons (SDL_Joystick *j) { (void)j; return 0; }
int SDL_JoystickNumHats (SDL_Joystick *j) { (void)j; return 0; }
Sint16 SDL_JoystickGetAxis (SDL_Joystick *j, int a) { (void)j; (void)a; return 0; }
Uint8 SDL_JoystickGetHat (SDL_Joystick *j, int h) { (void)j; (void)h; return 0; }
Uint8 SDL_JoystickGetButton (SDL_Joystick *j, int b) { (void)j; (void)b; return 0; }

/* ======================================================================
 * The 2D path
 * ====================================================================== */

#define XW_RECTS 2048

static Uint32 xw_pal[256];              /* 32-bit pixel per palette index */
static byte   xw_canon[256];            /* first index with the same pixel */
static byte   xw_stale[256];            /* the index's pixel changed this update */
static byte   xw_shown[XW_W * XW_H];    /* what the window shows, as a palette index */
static byte   xw_knownbits[XW_W * XW_H / 8];   /* clear: unknown, send it */
#define XW_KNOWN(i)   (xw_knownbits[(i) >> 3] >> ((i) & 7) & 1)
#define XW_SET(i)     (xw_knownbits[(i) >> 3] |= (byte)(1 << ((i) & 7)))
#define XW_FORGET(i)  (xw_knownbits[(i) >> 3] &= (byte)~(1 << ((i) & 7)))
static short  xw_rect[XW_RECTS][4];
static byte   xw_rect_col[XW_RECTS];
static int    xw_nrect;

static int    xw_open_row[2][XW_W];

/* The region the 3D renderer owns while it runs: the 2D path leaves it alone. */
static int xw_own_x, xw_own_y, xw_own_w, xw_own_h, xw_own_on;

static void xw_flush_rects_pal (const Uint32 *pal)
{
    static short sorted[XW_RECTS][4];
    int count[257] = { 0 }, pos[256];

    for (int i = 0; i < xw_nrect; i++)
        count[xw_rect_col[i] + 1]++;
    for (int c = 0; c < 256; c++)
        count[c + 1] += count[c];
    memcpy (pos, count, sizeof pos);
    for (int i = 0; i < xw_nrect; i++)
        memcpy (sorted[pos[xw_rect_col[i]]++], xw_rect[i], sizeof sorted[0]);
    for (int c = 0; c < 256; c++)
        if (count[c + 1] > count[c]) {
            x_color (pal[c]);
            x_fill_rects (&sorted[count[c]][0], count[c + 1] - count[c]);
        }
    xw_nrect = 0;
    for (int x = 0; x < XW_W; x++)
        xw_open_row[0][x] = xw_open_row[1][x] = -1;
}

static void xw_flush_rects (void) { xw_flush_rects_pal (xw_pal); }

/*
 * The buffer's view area as it was at the last 3D frame. The engine's
 * buffer never holds the 3D view (xwolf draws it), so while this still
 * matches, the window's view is right as it is. A pixel that differs is
 * something the engine drew over the view since (PAUSED, say), and is
 * sent; when most of it differs, the view is handed back to the buffer.
 */
static byte xw_snap[XW_W * XW_H];

static byte xw_row_clean[XW_H];         /* every pixel outside the 3D view is known */
static int  xw_open_y[2] = { -1, -1 };  /* the row xw_open_row[i] belongs to */
static int  xw_overlay;                 /* pixels over the 3D view have been sent */

/* Forget what the window shows in a rectangle, so it is sent again. */
static void xw_forget (int x0, int y0, int w, int h)
{
    for (int y = y0; y < y0 + h && y < XW_H; y++) {
        for (int x = x0; x < x0 + w && x < XW_W; x++)
            XW_FORGET (y * XW_W + x);
        xw_row_clean[y] = 0;
    }
}

/* Send row y's pixels [a, b) that differ from what the window shows; with
 * snap, pixels still equal to it belong to the 3D view and are left alone. */
static void xw_send_span (const byte *row, int y, int a, int b, const byte *snap)
{
    byte *shown = xw_shown + y * XW_W;
    int base = y * XW_W, first = -1, last = -1, cur = y & 1, prev = cur ^ 1;

    for (int x = a; x < b; x++) {
        if (snap != NULL && row[x] == snap[x])
            continue;
        if (!XW_KNOWN (base + x) || xw_stale[shown[x]] || xw_pal[shown[x]] != xw_pal[row[x]]) {
            if (first < 0)
                first = x;
            last = x;
        }
    }
    if (first < 0)
        return;
    if (xw_open_y[cur] != y) {          /* first run on this row: start its list */
        memset (xw_open_row[cur], 0xff, sizeof xw_open_row[cur]);
        xw_open_y[cur] = y;
    }
    for (int x = first; x <= last;) {
        byte col = xw_canon[row[x]];
        int x0 = x, idx;

        while (x <= last && xw_canon[row[x]] == col && !(snap != NULL && row[x] == snap[x]))
            x++;
        if (x == x0) {                  /* a pixel of the 3D view */
            x++;
            continue;
        }
        for (int i = x0; i < x; i++) {
            shown[i] = row[i];
            XW_SET (base + i);
        }
        if (snap != NULL)
            xw_overlay = 1;
        idx = xw_open_y[prev] == y - 1 ? xw_open_row[prev][x0] : -1;
        if (idx >= 0 && xw_rect[idx][2] == (x - x0) * S && xw_rect_col[idx] == col) {
            xw_rect[idx][3] += S;       /* the same run as the row above: grow it */
        } else {
            if (xw_nrect == XW_RECTS) {
                xw_flush_rects ();
                memset (xw_open_row[cur], 0xff, sizeof xw_open_row[cur]);
                xw_open_y[cur] = y;
            }
            idx = xw_nrect++;
            xw_rect[idx][0] = (short)(xw_ox + x0 * S);
            xw_rect[idx][1] = (short)(xw_oy + y * S);
            xw_rect[idx][2] = (short)((x - x0) * S);
            xw_rect[idx][3] = (short)S;
            xw_rect_col[idx] = col;
        }
        xw_open_row[cur][x0] = (short)idx;
    }
}

/* Send what changed in the 8-bit buffer s to the back buffer. Most rows
 * haven't changed at all, which one memcmp per row finds out. */
static void xw_update_2d (SDL_Surface *s, int frame3d)
{
    static SDL_Color last[256];
    static int have_last;
    const SDL_Color *c = s->format->palette->colors;
    int stale = 0;

    /* A pixel shown as index i is still right if i's colour is unchanged
     * and the buffer's index there has the same colour. */
    if (!have_last || memcmp (last, c, sizeof last) != 0) {
        for (int i = 0; i < 256; i++) {
            Uint32 p = x_rgb (c[i].r, c[i].g, c[i].b);

            xw_stale[i] = have_last && p != xw_pal[i];
            stale |= xw_stale[i];
            xw_pal[i] = p;
            xw_canon[i] = (byte)i;
            for (int j = 0; j < i; j++)
                if (xw_pal[j] == p) {
                    xw_canon[i] = (byte)j;
                    break;
                }
        }
        memcpy (last, c, sizeof last);
        have_last = 1;
    } else {
        memset (xw_stale, 0, sizeof xw_stale);
    }
    xw_open_y[0] = xw_open_y[1] = -1;
    for (int y = 0; y < XW_H && y < s->h; y++) {
        const byte *row = (const byte *)s->pixels + (size_t)y * s->pitch;
        const byte *shown = xw_shown + y * XW_W;
        int oa = XW_W, ob = XW_W;       /* the 3D view's part of the row */

        if (xw_own_on && y >= xw_own_y && y < xw_own_y + xw_own_h) {
            oa = xw_own_x;
            ob = xw_own_x + xw_own_w;
        }
        if (stale || !xw_row_clean[y] || memcmp (row, shown, (size_t)oa) != 0)
            xw_send_span (row, y, 0, oa, NULL);
        if (ob < XW_W && (stale || !xw_row_clean[y] || memcmp (row + ob, shown + ob, (size_t)(XW_W - ob)) != 0))
            xw_send_span (row, y, ob, XW_W, NULL);
        if (oa < ob && !frame3d && memcmp (row + oa, xw_snap + y * XW_W + oa, (size_t)(ob - oa)) != 0)
            xw_send_span (row, y, oa, ob, xw_snap + y * XW_W);
        xw_row_clean[y] = 1;
    }
    xw_flush_rects ();
}

/* VH_UpdateScreen lands here: show the frame. */
static int xw_3d_frame;

/* ---- tints: the red and gold flashes ---- */

static int xw_tint_level;               /* 0..16: how many of 16 pixels */
static Uint32 xw_tint_pixel;
static unsigned xw_tint_masks[17];

void XW_Tint (int r, int g, int b, int num, int den)
{
    xw_tint_level = den > 0 ? (16 * num + den / 2) / den : 0;
    if (num > 0 && xw_tint_level == 0)
        xw_tint_level = 1;
    if (xw_tint_level > 16)
        xw_tint_level = 16;
    xw_tint_pixel = x_rgb (r, g, b);
}

/* Over the frame just shown: a 4x4 ordered dither of the tint colour. */
static void xw_draw_tint (void)
{
    static const byte bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    int n = xw_tint_level;

    if (n <= 0)
        return;
    if (xw_tint_masks[n] == 0) {
        short dots[16][4];
        int k = 0;

        xw_tint_masks[n] = x_mask (4, 4);
        for (int i = 0; i < 16; i++)
            if (bayer[i] < n) {
                dots[k][0] = (short)(i % 4);
                dots[k][1] = (short)(i / 4);
                dots[k][2] = dots[k][3] = 1;
                k++;
            }
        x_mask_rects (xw_tint_masks[n], &dots[0][0], k);
    }
    x_target_window ();
    x_color (xw_tint_pixel);
    x_fill_stippled (xw_tint_masks[n], xw_ox, xw_oy, XW_W * S, XW_H * S);
    x_target (0);
}

void XW_Update (SDL_Surface *s)
{
    static unsigned long sent0;
    static Uint32 t0;
    static unsigned long long u_frame0, write0;
    unsigned long long u_2d = xi_us (), u_sync;
    const byte *px = s->pixels;
    int was3d = xw_3d_frame;

    if (xw_3d_frame) {                  /* the window's view is the 3D view */
        if (xw_own_on && (xw_own_x != viewscreenx || xw_own_y != viewscreeny ||
                          xw_own_w != viewwidth || xw_own_h != viewheight))
            xw_forget (xw_own_x, xw_own_y, xw_own_w, xw_own_h);   /* the view moved */
        else if (xw_overlay)            /* the 3D frame has painted over them */
            xw_forget (xw_own_x, xw_own_y, xw_own_w, xw_own_h);
        /* The snapshot is taken when the view is first owned; the engine
         * doesn't draw into its buffer's view between 3D frames, and a 3D
         * frame covers whatever it might have, so it isn't checked again
         * (reading it every frame is slow in the board's memory). */
        if (!xw_own_on || xw_overlay || xw_own_x != viewscreenx || xw_own_y != viewscreeny ||
            xw_own_w != viewwidth || xw_own_h != viewheight)
            for (int y = viewscreeny; y < viewscreeny + viewheight; y++)
                memcpy (xw_snap + y * XW_W + viewscreenx, px + (size_t)y * s->pitch + viewscreenx,
                        (size_t)viewwidth);
        xw_overlay = 0;
        xw_own_on = 1;
        xw_own_x = viewscreenx;
        xw_own_y = viewscreeny;
        xw_own_w = viewwidth;
        xw_own_h = viewheight;
    } else if (xw_own_on) {
        long changed = 0;

        for (int y = xw_own_y; y < xw_own_y + xw_own_h; y++) {
            const byte *r = px + (size_t)y * s->pitch, *n = xw_snap + y * XW_W;

            if (memcmp (r + xw_own_x, n + xw_own_x, (size_t)xw_own_w) == 0)
                continue;
            for (int x = xw_own_x; x < xw_own_x + xw_own_w; x++)
                changed += r[x] != n[x];
        }
        if (changed * 2 > (long)xw_own_w * xw_own_h) {
            xw_own_on = 0;              /* a new picture: repaint it all */
            xw_forget (xw_own_x, xw_own_y, xw_own_w, xw_own_h);
        }
    }
    xw_3d_frame = 0;
    xw_update_2d (s, was3d);
    u_sync = xi_us ();
    x_fence_wait ();                    /* the previous frame is on screen */
    x_present ();
    xw_draw_tint ();
    x_fence ();                         /* this one is on its way */
    xw_pump ();
    if (was3d && t0) {                  /* from the end of one 3D frame to the next */
        unsigned long long now = xi_us ();

        xw_frames3d++;
        xw_bytes3d += XW_BYTES() - sent0;
        xw_t3d += SDL_GetTicks () - t0;
        xw_us_all += now - u_frame0;
        if (now - u_frame0 > xw_us_worst)
            xw_us_worst = (unsigned long)(now - u_frame0);
        xw_hitches += now - u_frame0 > 40000;
        xw_us_walls += xw_acc_walls;
        xw_us_sprites += xw_acc_sprites;
        xw_us_2d += u_sync - u_2d;
        xw_us_sync += now - u_sync;
        xw_us_write += x_write_us - write0;
    }
    sent0 = XW_BYTES();
    t0 = was3d ? SDL_GetTicks () : 0;
    u_frame0 = xi_us ();
    write0 = x_write_us;
    xw_acc_walls = xw_acc_sprites = 0;
}

/* FizzleFade lands here: the new picture appears in one go. */
void XW_Fizzle (SDL_Surface *source, int x, int y, unsigned width, unsigned height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    XW_Update (source);
}

/* ======================================================================
 * The 3D path
 *
 * The engine's raycaster still decides what each view column shows; the
 * hooks in wl_draw.c and wl_scale.c hand that over here instead of drawing
 * it into the 8-bit buffer, and the X server draws it:
 *
 *   ceiling, floor  a rectangle each
 *   walls           each texture is uploaded once (64x64), widened to the
 *                   window scale on the server, and kept scaled to every
 *                   height it has been seen at; a view column is then one
 *                   CopyArea from that, merged with its neighbours when they
 *                   continue the same texture
 *   sprites         the weapon, and things seen at the same size two
 *                   frames running, are kept on the server with a mask
 *                   and drawn with a masked copy; others as a rectangle
 *                   per run of one colour in each texture column (see
 *                   XW_Sprite)
 * ====================================================================== */

int xw3d_on = 1;                        /* 0: the engine's own software view (XW_SOFT=1) */
static Uint32 xw3_pal[256];             /* the game palette as pixels */
static int xw3_pal_ready;

static short xw_col_page[XW_W], xw_col_half[XW_W];
static byte  xw_col_u[XW_W];
static int   xw_post_page, xw_post_u;
static int   xw_ceiling;

#define XW_CACHE     2048               /* hash table size, a power of two */
/* scaled textures kept at once: fewer when they are bigger */
#define XW_CACHE_MAX (4800 / (S * S) > 100 ? 4800 / (S * S) : 100)
#define XW_TOMB      0xffffffffu
typedef struct { unsigned key, pix, used; } xw_entry;
static xw_entry xw_cache[XW_CACHE];
static int      xw_cache_n;
static unsigned xw_clock;
static unsigned xw_base[2048];          /* widened texture per VSWAP page */

static void xw3_palette (void)
{
    if (xw3_pal_ready)
        return;
    for (int i = 0; i < 256; i++)
        xw3_pal[i] = x_rgb (gamepal[i].r, gamepal[i].g, gamepal[i].b);
    xw3_pal_ready = 1;
}

/* The texture of a VSWAP wall page at window scale: S*64 wide, 64 high. */
static unsigned xw_base_of (int page)
{
    static Uint32 img[64 * 64];
    byte *src;
    unsigned narrow, wide;

    if (page < 0 || page >= (int)(sizeof xw_base / sizeof xw_base[0]))
        Quit ("xwolf: wall page %d out of range", page);
    if (xw_base[page])
        return xw_base[page];
    xw3_palette ();
    src = PM_GetPage (page);            /* stored a column at a time */
    for (int r = 0; r < 64; r++)
        for (int c = 0; c < 64; c++)
            img[r * 64 + c] = xw3_pal[src[c * 64 + r]];
    narrow = x_canvas (64, 64);
    x_target (narrow);
    x_image (0, 0, 64, 64, img, 64);
    wide = x_canvas (64 * S, 64);
    x_target (wide);
    for (int c = 0; c < 64; c++)
        for (int k = 0; k < S; k++)
            x_blit (narrow, c, 0, 1, 64, c * S + k, 0);
    x_target (0);
    x_free_canvas (narrow);
    return xw_base[page] = wide;
}

/* Heights are kept exact while small, in steps when large. */
static int xw_qhalf (int h)
{
    if (h <= 64)  return h;
    if (h <= 128) return h & ~1;
    if (h <= 256) return h & ~3;
    return h & ~7;
}

static void xw_evict (void)
{
    int oldest = -1;

    for (int i = 0; i < XW_CACHE; i++)
        if (xw_cache[i].key != 0 && xw_cache[i].key != XW_TOMB &&
            (oldest < 0 || xw_cache[i].used < xw_cache[oldest].used))
            oldest = i;
    if (oldest >= 0) {
        x_free_canvas (xw_cache[oldest].pix);
        xw_cache[oldest].key = XW_TOMB;
        xw_cache_n--;
    }
}

/*
 * Building a scaled texture takes 100-200 requests, and walking into a new
 * room can ask for a dozen in one frame, which shows as a hitch. So only
 * XW_BUILD_BUDGET are built per frame; past that a wall may be drawn from
 * a height already built if it is within about 6% (else it is built anyway:
 * a bigger difference shows as walls jumping).
 */
#define XW_BUILD_BUDGET 3
static int xw_builds_frame;
static int xw_spr_builds_frame;         /* sprites kept this frame (see XW_Sprite) */
static unsigned xw_frame_no;            /* 3D frames so far */

/* page's texture at half-height qh if built, else 0; *slot: where it would go. */
static unsigned xw_lookup (int page, int qh, int *slot)
{
    unsigned key = (unsigned)(page + 1) << 16 | (unsigned)qh;
    unsigned i = (key * 2654435761u) >> 20 & (XW_CACHE - 1);
    int free_slot = -1;

    for (int n = 0; n < XW_CACHE; n++, i = (i + 1) & (XW_CACHE - 1)) {
        if (xw_cache[i].key == key) {
            xw_cache[i].used = ++xw_clock;
            return xw_cache[i].pix;
        }
        if (xw_cache[i].key == XW_TOMB && free_slot < 0)
            free_slot = (int)i;
        if (xw_cache[i].key == 0) {
            if (free_slot < 0)
                free_slot = (int)i;
            break;
        }
    }
    if (slot != NULL)
        *slot = free_slot;
    return 0;
}

/* page's texture at half-height *qh (window pixels: S*64 by 2*qh*S); over
 * budget, *qh may come back as a nearby height that is already built. */
static unsigned xw_scaled (int page, int *qhp)
{
    int qh = *qhp, free_slot, H = 2 * qh * S;
    unsigned base, pix = xw_lookup (page, qh, &free_slot);

    if (pix)
        return pix;
    if (xw_builds_frame >= XW_BUILD_BUDGET) {
        int most = qh / 16 > 2 ? qh / 16 : 2;

        for (int d = 1; d <= most; d++)
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                int q = xw_qhalf (qh + sgn * d);

                if (q > 0 && q != qh && (pix = xw_lookup (page, q, NULL)) != 0) {
                    *qhp = q;
                    return pix;
                }
            }
    }
    xw_builds_frame++;
    /* the slot from the lookup above: nothing has changed the table since */
    unsigned long b0 = XW_BYTES();

    if (xw_cache_n >= XW_CACHE_MAX) {
        xw_evict ();
        xw_lookup (page, qh, &free_slot);     /* eviction may free an earlier slot */
    }
    base = xw_base_of (page);
    pix = x_canvas (64 * S, H);
    x_target (pix);
    /* each texture row becomes a band of rows: copy it once, then double */
    for (int r = 0; r < 64; r++) {
        int y0 = r * H / 64, y1 = (r + 1) * H / 64, n = 1;

        if (y1 <= y0)
            continue;
        x_blit (base, 0, r, 64 * S, 1, 0, y0);
        while (n < y1 - y0) {
            int m = n < y1 - y0 - n ? n : y1 - y0 - n;

            x_blit (pix, 0, y0, 64 * S, m, 0, y0 + n);
            n += m;
        }
    }
    x_target (0);
    xw_cache[free_slot].key = (unsigned)(page + 1) << 16 | (unsigned)qh;
    xw_cache[free_slot].pix = pix;
    xw_cache[free_slot].used = ++xw_clock;
    xw_cache_n++;
    xw_builds++;
    xw_bbuild += XW_BYTES() - b0;
    return pix;
}

static unsigned long xw_bbuild_now (void) { return xw_bbuild; }

/* ---- hooks called by the engine ---- */

void XW_SetPost (int page, int texture)
{
    xw_post_page = page;
    xw_post_u = (texture >> TEXTURESHIFT) & (TEXTURESIZE - 1);
}

void XW_WallPost (int x, int half)
{
    if (x < 0 || x >= XW_W)
        return;
    xw_col_page[x] = (short)xw_post_page;
    xw_col_u[x] = (byte)xw_post_u;
    xw_col_half[x] = (short)half;
}

void XW_ClearView (int ceiling)
{
    xw_ceiling = ceiling;
    xw_builds_frame = 0;
    xw_spr_builds_frame = 0;
    xw_frame_no++;
    memset (xw_col_half, 0, sizeof xw_col_half);
}

void XW_DrawWalls (void)
{
    unsigned long long u0 = xi_us ();
    unsigned long b0 = XW_BYTES(), bb = xw_bbuild_now ();
    int ox = xw_ox + viewscreenx * S, oy = xw_oy + viewscreeny * S, vh = viewheight * S;
    int half = viewheight / 2;

    xw3_palette ();
    x_target (0);
    x_color (xw3_pal[xw_ceiling]);
    x_fill_rect (ox, oy, viewwidth * S, half * S);
    x_color (xw3_pal[0x19]);
    x_fill_rect (ox, oy + half * S, viewwidth * S, (viewheight - half) * S);
    for (int x = 0; x < viewwidth;) {
        int h = xw_col_half[x], qh, page = xw_col_page[x], u = xw_col_u[x], w = 1;
        int top, sy = 0, rows;
        unsigned pix;

        if (h <= 0) {
            x++;
            continue;
        }
        qh = xw_qhalf (h);
        /* neighbours that carry on along the same texture at the same height */
        while (x + w < viewwidth && xw_col_page[x + w] == page &&
               xw_col_half[x + w] > 0 && xw_qhalf (xw_col_half[x + w]) == qh &&
               xw_col_u[x + w] == u + w && u + w < 64)
            w++;
        pix = xw_scaled (page, &qh);
        top = (centery - qh) * S;
        if (top < 0) {
            sy = -top;
            top = 0;
        }
        rows = 2 * qh * S - sy;
        if (top + rows > vh)
            rows = vh - top;
        if (rows > 0)
            x_blit (pix, u * S, sy, w * S, rows, ox + x * S, oy + top);
        xw_nblits++;
        x += w;
    }
    xw_bwall += XW_BYTES() - b0 - (xw_bbuild_now () - bb);
    xw_acc_walls += xi_us () - u0;
}

/*
 * Sprites. A sprite is drawn as a rectangle per run of one colour in each
 * texture column, about 1-2 KB for a big one. The weapon, and any sprite
 * seen at the same size two frames running (things standing still while
 * you look at them), are kept instead on the server: a pixmap of the
 * sprite at that size and a 1-bit mask of its pixels, drawn with one
 * masked copy (about 70 bytes) per stretch of it not hidden by walls.
 */

/* where rectangles go: window pixels of engine (0, 0), and the rows kept */
static int xw_spr_ox, xw_spr_oy, xw_spr_ylim, xw_spr_building, xw_spr_full;

/* Add one colour run of a sprite (engine pixels, from xw_spr_ox/oy). */
static void xw_sprite_rect (int x0, int x1, int y0, int y1, byte col)
{
    if (xw_nrect == XW_RECTS) {
        if (xw_spr_building) {          /* too many to keep: draw it directly */
            xw_spr_full = 1;
            return;
        }
        xw_flush_rects_pal (xw3_pal);
    }
    xw_rect[xw_nrect][0] = (short)(xw_spr_ox + x0 * S);
    xw_rect[xw_nrect][1] = (short)(xw_spr_oy + y0 * S);
    xw_rect[xw_nrect][2] = (short)((x1 - x0) * S);
    xw_rect[xw_nrect][3] = (short)((y1 - y0) * S);
    xw_rect_col[xw_nrect++] = col;
}

/* One texture column of a sprite over columns [xs, xe), rows [0, ylim). */
static void xw_sprite_column (byte *src, byte *cmds, int xs, int xe, int toppix, fixed fracstep)
{
    int ylim = xw_spr_ylim;

    for (int end = ReadShort (cmds) >> 1; end; cmds += 6, end = ReadShort (cmds) >> 1) {
        int top = (int16_t)ReadShort (cmds + 2), start = ReadShort (cmds + 4) >> 1;
        fixed frac = start * fracstep;
        int endpix = (int)(frac >> FRACBITS) + toppix;
        int run0 = 0, run1 = 0, runcol = -1;

        for (; start < end; start++) {
            int startpix = endpix;
            byte col = src[top + start];

            frac += fracstep;
            endpix = (int)(frac >> FRACBITS) + toppix;
            if (startpix >= ylim)
                break;
            if (endpix <= 0 || endpix <= startpix)
                continue;
            if (startpix < 0)
                startpix = 0;
            if (col == runcol && startpix == run1) {
                run1 = endpix > ylim ? ylim : endpix;
                continue;
            }
            if (runcol >= 0)
                xw_sprite_rect (xs, xe, run0, run1, (byte)runcol);
            runcol = col;
            run0 = startpix;
            run1 = endpix > ylim ? ylim : endpix;
        }
        if (runcol >= 0)
            xw_sprite_rect (xs, xe, run0, run1, (byte)runcol);
    }
}

/* Draw it as rectangles, clipped to the view (and walls, with clip). */
static void xw_sprite_direct (byte *src, compshape_t *shape, fixed fracstep, int actx, int toppix,
                              int height, int clip)
{
    fixed frac = shape->leftpix * fracstep;
    int x1, x2 = (int)(frac >> FRACBITS) + actx;

    xw_spr_ox = xw_ox + viewscreenx * S;
    xw_spr_oy = xw_oy + viewscreeny * S;
    xw_spr_ylim = viewheight;
    for (int i = shape->leftpix; i <= shape->rightpix; i++) {
        int a, b;

        x1 = x2;
        if (x1 >= viewwidth)
            break;
        frac += fracstep;
        x2 = (int)(frac >> FRACBITS) + actx;
        a = x1 < 0 ? 0 : x1;
        b = x2 > viewwidth ? viewwidth : x2;
        while (a < b) {                 /* runs of columns not behind a wall */
            int s;

            if (clip && wallheight[a] >= height) {
                a++;
                continue;
            }
            s = a;
            while (a < b && !(clip && wallheight[a] >= height))
                a++;
            xw_sprite_column (src, src + shape->dataofs[i - shape->leftpix], s, a, toppix, fracstep);
        }
    }
    x_target (0);
    xw_flush_rects_pal (xw3_pal);       /* one sprite's runs don't overlap: group by colour */
}

#define XW_SPR 64
#define XW_SPR_MAX (256 / (S * S) < 16 ? 16 : 256 / (S * S) > XW_SPR ? XW_SPR : 256 / (S * S))
#define XW_SPR_BUILDS 2                 /* per frame */
typedef struct {
    int shape, scale;                   /* 0 scale: unused */
    unsigned pix, mask, stencil;        /* 0 pix: seen, not kept */
    int bx, by, bw, bh;                 /* the pixmap, window pixels from the sprite's corner */
    unsigned used, seen;
} xw_spr;
static xw_spr xw_sprs[XW_SPR];

static void xw_spr_drop (xw_spr *e)
{
    x_free_stencil (e->stencil);
    e->stencil = 0;
    x_free_canvas (e->pix);
    x_free_canvas (e->mask);
    e->pix = e->mask = 0;
}

/* The entry for shape at scale, made if need be (replacing the oldest). */
static xw_spr *xw_spr_find (int shape, int scale)
{
    xw_spr *e = NULL, *old = &xw_sprs[0];
    int kept = 0;

    for (int i = 0; i < XW_SPR; i++) {
        if (xw_sprs[i].scale == scale && xw_sprs[i].shape == shape && scale != 0)
            e = &xw_sprs[i];
        if (xw_sprs[i].used < old->used)
            old = &xw_sprs[i];
        kept += xw_sprs[i].pix != 0;
    }
    if (e == NULL) {
        xw_spr_drop (old);
        memset (old, 0, sizeof *old);
        old->shape = shape;
        old->scale = scale;
        e = old;
    }
    if (e->pix == 0 && kept >= XW_SPR_MAX) {     /* room for one more to be kept */
        xw_spr *drop = NULL;

        for (int i = 0; i < XW_SPR; i++)
            if (xw_sprs[i].pix && (drop == NULL || xw_sprs[i].used < drop->used))
                drop = &xw_sprs[i];
        if (drop != NULL)
            xw_spr_drop (drop);
    }
    e->used = ++xw_clock;
    return e;
}

/* Make e's pixmap and mask; 0 if it has too many runs to keep. */
static int xw_spr_build (xw_spr *e, byte *src, compshape_t *shape, fixed fracstep)
{
    fixed frac = shape->leftpix * fracstep;
    int x1, x2 = (int)(frac >> FRACBITS), n, minx = 1 << 30, miny = 1 << 30, maxx = 0, maxy = 0;

    xw_spr_ox = xw_spr_oy = 0;
    xw_spr_ylim = 1 << 14;
    xw_spr_building = 1;
    xw_spr_full = 0;
    xw_nrect = 0;
    for (int i = shape->leftpix; i <= shape->rightpix && !xw_spr_full; i++) {
        x1 = x2;
        frac += fracstep;
        x2 = (int)(frac >> FRACBITS);
        if (x2 > x1)
            xw_sprite_column (src, src + shape->dataofs[i - shape->leftpix], x1, x2, 0, fracstep);
    }
    xw_spr_building = 0;
    n = xw_nrect;
    if (xw_spr_full || n == 0) {
        xw_nrect = 0;
        return 0;
    }
    for (int i = 0; i < n; i++) {
        short *r = xw_rect[i];

        minx = r[0] < minx ? r[0] : minx;
        miny = r[1] < miny ? r[1] : miny;
        maxx = r[0] + r[2] > maxx ? r[0] + r[2] : maxx;
        maxy = r[1] + r[3] > maxy ? r[1] + r[3] : maxy;
    }
    for (int i = 0; i < n; i++) {
        xw_rect[i][0] = (short)(xw_rect[i][0] - minx);
        xw_rect[i][1] = (short)(xw_rect[i][1] - miny);
    }
    e->bx = minx;
    e->by = miny;
    e->bw = maxx - minx;
    e->bh = maxy - miny;
    e->pix = x_canvas (e->bw, e->bh);
    e->mask = x_mask (e->bw, e->bh);
    x_mask_rects (e->mask, &xw_rect[0][0], n);
    e->stencil = x_stencil (e->mask);
    x_target (e->pix);
    xw_flush_rects_pal (xw3_pal);
    x_target (0);
    return 1;
}

/* Draw kept sprite e with its corner at view (actx, toppix). */
static void xw_spr_draw (const xw_spr *e, int actx, int toppix, int height, int clip)
{
    int vx = xw_ox + viewscreenx * S, vy = xw_oy + viewscreeny * S;
    int px = vx + actx * S + e->bx, py = vy + toppix * S + e->by;
    int y0 = py > vy ? py : vy;
    int y1 = py + e->bh < vy + viewheight * S ? py + e->bh : vy + viewheight * S;
    int a = (px - vx) / S, b = (px + e->bw - vx + S - 1) / S;

    if (y1 <= y0)
        return;
    if (a < 0)
        a = 0;
    if (b > viewwidth)
        b = viewwidth;
    x_target (0);
    while (a < b) {                     /* stretches of columns not behind a wall */
        int s, dx0, dx1;

        if (clip && wallheight[a] >= height) {
            a++;
            continue;
        }
        s = a;
        while (a < b && !(clip && wallheight[a] >= height))
            a++;
        dx0 = vx + s * S > px ? vx + s * S : px;
        dx1 = vx + a * S < px + e->bw ? vx + a * S : px + e->bw;
        if (dx1 > dx0)
            x_blit_stencil (e->stencil, e->pix, dx0 - px, y0 - py, dx1 - dx0, y1 - y0, dx0, y0);
    }
}

/* ScaleShape (clip: hidden where a wall is nearer) and SimpleScaleShape. */
void XW_Sprite (int xcenter, int shapenum, int scale, int height, int clip)
{
    byte *src;
    compshape_t *shape;
    fixed fracstep;
    int actx, toppix;
    xw_spr *e = NULL;
    unsigned long b0 = XW_BYTES();
    unsigned long long u0 = xi_us ();

    if (scale <= 0)
        return;
    xw3_palette ();
    src = PM_GetSpritePage (shapenum);
    shape = (compshape_t *)src;
    fracstep = FixedDiv (scale, TEXTURESIZE / 2);
    actx = xcenter - scale;
    toppix = centery - scale;
    if (scale <= viewheight) {          /* not one filling the screen */
        e = xw_spr_find (shapenum, scale);
        if (e->pix == 0) {
            int want = !clip || e->seen + 1 >= xw_frame_no;     /* the weapon, or seen last frame */

            e->seen = xw_frame_no;
            if (!want || xw_spr_builds_frame >= XW_SPR_BUILDS ||
                !(xw_spr_builds_frame++, xw_spr_build (e, src, shape, fracstep)))
                e = NULL;
        }
    }
    if (e != NULL)
        xw_spr_draw (e, actx, toppix, height, clip);
    else
        xw_sprite_direct (src, shape, fracstep, actx, toppix, height, clip);
    if (clip)
        xw_bsprite += XW_BYTES() - b0;
    else
        xw_bweapon += XW_BYTES() - b0;
    xw_acc_sprites += xi_us () - u0;
}

void XW_Mark3D (void)
{
    xw_3d_frame = 1;
}

/* ======================================================================
 * Resizing
 * ====================================================================== */

/* The window is now w x h: pick the scale and margins, and start over. */
static void xw_layout (int w, int h)
{
    int s = w / XW_W < h / XW_H ? w / XW_W : h / XW_H;

    if (s < 1)
        s = 1;
    if (s != xw_s) {                    /* every scaled texture is the wrong size */
        for (int i = 0; i < XW_CACHE; i++) {
            if (xw_cache[i].key != 0 && xw_cache[i].key != XW_TOMB)
                x_free_canvas (xw_cache[i].pix);
            xw_cache[i].key = 0;
        }
        xw_cache_n = 0;
        for (int i = 0; i < XW_SPR; i++)
            xw_spr_drop (&xw_sprs[i]);
        for (int i = 0; i < (int)(sizeof xw_base / sizeof xw_base[0]); i++) {
            x_free_canvas (xw_base[i]);
            xw_base[i] = 0;
        }
        xw_s = s;
    }
    xw_ox = (w - XW_W * s) / 2;
    xw_oy = (h - XW_H * s) / 2;
    /* x11.h made a new back buffer: black margins, then everything again */
    x_target (0);
    x_color (x_rgb (0, 0, 0));
    x_fill_rect (0, 0, w, h);
    xw_forget (0, 0, XW_W, XW_H);
    xw_own_on = 0;                      /* the next 3D frame takes the view again */
}
