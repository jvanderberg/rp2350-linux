/* bbs.c - a tiny telnet BBS for RP2350 Linux (8 MiB RAM, busybox inetd, tcc).
 *
 * One process per caller: inetd hands us the TCP socket as stdin/stdout and
 * we speak just enough telnet to get character-at-a-time input.  Callers log
 * in with a handle and password (data/users).  Shared state is plain files
 * under data/ (see bbsctl for setup).
 *
 * Build:  ./bbsctl build      (tcc -o bbs bbs.c)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <crypt.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/select.h>

#ifndef BBSDIR
#define BBSDIR "."   /* bbsctl passes the real directory */
#endif
#define MAXNODES 8
#define CHATLOG "data/chat.log"

static char bbsname[40] = "RP2350 BBS", sysop[16] = "sysop", me[16] = "";
static int max_users = 4, idle_secs = 600;
static long max_upload_kb = 16384;
static int node, nodefd = -1, in_chat, in_mud;

static int telnet, hide;     /* -t: raw TCP from inetd; hide: echo '*' */
static void onsig(int s);
static void mud_gone(void);
static void (*poll_hook)(void);     /* run while tail_getln waits */

/* ---------- output: own buffer, so telnet can get CRLF line ends ---------- */

static char obuf[2048];
static int on;

static void oflush(void)
{
    int o = 0, n;
    while (o < on) {
        n = write(1, obuf + o, on - o);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) onsig(0);
        o += n;
    }
    on = 0;
}

/* Our text and art are UTF-8.  Retro BBS terminals draw in CP437 (the DOS
 * character set), where each block/box character is a single byte, so for
 * those callers UTF-8 sequences are folded to CP437 on the way out.
 * cp437: -1 = not known yet, 0 = caller is UTF-8, 1 = caller is CP437. */
static int cp437 = -1;

static int to437(unsigned u)
{
    static const unsigned short map[] = {
        0x2588, 0xDB, 0x2593, 0xB2, 0x2592, 0xB1, 0x2591, 0xB0, 0x2580, 0xDF, 0x2584, 0xDC,
        0x258C, 0xDD, 0x2590, 0xDE, 0x25A0, 0xFE, 0x2500, 0xC4, 0x2502, 0xB3, 0x250C, 0xDA,
        0x2510, 0xBF, 0x2514, 0xC0, 0x2518, 0xD9, 0x251C, 0xC3, 0x2524, 0xB4, 0x252C, 0xC2,
        0x2534, 0xC1, 0x253C, 0xC5, 0x2550, 0xCD, 0x2551, 0xBA, 0x2554, 0xC9, 0x2557, 0xBB,
        0x255A, 0xC8, 0x255D, 0xBC, 0x00B7, 0xFA, 0x00B0, 0xF8, 0x2022, 0x07, 0 };
    int i;
    for (i = 0; map[i]; i += 2)
        if (map[i] == u) return map[i + 1];
    return '?';
}

static void outc(int c)
{
    static unsigned u;
    static int need;
    c &= 255;
    if (cp437 > 0 && c >= 0x80) {
        if (c >= 0xC0) {                    /* lead byte: start collecting */
            need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            u = c & (0xFF >> (need + 2));
            return;
        }
        if (!need) return;
        u = u << 6 | (c & 0x3F);
        if (--need) return;
        c = to437(u);
    }
    if (on > (int)sizeof obuf - 2) oflush();
    if (c == '\n' && telnet) obuf[on++] = '\r';
    obuf[on++] = c;
}

static void outs(const char *s)
{
    while (*s) outc(*s++);
}

/* "|NN" pipe colour codes (00-15 fg, 16-23 bg) */
static void emit(const char *s)
{
    static const char map[] = "04261537";   /* DOS colour -> ANSI colour */
    char e[12];
    for (; *s; s++) {
        if (s[0] == '|' && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2])) {
            int n = (s[1] - '0') * 10 + (s[2] - '0');
            /* "bright black" vanishes on many dark themes: use a fixed mid grey */
            if (n == 8) { outs(cp437 > 0 ? "\033[1;30m" : "\033[0;38;5;245m"); s += 2; continue; }
            if (n < 16) { sprintf(e, "\033[%d;3%cm", n > 7, map[n & 7]); outs(e); s += 2; continue; }
            if (n < 24) { sprintf(e, "\033[4%cm", map[n - 16]); outs(e); s += 2; continue; }
        }
        outc(*s);
    }
}

static void pp(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    emit(b);
}

static void chat_say(const char *fmt, ...)
{
    char b[400];
    va_list ap;
    int fd, n;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof b) n = sizeof b - 1;
    fd = open(CHATLOG, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) { write(fd, b, n); close(fd); }
}

static void bye(int rc)
{
    if (in_chat) { in_chat = 0; chat_say("|08* %s left chat|07\n", me); }
    mud_gone();
    if (telnet) outs("\033[0m\n");
    oflush();
    exit(rc);
}

static void onsig(int s)
{
    (void)s;
    mud_gone();
    _exit(0);
}

/* ---------- input ---------- */

/* strip telnet protocol from the input stream; -1 = byte swallowed */
/* guess the character set from the telnet terminal type, if it is one we know */
static void term_is(char *name)
{
    static const char *utf[] = { "xterm", "vt", "linux", "screen", "tmux", "rxvt", "kitty", "alacritty", "putty", 0 };
    static const char *dos[] = { "ansi", "sync", "cterm", "pc", "qodem", "netrunner", 0 };
    int i;
    for (i = 0; name[i]; i++) name[i] = tolower((unsigned char)name[i]);
    if (cp437 >= 0) return;
    for (i = 0; utf[i]; i++) if (strstr(name, utf[i])) cp437 = 0;
    for (i = 0; cp437 < 0 && dos[i]; i++) if (strstr(name, dos[i])) cp437 = 1;
}

static int tfilter(int ch)
{
    static int st, lastcr, cmd, sn;
    static unsigned char sb[32];
    switch (st) {
    case 0:
        if (ch == 255) { st = 1; return -1; }
        if (lastcr && (ch == 0 || ch == '\n')) { lastcr = 0; return -1; }  /* CR NUL / CR LF */
        lastcr = ch == '\r';
        return ch;
    case 1:                                 /* after IAC */
        cmd = ch;
        sn = 0;
        st = ch == 250 ? 3 : ch >= 251 && ch <= 254 ? 2 : 0;
        return -1;
    case 2:                                 /* WILL/WONT/DO/DONT option */
        st = 0;
        if (cmd == 251 && ch == 24) {       /* WILL TTYPE: ask which (SB TTYPE SEND) */
            oflush();
            write(1, "\377\372\030\001\377\360", 6);
        }
        return -1;
    case 3:                                 /* inside SB ... */
        if (ch == 255) st = 4;
        else if (sn < (int)sizeof sb - 1) sb[sn++] = ch;
        return -1;
    default:                                /* ... until IAC SE */
        st = ch == 240 ? 0 : 3;
        if (ch == 240 && sn > 2 && sb[0] == 24 && sb[1] == 0) {     /* TTYPE IS name */
            sb[sn] = 0;
            term_is((char *)sb + 2);
        }
        return -1;
    }
}

/* ms < 0: block (idle timeout hangs up).  Returns -1 on timeout, 0 for a
 * swallowed escape sequence, else the byte. */
static int getkey(int ms)
{
    fd_set r;
    struct timeval tv;
    unsigned char ch;
    int n;

    oflush();
    FD_ZERO(&r);
    FD_SET(0, &r);
    tv.tv_sec = ms < 0 ? idle_secs : ms / 1000;
    tv.tv_usec = ms < 0 ? 0 : (ms % 1000) * 1000;
    n = select(1, &r, 0, 0, &tv);
    if (n == 0) {
        if (ms < 0) { pp("\n|12Idle timeout - hanging up.|07\n"); bye(0); }
        return -1;
    }
    if (n < 0) { if (errno == EINTR) return -1; bye(1); }
    if (read(0, &ch, 1) != 1) bye(0);
    if (telnet && tfilter(ch) < 0) return 0;
    if (ch == 27) {                     /* arrow keys etc: eat the sequence */
        for (n = 0; n < 8; n++) {
            FD_ZERO(&r);
            FD_SET(0, &r);
            tv.tv_sec = 0;
            tv.tv_usec = 30000;
            if (select(1, &r, 0, 0, &tv) <= 0 || read(0, &ch, 1) != 1) break;
        }
        return 0;
    }
    return ch;
}

static int waitkey(void)
{
    int k;
    while ((k = getkey(-1)) <= 0) ;
    return k;
}

/* one edit step shared by the line editor and chat; returns new length */
static int edit(char *b, int n, int max, int k)
{
    if (k == 127 || k == 8) {
        while (n > 0) {
            unsigned char c = b[--n];
            if ((c & 0xC0) != 0x80) { outs("\b \b"); break; }
        }
    } else if (k >= 32 && n < max - 1) {
        b[n++] = k;
        outc(hide ? '*' : k);
    }
    return n;
}

static int getln(char *b, int max)
{
    int n = 0, k;
    for (;;) {
        k = waitkey();
        if (k == '\r' || k == '\n') break;
        if (k == 3) { n = 0; break; }               /* ^C cancels the line */
        n = edit(b, n, max, k);
    }
    b[n] = 0;
    outc('\n');
    return n;
}

static void pause_key(void)
{
    pp("|08[ press any key ]|07");
    waitkey();
    outs("\r\033[K");
}

/* pager: returns 1 when the user wants out */
static int more(int *ln)
{
    int k;
    if (++*ln < 21) return 0;
    *ln = 0;
    pp("|08-- more (q quits) --|07");
    k = waitkey();
    outs("\r\033[K");
    return k == 'q' || k == 'Q';
}

/* ---------- small helpers ---------- */

static char *stamp(time_t t)
{
    static char b[20];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", localtime(&t));
    return b;
}

static void chomp(char *s)
{
    int n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
}

static void loadconf(void)
{
    char l[128], *v;
    FILE *f = fopen("bbs.conf", "r");
    if (!f) return;
    while (fgets(l, sizeof l, f)) {
        chomp(l);
        if (l[0] == '#' || !(v = strchr(l, '='))) continue;
        *v++ = 0;
        if (!strcmp(l, "name")) snprintf(bbsname, sizeof bbsname, "%s", v);
        else if (!strcmp(l, "sysop")) snprintf(sysop, sizeof sysop, "%s", v);
        else if (!strcmp(l, "max_users")) max_users = atoi(v);
        else if (!strcmp(l, "idle_timeout")) idle_secs = atoi(v);
        else if (!strcmp(l, "max_upload_kb")) max_upload_kb = atol(v);
    }
    fclose(f);
    if (max_users < 1) max_users = 1;
    if (max_users > MAXNODES) max_users = MAXNODES;
    if (idle_secs < 30) idle_secs = 30;
}

static int is_sysop(void) { return !strcasecmp(me, sysop); }

/* ---------- nodes: one flock()ed file per line, freed when we die ---------- */

static int glock(void)
{
    int fd = open("data/lock", O_RDWR | O_CREAT, 0644);
    if (fd >= 0) flock(fd, LOCK_EX);
    return fd;
}

/* (re)write who is on our node */
static void node_mark(void)
{
    char b[64];
    int n = sprintf(b, "%s %ld\n", me, (long)time(0));
    lseek(nodefd, 0, SEEK_SET);
    ftruncate(nodefd, 0);
    write(nodefd, b, n);
}

static int node_take(void)
{
    char p[40];
    int g = glock(), i, fd;
    for (i = 1; i <= max_users && !node; i++) {
        sprintf(p, "data/nodes/%d", i);
        if ((fd = open(p, O_RDWR | O_CREAT, 0644)) < 0) continue;
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            node = i;
            nodefd = fd;
            node_mark();
        } else close(fd);
    }
    if (g >= 0) close(g);
    return node;
}

/* count callers online; show != 0 prints the list */
static int who(int show)
{
    char p[40], b[64], h[16];
    long t;
    int g = glock(), i, fd, n, cnt = 0, busy;
    for (i = 1; i <= MAXNODES; i++) {
        sprintf(p, "data/nodes/%d", i);
        if ((fd = open(p, O_RDONLY)) < 0) continue;
        busy = i == node || flock(fd, LOCK_EX | LOCK_NB) != 0;
        n = busy ? read(fd, b, sizeof b - 1) : 0;
        close(fd);
        if (!busy) continue;
        cnt++;
        b[n > 0 ? n : 0] = 0;
        t = 0;
        strcpy(h, "?");
        sscanf(b, "%15s %ld", h, &t);
        if (show)
            pp("  |11node %d|07  |15%-12s|07  on for %ld min%s\n", i, h,
               ((long)time(0) - t) / 60, i == node ? "  |08(you)|07" : "");
    }
    if (g >= 0) close(g);
    return cnt;
}

/* ---------- splash / text files with @CODES@ ---------- */

static void expand(const char *in, char *out, int max)
{
    static const char *key[] = { "@USER@", "@BBS@", "@SYSOP@", "@NODE@", "@MAX@", "@ONLINE@" };
    char val[40];
    int o = 0, i, l;
    while (*in && o < max - 1) {
        if (*in == '@') {
            for (i = 0; i < 6; i++)
                if (!strncmp(in, key[i], l = strlen(key[i]))) break;
            if (i < 6) {
                switch (i) {
                case 0: snprintf(val, sizeof val, "%s", me); break;
                case 1: snprintf(val, sizeof val, "%s", bbsname); break;
                case 2: snprintf(val, sizeof val, "%s", sysop); break;
                case 3: snprintf(val, sizeof val, "%d", node); break;
                case 4: snprintf(val, sizeof val, "%d", max_users); break;
                default: snprintf(val, sizeof val, "%d", who(0));
                }
                o += snprintf(out + o, max - o, "%s", val);
                if (o > max - 1) o = max - 1;
                in += l;
                continue;
            }
        }
        out[o++] = *in++;
    }
    out[o] = 0;
}

static void showfile(const char *path, int delay_ms)
{
    char l[512], x[768];
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(l, sizeof l, f)) {
        chomp(l);
        expand(l, x, sizeof x);
        emit(x);
        emit("|07\n");
        if (delay_ms) { oflush(); usleep(delay_ms * 1000); }
    }
    fclose(f);
}

static void tail(const char *path, long bytes)
{
    char l[512];
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    if (ftell(f) > bytes) {
        fseek(f, -bytes, SEEK_END);
        fgets(l, sizeof l, f);                      /* drop the partial line */
    } else rewind(f);
    while (fgets(l, sizeof l, f)) emit(l);
    fclose(f);
}

/* ---------- line input that keeps printing what others append to a file ---------- */

static void tail_getln(const char *path, long *pos, const char *prompt, char *in, int max)
{
    char l[512];
    struct stat st;
    int n = 0, k, redraw = 1;
    time_t last = time(0);
    FILE *f;

    for (;;) {
        if (poll_hook) poll_hook();
        if (stat(path, &st) == 0 && st.st_size != *pos) {
            if (st.st_size < *pos) *pos = 0;            /* file was truncated */
            if ((f = fopen(path, "r"))) {
                fseek(f, *pos, SEEK_SET);
                outs("\r\033[K");
                while (fgets(l, sizeof l, f)) emit(l);
                *pos = ftell(f);
                fclose(f);
                redraw = 1;
            }
        }
        if (redraw) {
            emit(prompt);
            for (k = 0; k < n; k++) outc(in[k]);
            redraw = 0;
        }
        k = getkey(400);
        if (k < 0) {
            if (time(0) - last > idle_secs) { pp("\n|12Idle timeout - hanging up.|07\n"); bye(0); }
            continue;
        }
        last = time(0);
        if (k == '\r' || k == '\n') {
            in[n] = 0;
            outs("\r\033[K");
            return;
        }
        n = edit(in, n, max, k);
    }
}

/* ---------- chat: everyone appends to one log and tails it ---------- */

static void chat(void)
{
    char in[200], prompt[40];
    struct stat st;
    long pos = 0;

    pp("\033[2J\033[H|14-=[ Chat ]=-|07  |08/q quit  /w who  /me action|07\n");
    tail(CHATLOG, 700);
    if (stat(CHATLOG, &st) == 0) pos = st.st_size;
    in_chat = 1;
    chat_say("|08* %s joined chat|07\n", me);
    snprintf(prompt, sizeof prompt, "|11%s>|07 ", me);
    for (;;) {
        tail_getln(CHATLOG, &pos, prompt, in, sizeof in);
        if (!in[0]) continue;
        if (!strcmp(in, "/q")) break;
        if (!strcmp(in, "/w")) who(1);
        else if (!strncmp(in, "/me ", 4)) chat_say("|13* %s %s|07\n", me, in + 4);
        else chat_say("|10<%s>|07 %s\n", me, in);
    }
    in_chat = 0;
    chat_say("|08* %s left chat|07\n", me);
}

/* ---------- MUD ----------
 * The world is text, opened only when needed and never cached:
 *   mud/ROOM.txt   one room each (format: see mud/start.txt)
 *   mud/npcs.txt   creatures, what they say, what they want (the quest)
 *   mud/items.txt  things to carry, wield and eat
 * Live state is in RAM (tmpfs, MUDDIR), shared by every caller's process:
 *   players  one mrec per node: who, which room, health
 *   npcs     one nrec per creature: where it is now, health
 *   items    one irec per item: where it is now
 *   in.N     node N's inbox; that caller tails it while typing
 * There is no server.  Whoever is in the MUD runs the world tick (NPCs
 * wander, talk and fight) when it is due, under the MUDDIR/lock flock. */

#define MUDDIR "/tmp/bbs-mud"
#define NPCS "mud/npcs.txt"
#define ITEMS "mud/items.txt"
#define MAXNPC 16
#define MAXITEM 32
#define MAXHP 20
#define TICK 4                          /* seconds between world ticks */

enum { K_THING, K_WEAPON, K_HEAL };
struct mrec { char name[16], room[24]; int hp; };
struct nrec { char id[16], name[32], room[24], home[24], drop[16]; int hp, maxhp, dmg, wander, hostile, angry; long back; };
struct irec { char id[16], name[32], loc[24], home[24]; int kind, val; };  /* loc: room, "#N" = node N carries it, "-" = nowhere */

static struct nrec N[MAXNPC];
static struct irec I[MAXITEM];
static int nn, ni, mlocked;
static char mroom[24], myloc[8];

static FILE *room_open(const char *id);

static int okroom(const char *s)
{
    int i;
    for (i = 0; s[i]; i++)
        if (!islower((unsigned char)s[i]) && !isdigit((unsigned char)s[i]) && s[i] != '_') return 0;
    return i > 0 && i < 21;
}

/* ----- shared state ----- */

static int mlk(int wait)
{
    int fd = open(MUDDIR "/lock", O_RDWR | O_CREAT, 0644);
    if (fd >= 0 && flock(fd, wait ? LOCK_EX : LOCK_EX | LOCK_NB)) { close(fd); fd = -1; }
    if (fd >= 0) mlocked = 1;
    return fd;
}

static void munlk(int fd)
{
    if (fd >= 0) close(fd);
    mlocked = 0;
}

static void put_rec(const char *path, int i, const void *rec, int size)
{
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) return;
    pwrite(fd, rec, size, (off_t)i * size);
    close(fd);
}

static void pl_write(int n, struct mrec *r) { put_rec(MUDDIR "/players", n, r, sizeof *r); }
static void npc_save(int i) { put_rec(MUDDIR "/npcs", i, &N[i], sizeof N[0]); }
static void item_save(int i) { put_rec(MUDDIR "/items", i, &I[i], sizeof I[0]); }

static int mud_get(int n, struct mrec *r)
{
    int fd = open(MUDDIR "/players", O_RDONLY), ok;
    if (fd < 0) return 0;
    ok = pread(fd, r, sizeof *r, (off_t)n * sizeof *r) == sizeof *r && r->name[0];
    close(fd);
    if (ok) r->name[15] = r->room[23] = 0;
    return ok;
}

static void world_load(void)
{
    int fd, n;
    nn = ni = 0;
    if ((fd = open(MUDDIR "/npcs", O_RDONLY)) >= 0) {
        if ((n = read(fd, N, sizeof N)) > 0) nn = n / sizeof N[0];
        close(fd);
    }
    if ((fd = open(MUDDIR "/items", O_RDONLY)) >= 0) {
        if ((n = read(fd, I, sizeof I)) > 0) ni = n / sizeof I[0];
        close(fd);
    }
}

static void mud_raw(int n, const char *b, int len)
{
    char p[40];
    int fd;
    sprintf(p, MUDDIR "/in.%d", n);
    if ((fd = open(p, O_WRONLY | O_APPEND)) >= 0) { write(fd, b, len); close(fd); }
}

/* send a line to everyone in `room` (everyone in the MUD if 0) except node `skip` */
static void mud_send(const char *room, int skip, const char *fmt, ...)
{
    char b[500];
    struct mrec r;
    va_list ap;
    int n, len;
    va_start(ap, fmt);
    len = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (len >= (int)sizeof b) len = sizeof b - 1;
    for (n = 1; n <= MAXNODES; n++)
        if (n != skip && mud_get(n, &r) && (!room || !strcmp(r.room, room))) mud_raw(n, b, len);
}

/* ----- definitions: "@tag id" blocks of "key value" lines ----- */

/* in the idx-th block of `file`: the id (key 0), or the nth "key value" line */
static int def_get(const char *file, int idx, const char *key, int nth, char *out, int max)
{
    char l[400], *sp;
    FILE *f = fopen(file, "r");
    int cur = -1, kl = key ? strlen(key) : 0, hit = 0;
    if (!f) return 0;
    while (!hit && fgets(l, sizeof l, f)) {
        chomp(l);
        if (l[0] == '@') {
            if (++cur > idx) break;
            if (cur == idx && !key) {
                sp = strchr(l, ' ');
                snprintf(out, max, "%s", sp ? sp + 1 : "");
                hit = 1;
            }
            continue;
        }
        if (cur != idx || !key || strncmp(l, key, kl) || l[kl] != ' ' || nth-- > 0) continue;
        snprintf(out, max, "%s", l + kl + 1);
        hit = 1;
    }
    fclose(f);
    return hit;
}

static int def_int(const char *file, int idx, const char *key)
{
    char v[32];
    return def_get(file, idx, key, 0, v, sizeof v) ? atoi(v) : 0;
}

/* build the RAM state from the text files, once per boot (lock held) */
static void world_init(void)
{
    char v[400];
    int i, fd;
    if (!access(MUDDIR "/npcs", F_OK)) return;
    memset(N, 0, sizeof N);
    memset(I, 0, sizeof I);
    for (i = 0; i < MAXITEM && def_get(ITEMS, i, 0, 0, v, sizeof v); i++) {
        struct irec *p = &I[i];
        snprintf(p->id, sizeof p->id, "%s", v);
        def_get(ITEMS, i, "name", 0, v, sizeof v);
        snprintf(p->name, sizeof p->name, "%s", v);
        strcpy(p->home, "-");
        if (def_get(ITEMS, i, "room", 0, v, sizeof v)) snprintf(p->home, sizeof p->home, "%s", v);
        strcpy(p->loc, p->home);
        v[0] = 0;
        def_get(ITEMS, i, "kind", 0, v, sizeof v);
        p->kind = !strcmp(v, "weapon") ? K_WEAPON : !strcmp(v, "heal") ? K_HEAL : K_THING;
        p->val = def_int(ITEMS, i, "value");
    }
    ni = i;
    if ((fd = open("data/mud_items.sav", O_RDONLY)) >= 0) {    /* where things lay at the last save */
        struct irec s;
        FILE *rf;
        for (i = 0; i < ni && pread(fd, &s, sizeof s, (off_t)i * sizeof s) == sizeof s; i++) {
            s.loc[sizeof s.loc - 1] = 0;
            if (strncmp(s.id, I[i].id, sizeof s.id) || !s.loc[0] || s.loc[0] == '#') continue;
            if (strcmp(s.loc, "-")) { if (!(rf = room_open(s.loc))) continue; fclose(rf); }
            strcpy(I[i].loc, s.loc);
        }
        close(fd);
    }
    if ((fd = open(MUDDIR "/items", O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
        write(fd, I, ni * sizeof I[0]);
        close(fd);
    }
    for (i = 0; i < MAXNPC && def_get(NPCS, i, 0, 0, v, sizeof v); i++) {
        struct nrec *p = &N[i];
        snprintf(p->id, sizeof p->id, "%s", v);
        def_get(NPCS, i, "name", 0, v, sizeof v);
        snprintf(p->name, sizeof p->name, "%s", v);
        strcpy(p->home, "start");
        if (def_get(NPCS, i, "room", 0, v, sizeof v)) snprintf(p->home, sizeof p->home, "%s", v);
        strcpy(p->room, p->home);
        if (def_get(NPCS, i, "drop", 0, v, sizeof v)) snprintf(p->drop, sizeof p->drop, "%s", v);
        p->hp = p->maxhp = def_int(NPCS, i, "hp");
        p->dmg = def_int(NPCS, i, "damage");
        p->wander = def_int(NPCS, i, "wander");
        p->hostile = def_int(NPCS, i, "hostile");
    }
    nn = i;
    if ((fd = open("data/mud_npcs.sav", O_RDONLY)) >= 0) {     /* and where creatures were */
        struct nrec s;
        FILE *rf;
        for (i = 0; i < nn && pread(fd, &s, sizeof s, (off_t)i * sizeof s) == sizeof s; i++) {
            s.room[sizeof s.room - 1] = 0;
            if (strncmp(s.id, N[i].id, sizeof s.id) || s.hp <= 0 || !(rf = room_open(s.room))) continue;
            fclose(rf);
            strcpy(N[i].room, s.room);
            N[i].hp = s.hp > N[i].maxhp ? N[i].maxhp : s.hp;
        }
        close(fd);
    }
    if ((fd = open(MUDDIR "/npcs", O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
        write(fd, N, nn * sizeof N[0]);
        close(fd);
    }
}

/* ----- lookups (after world_load) ----- */

static int has(const char *hay, const char *w)      /* case-blind substring */
{
    int n = strlen(w);
    for (; n && *hay; hay++)
        if (!strncasecmp(hay, w, n)) return 1;
    return 0;
}

static int npc_here(const char *w)
{
    int i;
    for (i = 0; i < nn; i++)
        if (N[i].room[0] && !strcmp(N[i].room, mroom) && (!strcasecmp(N[i].id, w) || has(N[i].name, w))) return i;
    return -1;
}

static int item_at(const char *loc, const char *w)
{
    int i;
    for (i = 0; i < ni; i++)
        if (!strcmp(I[i].loc, loc) && (!strcasecmp(I[i].id, w) || has(I[i].name, w))) return i;
    return -1;
}

static int item_id(const char *id)
{
    int i;
    for (i = 0; i < ni; i++)
        if (!strcmp(I[i].id, id)) return i;
    return -1;
}

/* ----- rooms ----- */

static FILE *room_open(const char *id)
{
    char p[48];
    if (!okroom(id)) return 0;
    snprintf(p, sizeof p, "mud/%s.txt", id);
    return fopen(p, "r");
}

/* find the line "KEY WORD rest" (or "KEY rest" when word is 0) and copy rest */
static int room_find(const char *id, const char *key, const char *word, char *out, int max)
{
    char l[400], w[24];
    FILE *f = room_open(id);
    int kl = strlen(key), hit = 0, off;
    if (!f) return 0;
    while (!hit && fgets(l, sizeof l, f)) {
        chomp(l);
        if (strncmp(l, key, kl) || l[kl] != ' ') continue;
        off = 0;
        if (word && (sscanf(l + kl + 1, "%23s %n", w, &off) < 1 || strcasecmp(w, word))) continue;
        snprintf(out, max, "%s", l + kl + 1 + off);
        hit = 1;
    }
    fclose(f);
    return hit;
}

static void room_show(const char *id)
{
    char l[400], w[24], exits[200] = "";
    struct mrec r;
    FILE *f = room_open(id);
    int n, any = 0;
    if (!f) { pp("|12You are nowhere. (mud/%s.txt is missing)|07\n", id); return; }
    while (fgets(l, sizeof l, f)) {
        chomp(l);
        if (l[0] == '#') continue;
        if (!strncmp(l, "@name ", 6)) pp("\n|14%s|07\n", l + 6);
        else if (!strncmp(l, "@exit ", 6)) {
            if (sscanf(l + 6, "%23s", w) == 1 && strlen(exits) + strlen(w) + 2 < sizeof exits) {
                if (exits[0]) strcat(exits, ", ");
                strcat(exits, w);
            }
        } else if (l[0] != '@') { emit(l); emit("|07\n"); }
    }
    fclose(f);
    pp("|08Exits:|07 |11%s|07\n", exits[0] ? exits : "none");
    world_load();
    for (n = 0; n < nn; n++)
        if (!strcmp(N[n].room, id)) pp("|12%s|07 is here.\n", N[n].name);
    for (n = 0; n < ni; n++)
        if (!strcmp(I[n].loc, id)) pp(any++ ? ", |14%s|07" : "|08You see:|07 |14%s|07", I[n].name);
    if (any) outc('\n');
    for (any = 0, n = 1; n <= MAXNODES; n++)
        if (n != node && mud_get(n, &r) && !strcmp(r.room, id))
            pp(any++ ? ", |15%s|07" : "|13Also here:|07 |15%s|07", r.name);
    if (any) outc('\n');
}

/* ----- the world tick: run by whichever caller notices it is due ----- */

static void npc_wander(struct nrec *p)
{
    char l[400], dir[8][24], dst[8][24];
    FILE *f = room_open(p->room);
    int c = 0, k;
    if (!f) return;
    while (c < 8 && fgets(l, sizeof l, f))
        if (!strncmp(l, "@exit ", 6) && sscanf(l + 6, "%23s %23s", dir[c], dst[c]) == 2) c++;
    fclose(f);
    if (!c) return;
    k = rand() % c;
    if (!(f = room_open(dst[k]))) return;
    fclose(f);
    mud_send(p->room, 0, "|08%s wanders %s.|07\n", p->name, dir[k]);
    snprintf(p->room, sizeof p->room, "%s", dst[k]);
    mud_send(p->room, 0, "|08%s wanders in.|07\n", p->name);
}

static void npc_talk(int i)
{
    char t[300];
    int c = 0;
    while (def_get(NPCS, i, "say", c, t, sizeof t)) c++;
    if (c && def_get(NPCS, i, "say", rand() % c, t, sizeof t))
        mud_send(N[i].room, 0, "|11%s|07 says, \"%s|07\"\n", N[i].name, t);
}

static void world_tick(void)
{
    static struct mrec P[MAXNODES + 1];
    int hit[MAXNODES + 1], here[MAXNODES], i, n, k, cnt, t, d;
    long now = time(0);

    world_load();
    for (n = 1; n <= MAXNODES; n++) {
        hit[n] = 0;
        if (!mud_get(n, &P[n])) P[n].name[0] = 0;
    }
    for (i = 0; i < nn; i++) {
        struct nrec *p = &N[i];
        if (!p->room[0]) {                          /* dead: come back later */
            if (now < p->back) continue;
            strcpy(p->room, p->home);
            p->hp = p->maxhp;
            p->angry = 0;
            if ((k = item_id(p->drop)) >= 0 && I[k].loc[0] != '#') { strcpy(I[k].loc, "-"); item_save(k); }
            mud_send(p->room, 0, "|08%s flickers into being.|07\n", p->name);
            npc_save(i);
            continue;
        }
        for (cnt = 0, n = 1; n <= MAXNODES; n++)
            if (P[n].name[0] && P[n].hp > 0 && !strcmp(P[n].room, p->room)) here[cnt++] = n;
        for (t = 0, n = 0; n < cnt; n++) if (here[n] == p->angry) t = p->angry;
        if (p->angry && !t) p->angry = 0;           /* its attacker left */
        if (cnt && p->dmg > 0 && (p->hostile || t)) {
            if (!t) t = here[rand() % cnt];
            d = 1 + rand() % p->dmg;
            P[t].hp -= d;
            hit[t] = 1;
            pl_write(t, &P[t]);
            mud_send(p->room, t, "|12%s hits %s.|07\n", p->name, P[t].name);
            {
                char b[120];
                n = snprintf(b, sizeof b, "|12%s hits you for %d!|07 |08(%d hp)|07\n", p->name, d, P[t].hp);
                mud_raw(t, b, n);
            }
        } else {
            k = rand() % 6;
            if (k == 0 && p->wander) npc_wander(p);
            else if (k == 1 && cnt) npc_talk(i);
        }
        npc_save(i);
    }
    for (n = 1; n <= MAXNODES; n++)                 /* rest heals */
        if (P[n].name[0] && !hit[n] && P[n].hp > 0 && P[n].hp < MAXHP) { P[n].hp++; pl_write(n, &P[n]); }
}

/* copy the live world to the SD card once a minute, so a crash or a
 * reboot loses little; world_init() reads it back (lock held) */
static void world_save(int tickfd, long now)
{
    long last = 0;
    int fd;
    if (pread(tickfd, &last, sizeof last, sizeof last) != sizeof last) last = 0;
    if (now >= last && now - last < 60) return;
    pwrite(tickfd, &now, sizeof now, sizeof now);
    if ((fd = open("data/mud_npcs.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
        write(fd, N, nn * sizeof N[0]);
        close(fd);
        rename("data/mud_npcs.tmp", "data/mud_npcs.sav");
    }
    if ((fd = open("data/mud_items.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
        write(fd, I, ni * sizeof I[0]);
        close(fd);
        rename("data/mud_items.tmp", "data/mud_items.sav");
    }
}

/* called a few times a second while we wait for typing */
static void mud_hook(void)
{
    struct mrec r;
    long last = 0, now = time(0);
    char b[200];
    int fd, L, k;

    if ((fd = open(MUDDIR "/tick", O_RDWR | O_CREAT, 0644)) >= 0) {
        if (read(fd, &last, sizeof last) != sizeof last) last = 0;
        if ((now - last >= TICK || now < last) && (L = mlk(0)) >= 0) {
            pread(fd, &last, sizeof last, 0);       /* somebody may have just run it */
            if (now - last >= TICK || now < last) {
                pwrite(fd, &now, sizeof now, 0);
                world_tick();
                world_save(fd, now);
            }
            munlk(L);
        }
        close(fd);
    }
    if (!mud_get(node, &r) || r.hp > 0) return;

    L = mlk(1);                                     /* we died */
    world_load();
    for (k = 0; k < ni; k++)
        if (!strcmp(I[k].loc, myloc)) { snprintf(I[k].loc, sizeof I[k].loc, "%s", r.room); item_save(k); }
    mud_send(r.room, node, "|12%s is derezzed and vanishes!|07\n", me);
    strcpy(r.room, "start");
    strcpy(mroom, "start");
    r.hp = MAXHP;
    pl_write(node, &r);
    munlk(L);
    k = snprintf(b, sizeof b, "\n|12You have been derezzed!|07 Your things clatter to the floor.\n"
                 "|08You reboot back at the start. (|07look|08)|07\n");
    mud_raw(node, b, k);
    mud_send("start", node, "|08%s reboots here, looking dazed.|07\n", me);
}

/* ----- our own record ----- */

/* move to `room`; hp < 0 keeps the health others may have changed */
static void mud_put(const char *room, int hp)
{
    struct mrec r, old;
    int L = mlk(1);
    memset(&r, 0, sizeof r);
    snprintf(r.name, sizeof r.name, "%s", me);
    snprintf(r.room, sizeof r.room, "%s", room);
    r.hp = hp >= 0 ? hp : mud_get(node, &old) ? old.hp : MAXHP;
    pl_write(node, &r);
    strcpy(mroom, r.room);
    munlk(L);
}

/* wipe our slot and put back anything we carried */
static void mud_clear(void)
{
    struct mrec r;
    int L = mlk(1), k;
    world_load();
    for (k = 0; k < ni; k++)
        if (!strcmp(I[k].loc, myloc)) { strcpy(I[k].loc, I[k].home); item_save(k); }
    memset(&r, 0, sizeof r);
    if (L >= 0) pl_write(node, &r);
    munlk(L);
}

/* called on every way out, including a dropped connection */
static void mud_gone(void)
{
    char room[24];
    if (!in_mud || mlocked) return;
    in_mud = 0;
    poll_hook = 0;
    strcpy(room, mroom);
    mud_clear();
    mud_send(room, node, "|08%s fades out of existence.|07\n", me);
}

/* ----- commands ----- */

static const char *longdir(const char *s)
{
    static const char *d[] = { "n", "north", "s", "south", "e", "east", "w", "west", "u", "up", "d", "down",
                               "ne", "northeast", "nw", "northwest", "se", "southeast", "sw", "southwest", 0 };
    int i;
    for (i = 0; d[i]; i += 2)
        if (!strcmp(s, d[i])) return d[i + 1];
    return s;
}

static int mud_go(const char *dir)
{
    char dest[80], *sp;
    FILE *f;
    if (!dir[0] || !room_find(mroom, "@exit", dir, dest, sizeof dest)) return 0;
    if ((sp = strchr(dest, ' '))) *sp = 0;
    if (!(f = room_open(dest))) { pp("|12That way is bricked up (no room \"%s\").|07\n", dest); return 1; }
    fclose(f);
    mud_send(mroom, node, "|08%s heads %s.|07\n", me, dir);
    mud_put(dest, -1);
    mud_send(mroom, node, "|08%s arrives.|07\n", me);
    room_show(mroom);
    return 1;
}

static void mud_look(const char *what)
{
    char t[400];
    struct mrec r;
    int n;
    if (!what[0]) { room_show(mroom); return; }
    world_load();
    if ((n = npc_here(what)) >= 0) {
        if (!def_get(NPCS, n, "desc", 0, t, sizeof t)) strcpy(t, "Hard to describe.");
        emit(t);
        if (N[n].maxhp) pp("|07 |08(%d/%d hp)", N[n].hp, N[n].maxhp);
        emit("|07\n");
        return;
    }
    if ((n = item_at(myloc, what)) >= 0 || (n = item_at(mroom, what)) >= 0) {
        if (!def_get(ITEMS, n, "desc", 0, t, sizeof t)) strcpy(t, "Nothing special.");
        emit(t);
        emit("|07\n");
        return;
    }
    if (room_find(mroom, "@look", what, t, sizeof t)) { emit(t); emit("|07\n"); return; }
    for (n = 1; n <= MAXNODES; n++)
        if (mud_get(n, &r) && !strcmp(r.room, mroom) && has(r.name, what)) {
            pp(n == node ? "That's you. You look slightly pixelated.\n" : "|15%s|07 is here, slightly pixelated.\n", r.name);
            return;
        }
    pp("You see nothing special about that.\n");
}

static void mud_inv(void)
{
    struct mrec r;
    int k, any = 0;
    world_load();
    for (k = 0; k < ni; k++)
        if (!strcmp(I[k].loc, myloc)) pp(any++ ? ", |14%s|07" : "|08You carry:|07 |14%s|07", I[k].name);
    pp(any ? "\n" : "You carry nothing.\n");
    if (mud_get(node, &r)) pp("|08Health:|07 %d/%d\n", r.hp, MAXHP);
}

static void mud_take(const char *what, int drop)
{
    int L = mlk(1), k;
    world_load();
    k = item_at(drop ? myloc : mroom, what);
    if (k >= 0) {
        strcpy(I[k].loc, drop ? mroom : myloc);
        item_save(k);
    }
    munlk(L);
    if (k < 0) { pp(drop ? "You aren't carrying that.\n" : "You don't see that here.\n"); return; }
    pp(drop ? "You drop %s.\n" : "You pick up %s.\n", I[k].name);
    mud_send(mroom, node, drop ? "|08%s drops %s.|07\n" : "|08%s picks up %s.|07\n", me, I[k].name);
}

static void mud_use(const char *what)
{
    struct mrec r;
    int L = mlk(1), k;
    world_load();
    k = item_at(myloc, what);
    if (k >= 0 && I[k].kind == K_HEAL && mud_get(node, &r)) {
        r.hp = r.hp + I[k].val > MAXHP ? MAXHP : r.hp + I[k].val;
        pl_write(node, &r);
        strcpy(I[k].loc, I[k].home);                /* used up: back where it grows */
        item_save(k);
        munlk(L);
        pp("|10You use %s and feel better.|07 |08(%d hp)|07\n", I[k].name, r.hp);
        mud_send(mroom, node, "|08%s uses %s.|07\n", me, I[k].name);
        return;
    }
    munlk(L);
    pp(k < 0 ? "You aren't carrying that.\n" : I[k].kind == K_WEAPON ? "It is a weapon: just |11attack|07 something.\n" : "Nothing happens.\n");
}

static void mud_attack(const char *what)
{
    int L = mlk(1), i, k, d, best = 0;
    world_load();
    i = npc_here(what);
    if (i < 0 || !N[i].maxhp) {
        munlk(L);
        if (i < 0) pp("There is nothing like that to attack here.\n");
        else pp("%s cannot be harmed.\n", N[i].name);
        return;
    }
    for (k = 0; k < ni; k++)
        if (!strcmp(I[k].loc, myloc) && I[k].kind == K_WEAPON && I[k].val > best) best = I[k].val;
    d = 1 + rand() % (3 + best);
    N[i].hp -= d;
    N[i].angry = node;
    if (N[i].hp > 0) {
        pp("|10You hit %s for %d.|07 |08(%d left)|07\n", N[i].name, d, N[i].hp);
        mud_send(mroom, node, "|08%s hits %s.|07\n", me, N[i].name);
    } else {
        pp("|10You hit %s for %d and it collapses into noise!|07\n", N[i].name, d);
        mud_send(mroom, node, "|10%s destroys %s!|07\n", me, N[i].name);
        if ((k = item_id(N[i].drop)) >= 0 && !strcmp(I[k].loc, "-")) {
            strcpy(I[k].loc, mroom);
            item_save(k);
            mud_send(mroom, 0, "|14%s falls to the floor.|07\n", I[k].name);
        }
        N[i].room[0] = 0;
        N[i].angry = 0;
        N[i].back = time(0) + 90;
    }
    npc_save(i);
    munlk(L);
}

static void mud_talk(const char *what)
{
    int i;
    world_load();
    if ((i = npc_here(what)) < 0) pp("Nobody like that is here.\n");
    else npc_talk(i);
}

/* give ITEM [to] NPC: handing an NPC what it "want"s completes its quest */
static void mud_give(char *arg)
{
    char t[400], *to = strchr(arg, ' ');
    int L, k, i, g;
    FILE *f;
    if (!to) { pp("Give what to whom?\n"); return; }
    *to++ = 0;
    while (*to == ' ') to++;
    if (!strncasecmp(to, "to ", 3)) to += 3;
    L = mlk(1);
    world_load();
    k = item_at(myloc, arg);
    i = npc_here(to);
    if (k < 0 || i < 0 || !def_get(NPCS, i, "want", 0, t, sizeof t) || strcmp(t, I[k].id)) {
        munlk(L);
        if (k < 0) pp("You aren't carrying that.\n");
        else if (i < 0) pp("Nobody like that is here.\n");
        else pp("%s has no use for %s.\n", N[i].name, I[k].name);
        return;
    }
    strcpy(I[k].loc, I[k].home);
    item_save(k);
    if (def_get(NPCS, i, "gives", 0, t, sizeof t) && (g = item_id(t)) >= 0) {
        strcpy(I[g].loc, myloc);
        item_save(g);
    }
    munlk(L);
    pp("You give %s to %s.\n", I[k].name, N[i].name);
    if (def_get(NPCS, i, "reward", 0, t, sizeof t)) pp("|11%s|07 says, \"%s|07\"\n", N[i].name, t);
    pp("|14*** Quest complete! ***|07\n");
    mud_send(0, node, "|14*** %s has completed %s's quest! ***|07\n", me, N[i].name);
    if ((f = fopen("data/mud_heroes.log", "a"))) {
        fprintf(f, "  |15%-12s|07  |08%s|07\n", me, stamp(time(0)));
        fclose(f);
    }
}

static void mud(void)
{
    char in[200], inbox[40], t[80], *arg;
    struct mrec r;
    long pos = 0;
    int fd, n;
    FILE *f;

    if (!(f = room_open("start"))) { pp("\n|12The MUD has no world yet (mud/start.txt).|07\n"); return; }
    fclose(f);
    mkdir(MUDDIR, 0755);
    fd = mlk(1);
    world_init();
    munlk(fd);
    srand(time(0) ^ getpid());
    sprintf(inbox, MUDDIR "/in.%d", node);
    if ((fd = open(inbox, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) close(fd);
    pp("\033[2J\033[H|14-=[ The MUD ]=-|07  |08type |07help|08 for commands, |07quit|08 to leave|07\n");
    mud_put("start", MAXHP);
    in_mud = 1;
    poll_hook = mud_hook;
    mud_send(mroom, node, "|08%s shimmers into existence.|07\n", me);
    room_show(mroom);

    for (;;) {
        tail_getln(inbox, &pos, "|10>|07 ", in, sizeof in - 1);
        if (pos > 8192) { truncate(inbox, 0); pos = 0; }
        if (!in[0]) continue;
        pp("|08> %s|07\n", in);
        if (in[0] == '\'' || in[0] == ':') {            /* 'hello  and  :waves */
            memmove(in + 2, in + 1, strlen(in));
            in[1] = ' ';
        }
        for (arg = in; *arg && *arg != ' '; arg++) *arg = tolower((unsigned char)*arg);
        while (*arg == ' ') *arg++ = 0;

        if (!strcmp(in, "quit") || !strcmp(in, "q")) break;
        if (!strcmp(in, "look") || !strcmp(in, "l") || !strcmp(in, "examine") || !strcmp(in, "x")) mud_look(arg);
        else if (!strcmp(in, "say") || !strcmp(in, "'")) {
            if (!arg[0]) { pp("Say what?\n"); continue; }
            pp("You say, \"%s|07\"\n", arg);
            mud_send(mroom, node, "|15%s|07 says, \"%s|07\"\n", me, arg);
        } else if (!strcmp(in, "emote") || !strcmp(in, "me") || !strcmp(in, ":")) {
            pp("|13%s %s|07\n", me, arg);
            mud_send(mroom, node, "|13%s %s|07\n", me, arg);
        } else if (!strcmp(in, "shout")) {
            pp("You shout, \"%s|07\"\n", arg);
            mud_send(0, node, "|12%s shouts, \"%s|12\"|07\n", me, arg);
        } else if (!strcmp(in, "who")) {
            for (n = 1; n <= MAXNODES; n++) {
                if (!mud_get(n, &r)) continue;
                if (!room_find(r.room, "@name", 0, t, sizeof t)) strcpy(t, "somewhere");
                pp("  |15%-12s|07  %s\n", r.name, t);
            }
        } else if (!strcmp(in, "inventory") || !strcmp(in, "inv") || !strcmp(in, "i") || !strcmp(in, "hp")) mud_inv();
        else if (!strcmp(in, "get") || !strcmp(in, "take")) mud_take(arg, 0);
        else if (!strcmp(in, "drop")) mud_take(arg, 1);
        else if (!strcmp(in, "use") || !strcmp(in, "eat")) mud_use(arg);
        else if (!strcmp(in, "attack") || !strcmp(in, "kill") || !strcmp(in, "hit") || !strcmp(in, "a")) mud_attack(arg);
        else if (!strcmp(in, "talk")) mud_talk(!strncasecmp(arg, "to ", 3) ? arg + 3 : arg);
        else if (!strcmp(in, "give")) mud_give(arg);
        else if (!strcmp(in, "heroes")) { pp("|09Quest heroes|07\n"); tail("data/mud_heroes.log", 500); }
        else if (!strcmp(in, "help") || !strcmp(in, "?")) {
            pp("|11look|07 [thing]   |11n s e w u d|07 or an exit name\n"
               "|11say|07 text (|11'|07text)   |11emote|07 text (|11:|07text)   |11shout|07 text   |11who|07\n"
               "|11get|07 / |11drop|07 / |11use|07 item   |11inv|07 (items and health)\n"
               "|11talk|07 creature   |11give|07 item creature   |11attack|07 creature\n"
               "|11heroes|07   |11quit|07\n");
        } else if (!strcmp(in, "go")) {
            for (n = 0; arg[n]; n++) arg[n] = tolower((unsigned char)arg[n]);
            if (!mud_go(longdir(arg))) pp("|12You can't go that way.|07\n");
        } else if (!mud_go(longdir(in))) pp("|08Nothing happens. (|07help|08 lists commands)|07\n");
    }
    mud_gone();
    unlink(inbox);
}

/* ---------- bulletin board: one file per post in data/posts ---------- */

#define MAXPOSTS 200

static int cmpint(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

/* newest MAXPOSTS ids, ascending */
static int post_ids(int *ids)
{
    DIR *d = opendir("data/posts");
    struct dirent *e;
    int n = 0, id, i, lo;
    if (!d) return 0;
    while ((e = readdir(d))) {
        if ((id = atoi(e->d_name)) <= 0) continue;
        if (n < MAXPOSTS) { ids[n++] = id; continue; }
        for (lo = 0, i = 1; i < n; i++) if (ids[i] < ids[lo]) lo = i;
        if (id > ids[lo]) ids[lo] = id;
    }
    closedir(d);
    qsort(ids, n, sizeof *ids, cmpint);
    return n;
}

/* open a post and read its header; the stream is left at the body */
static FILE *post_open(int id, char *from, char *date, char *subj)
{
    char p[40], l[256];
    FILE *f;
    sprintf(p, "data/posts/%04d.txt", id);
    if (!(f = fopen(p, "r"))) return 0;
    from[0] = date[0] = subj[0] = 0;
    while (fgets(l, sizeof l, f)) {
        chomp(l);
        if (!l[0]) break;
        if (!strncmp(l, "From: ", 6)) snprintf(from, 16, "%s", l + 6);
        else if (!strncmp(l, "Date: ", 6)) snprintf(date, 20, "%s", l + 6);
        else if (!strncmp(l, "Subj: ", 6)) snprintf(subj, 64, "%s", l + 6);
    }
    return f;
}

static void post_list(void)
{
    static int ids[MAXPOSTS];
    char from[16], date[20], subj[64];
    int n = post_ids(ids), i, ln = 0;
    FILE *f;
    if (!n) { pp("\n  |08No bulletins yet - be the first to post!|07\n"); return; }
    pp("\n|09   #  Date              From          Subject|07\n");
    for (i = 0; i < n; i++) {
        if (!(f = post_open(ids[i], from, date, subj))) continue;
        fclose(f);
        pp("|14%4d|07  %-16s  |11%-12s|07  %s|07\n", ids[i], date, from, subj);
        if (more(&ln)) break;
    }
}

static void post_read(int id)
{
    char from[16], date[20], subj[64], l[256];
    int ln = 4;
    FILE *f = post_open(id, from, date, subj);
    if (!f) { pp("|12No such bulletin.|07\n"); return; }
    pp("\n|09-=[ |15#%d|09 ]=- |15%s|07\n|08From |11%s|08 on %s|07\n\n", id, subj, from, date);
    while (fgets(l, sizeof l, f)) {
        emit(l);
        if (more(&ln)) break;
    }
    fclose(f);
}

static void post_new(void)
{
    static int ids[MAXPOSTS];
    static char body[4096];
    char subj[64], l[100], p[40];
    int n, id, fd = -1, len = 0, tries;
    FILE *f;

    pp("|11Subject:|07 ");
    if (!getln(subj, sizeof subj)) { pp("|08Cancelled.|07\n"); return; }
    pp("|08Type your message. A line with just \".\" saves, \"/a\" aborts.|07\n");
    for (;;) {
        pp("|08:|07 ");
        getln(l, 79);
        if (!strcmp(l, ".")) break;
        if (!strcmp(l, "/a")) { pp("|08Aborted.|07\n"); return; }
        if (len + (int)strlen(l) + 2 > (int)sizeof body) { pp("|12Message full - saving.|07\n"); break; }
        len += sprintf(body + len, "%s\n", l);
    }
    if (!len) { pp("|08Empty message, not posted.|07\n"); return; }
    n = post_ids(ids);
    id = n ? ids[n - 1] + 1 : 1;
    for (tries = 0; tries < 50 && fd < 0; tries++, id++) {
        sprintf(p, "data/posts/%04d.txt", id);
        fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
    }
    if (fd < 0 || !(f = fdopen(fd, "w"))) { pp("|12Could not save the bulletin.|07\n"); return; }
    fprintf(f, "From: %s\nDate: %s\nSubj: %s\n\n%s", me, stamp(time(0)), subj, body);
    fclose(f);
    pp("|10Posted as #%d.|07\n", id - 1);
}

static void post_del(int id)
{
    char from[16], date[20], subj[64], p[40];
    FILE *f = post_open(id, from, date, subj);
    if (!f) { pp("|12No such bulletin.|07\n"); return; }
    fclose(f);
    if (strcasecmp(from, me) && !is_sysop()) { pp("|12Only %s or the sysop can delete that.|07\n", from); return; }
    sprintf(p, "data/posts/%04d.txt", id);
    pp(unlink(p) ? "|12Delete failed.|07\n" : "|10Deleted.|07\n");
}

static void board(void)
{
    char l[32];
    post_list();
    for (;;) {
        pp("\n|09Bulletins|07  |14#|07 read  |14L|07ist  |14P|07ost  |14D|07 # delete  |14Q|07uit\n|11>|07 ");
        getln(l, sizeof l);
        switch (tolower((unsigned char)l[0])) {
        case 'q': case 0: return;
        case 'l': post_list(); break;
        case 'p': post_new(); break;
        case 'd': post_del(atoi(l + 1)); break;
        default:
            if (isdigit((unsigned char)l[0])) post_read(atoi(l));
        }
    }
}

/* ---------- files: data/files, moved with "ssh bbs@host get|put NAME" ---------- */

static int okname(const char *s)
{
    int n = strlen(s), i;
    if (n < 1 || n > 40 || s[0] == '.') return 0;
    for (i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && !strchr("._-", s[i])) return 0;
    return n < 5 || strcmp(s + n - 5, ".part");
}

static void owner_of(const char *name, char *out)
{
    char l[128], a[64], b[16];
    FILE *f = fopen("data/files.idx", "r");
    strcpy(out, "-");
    if (!f) return;
    while (fgets(l, sizeof l, f))
        if (sscanf(l, "%63s %15s", a, b) == 2 && !strcasecmp(a, name)) strcpy(out, b);
    fclose(f);
}

/* tty != 0: coloured and paged; otherwise plain text for "ssh ... ls" */
static void file_list(int tty)
{
    char p[80], own[16];
    struct stat st;
    struct dirent *e;
    DIR *d = opendir("data/files");
    int ln = 0, cnt = 0;
    if (tty) pp("\n|09Name                                Size  Date              From|07\n");
    while (d && (e = readdir(d))) {
        if (!okname(e->d_name)) continue;
        snprintf(p, sizeof p, "data/files/%s", e->d_name);
        if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        owner_of(e->d_name, own);
        cnt++;
        if (!tty) {
            char b[160];
            snprintf(b, sizeof b, "%-30s %10ld  %s  %s\n", e->d_name, (long)st.st_size, stamp(st.st_mtime), own);
            outs(b);
            continue;
        }
        pp("|15%-30s|07 %9ld  %s  |11%s|07\n", e->d_name, (long)st.st_size, stamp(st.st_mtime), own);
        if (more(&ln)) break;
    }
    if (d) closedir(d);
    if (!cnt && tty) pp("  |08No files yet.|07\n");
    oflush();
}

/* telnet has no side channel, so text files are uploaded by pasting */
static void file_paste(const char *name)
{
    char p[80], l[256];
    long total = 0;
    int fd;
    FILE *f;
    snprintf(p, sizeof p, "data/files/%s", name);
    if (!okname(name)) { pp("|12Usage: P name   (letters, digits, . _ -)|07\n"); return; }
    if ((fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0) { pp("|12That name is taken.|07\n"); return; }
    pp("|08Paste or type the text. A line with just \".\" saves, \"/a\" aborts.|07\n");
    for (;;) {
        getln(l, sizeof l);
        if (!strcmp(l, ".")) break;
        if (!strcmp(l, "/a") || (total += strlen(l) + 1) > 65536) {
            close(fd);
            unlink(p);
            pp("|08Upload dropped (aborted, or over 64 KB).|07\n");
            return;
        }
        write(fd, l, strlen(l));
        write(fd, "\n", 1);
    }
    close(fd);
    if ((f = fopen("data/files.idx", "a"))) { fprintf(f, "%s %s\n", name, me); fclose(f); }
    pp("|10Stored %s (%ld bytes).|07\n", name, total);
}


static void file_view(const char *name)
{
    char p[80], l[256];
    int ln = 0, n, i;
    FILE *f;
    snprintf(p, sizeof p, "data/files/%s", name);
    if (!okname(name) || !(f = fopen(p, "r"))) { pp("|12No such file.|07\n"); return; }
    n = fread(l, 1, sizeof l, f);
    for (i = 0; i < n; i++)
        if (!l[i]) { pp("|12That is a binary file - it cannot be shown.|07\n"); fclose(f); return; }
    rewind(f);
    while (fgets(l, sizeof l, f)) {
        for (i = 0; l[i]; i++)
            if ((unsigned char)l[i] < 32 && l[i] != '\n' && l[i] != '\t') l[i] = ' ';
        outs(l);
        if (more(&ln)) break;
    }
    fclose(f);
}

static void file_del(const char *name)
{
    char p[80], own[16];
    snprintf(p, sizeof p, "data/files/%s", name);
    if (!okname(name) || access(p, F_OK)) { pp("|12No such file.|07\n"); return; }
    owner_of(name, own);
    if (strcasecmp(own, me) && !is_sysop()) { pp("|12Only the uploader or the sysop can delete that.|07\n"); return; }
    pp(unlink(p) ? "|12Delete failed.|07\n" : "|10Deleted.|07\n");
}

/* ---------- XMODEM / YMODEM over telnet ----------
 * Telnet is switched to binary for the transfer; 0xFF is doubled on the way
 * out and un-doubled on the way in.  Uploads go straight to the SD card a
 * block at a time (fsync every 32 blocks), never through a RAM buffer. */

#define SOH 1
#define STX 2
#define EOT 4
#define ACK 6
#define NAK 21
#define CAN 24

static unsigned char ib[512];
static int ih, it;

static int xraw(int ms)
{
    fd_set r;
    struct timeval tv;
    int n;
    if (ih < it) return ib[ih++];
    FD_ZERO(&r);
    FD_SET(0, &r);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (select(1, &r, 0, 0, &tv) <= 0) return -1;
    if ((n = read(0, ib, sizeof ib)) <= 0) bye(0);
    ih = 1;
    it = n;
    return ib[0];
}

/* next data byte, or -1 on timeout; telnet commands are skipped */
static int xget(int ms)
{
    int c, d;
    for (;;) {
        if ((c = xraw(ms)) != 255) return c;
        if ((c = xraw(ms)) == 255 || c < 0) return c;       /* IAC IAC = 0xFF */
        if (c >= 251 && c <= 254) xraw(ms);                 /* WILL/WONT/DO/DONT x */
        else if (c == 250)                                  /* SB ... IAC SE */
            for (d = 0; (c = xraw(ms)) >= 0 && !(d == 255 && c == 240); d = c) ;
    }
}

static void xput(const unsigned char *b, int n)
{
    static unsigned char e[2200];
    int i, m = 0, o = 0, w;
    for (i = 0; i < n; i++) {
        if (b[i] == 255) e[m++] = 255;
        e[m++] = b[i];
    }
    while (o < m) {
        w = write(1, e + o, m - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) onsig(0);
        o += w;
    }
}

static void xc(int c)
{
    unsigned char b = c;
    xput(&b, 1);
}

static void xdrain(void)
{
    while (xget(300) >= 0) ;
}

static void xbinary(int on)
{
    unsigned char b[6] = { 255, 0, 0, 255, 0, 0 };          /* IAC WILL/WONT BINARY, IAC DO/DONT BINARY */
    b[1] = on ? 251 : 252;
    b[4] = on ? 253 : 254;
    oflush();
    write(1, b, 6);
    ih = it = 0;
}

static unsigned crc16(const unsigned char *b, int n)
{
    unsigned c = 0;
    int i;
    while (n--) {
        c ^= (unsigned)*b++ << 8;
        for (i = 0; i < 8; i++) c = c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1;
    }
    return c & 0xFFFF;
}

/* send one block and wait for the verdict: 1 = ACKed, 0 = gave up */
static int xsendblk(int blkno, const unsigned char *data, int len, int crc)
{
    static unsigned char p[1030];
    unsigned k = 0;
    int i, c, tries;
    p[0] = len == 1024 ? STX : SOH;
    p[1] = blkno;
    p[2] = ~blkno;
    memcpy(p + 3, data, len);
    if (crc) {
        k = crc16(data, len);
        p[3 + len] = k >> 8;
        p[4 + len] = k;
    } else {
        for (i = 0; i < len; i++) k += data[i];
        p[3 + len] = k;
    }
    for (tries = 0; tries < 10; tries++) {
        xput(p, len + (crc ? 5 : 4));
        for (;;) {
            c = xget(10000);
            if (c == ACK) return 1;
            if (c == CAN && xget(1000) == CAN) return 0;
            if (c == NAK || c < 0) break;                   /* resend */
        }
    }
    return 0;
}

static int xsend(const char *path, const char *name, int ymodem)
{
    static unsigned char d[1024];
    struct stat st;
    int fd, c = -1, crc, n, m, i, blk = 1, bs = ymodem ? 1024 : 128, ok = 0;

    if ((fd = open(path, O_RDONLY)) < 0) return 0;
    fstat(fd, &st);
    for (i = 0; i < 60 && c != 'C' && c != NAK && c != CAN; i++) c = xget(1000);
    if (c != 'C' && c != NAK) { close(fd); return 0; }
    crc = c == 'C';
    if (ymodem) {                                           /* block 0: name and size */
        memset(d, 0, 128);
        n = snprintf((char *)d, 100, "%s", name);
        snprintf((char *)d + n + 1, 20, "%ld", (long)st.st_size);
        if (!xsendblk(0, d, 128, crc)) goto out;
        for (c = -1, i = 0; i < 20 && c != 'C' && c != NAK; i++) c = xget(1000);
    }
    while ((n = read(fd, d, bs)) > 0) {
        while (n < bs && (m = read(fd, d + n, bs - n)) > 0) n += m;
        if (n < bs) memset(d + n, 0x1A, bs - n);
        if (!xsendblk(blk++ & 255, d, bs, crc)) goto out;
    }
    for (i = 0; i < 10 && !ok; i++) {
        xc(EOT);
        ok = xget(5000) == ACK;
    }
    if (ok && ymodem) {                                     /* empty block 0 ends the batch */
        for (c = -1, i = 0; i < 10 && c != 'C'; i++) c = xget(1000);
        memset(d, 0, 128);
        xsendblk(0, d, 128, 1);
    }
out:
    close(fd);
    if (!ok) { xc(CAN); xc(CAN); }
    return ok;
}

/* receive into fd (XMODEM-CRC, XMODEM-1K or YMODEM, whichever the caller
 * starts); returns the byte count or -1 */
static long xrecv(int fd, long maxbytes)
{
    static unsigned char d[1028];
    long total = 0, size = -1;
    unsigned k;
    int c, b, i, len, expect = 1, tries = 0, started = 0, ymodem = 0;

    for (;;) {
        if (!started) {
            if (tries++ >= 20) return -1;
            xc('C');
            if ((c = xget(3000)) < 0) continue;
        } else if ((c = xget(10000)) < 0) {
            if (++tries > 10) return -1;
            xc(NAK);
            continue;
        }
        if (c == SOH || c == STX) {
            len = c == SOH ? 128 : 1024;
            for (i = 0; i < len + 4 && (b = xget(2000)) >= 0; i++) d[i] = b;
            k = crc16(d + 2, len);
            if (i < len + 4 || d[0] != (255 & ~d[1]) || (k >> 8) != d[len + 2] || (k & 255) != d[len + 3]) {
                xdrain();
                if (started) xc(NAK);
                continue;
            }
            tries = 0;
            if (!started && d[0] == 0) {                    /* YMODEM block 0 */
                started = ymodem = 1;
                if (!d[2]) { xc(ACK); return -1; }          /* empty batch */
                size = atol((char *)d + 2 + strlen((char *)d + 2) + 1);
                xc(ACK);
                xc('C');
                continue;
            }
            started = 1;
            if (d[0] == (expect & 255)) {
                if (total + len > maxbytes || write(fd, d + 2, len) != len) { xc(CAN); xc(CAN); return -1; }
                total += len;
                if (!(++expect & 31)) fsync(fd);
                xc(ACK);
            } else if (d[0] == ((expect - 1) & 255)) xc(ACK);   /* duplicate */
            else { xc(CAN); xc(CAN); return -1; }
        } else if (c == EOT) {
            xc(ACK);
            if (ymodem) {                                   /* swallow the end-of-batch block */
                xc('C');
                if ((c = xget(5000)) == SOH || c == STX) {
                    len = c == SOH ? 128 : 1024;
                    for (i = 0; i < len + 4 && xget(2000) >= 0; i++) ;
                    xc(ACK);
                }
            }
            break;
        } else if (c == CAN && xget(1000) == CAN) return -1;
    }
    if (size >= 0 && size <= total) total = size;           /* YMODEM told us the real length */
    else if (!ymodem) {                                     /* XMODEM pads the tail with ^Z */
        i = total < 1024 ? total : 1024;
        if (pread(fd, d, i, total - i) == i)
            while (i > 0 && d[i - 1] == 0x1A) { i--; total--; }
    }
    ftruncate(fd, total);
    fsync(fd);
    return total;
}

static void file_upload(const char *name)
{
    char p[80], tmp[100];
    long n;
    int fd;
    FILE *f;
    snprintf(p, sizeof p, "data/files/%s", name);
    if (!okname(name)) { pp("|12Usage: U name   (letters, digits, . _ -)|07\n"); return; }
    if (!access(p, F_OK)) { pp("|12That name is taken.|07\n"); return; }
    snprintf(tmp, sizeof tmp, "%s.%d.part", p, (int)getpid());
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) { pp("|12Cannot create the file.|07\n"); return; }
    pp("|14Start an XMODEM or YMODEM upload in your terminal now|07 |08(waiting a minute; max %ld KB)|07\n", max_upload_kb);
    xbinary(1);
    n = xrecv(fd, max_upload_kb * 1024);
    close(fd);
    xdrain();
    xbinary(0);
    if (n <= 0 || !access(p, F_OK) || rename(tmp, p)) {
        unlink(tmp);
        pp("\n|12Upload failed or cancelled.|07\n");
        return;
    }
    if ((f = fopen("data/files.idx", "a"))) { fprintf(f, "%s %s\n", name, me); fclose(f); }
    pp("\n|10Stored %s (%ld bytes).|07\n", name, n);
}

static void file_send(const char *name)
{
    char p[80];
    int k, ok;
    snprintf(p, sizeof p, "data/files/%s", name);
    if (!okname(name) || access(p, F_OK)) { pp("|12No such file.|07\n"); return; }
    pp("|11Protocol:|07 |14X|07modem or |14Y|07modem? ");
    k = tolower(waitkey());
    outc('\n');
    if (k != 'x' && k != 'y') return;
    pp("|14Start the %s download in your terminal now|07 |08(waiting a minute)|07\n", k == 'y' ? "YMODEM" : "XMODEM");
    xbinary(1);
    ok = xsend(p, name, k == 'y');
    xdrain();
    xbinary(0);
    pp(ok ? "\n|10Sent.|07\n" : "\n|12Transfer failed or cancelled.|07\n");
}

static void files(void)
{
    char l[64], *arg;
    file_list(1);
    for (;;) {
        pp("\n|09Files|07  |14L|07ist  |14V|07iew name  |14G|07et name  |14U|07pload name  |14P|07aste-text name  |14D|07elete name  |14Q|07uit\n|11>|07 ");
        getln(l, sizeof l);
        for (arg = l + (l[0] != 0); *arg == ' '; arg++) ;
        switch (tolower((unsigned char)l[0])) {
        case 'q': case 0: return;
        case 'l': file_list(1); break;
        case 'u': file_upload(arg); break;
        case 'g': file_send(arg); break;
        case 'p': file_paste(arg); break;
        case 'v': file_view(arg); break;
        case 'd': file_del(arg); break;
        }
    }
}

/* ---------- accounts: data/users holds "handle:crypt-hash" lines ---------- */

static int user_find(const char *h, char *canon, char *hash)
{
    char l[200], *c;
    FILE *f = fopen("data/users", "r");
    int hit = 0;
    if (!f) return 0;
    while (!hit && fgets(l, sizeof l, f)) {
        chomp(l);
        if (!(c = strchr(l, ':'))) continue;
        *c++ = 0;
        if (strcasecmp(l, h)) continue;
        snprintf(canon, 16, "%s", l);
        snprintf(hash, 128, "%s", c);
        hit = 1;
    }
    fclose(f);
    return hit;
}

static void getpw(const char *prompt, char *b, int max)
{
    emit(prompt);
    hide = 1;
    getln(b, max);
    hide = 0;
    oflush();
}

/* returns 1 when a new account was made (and we are logged in as it) */
static int newuser(void)
{
    static const char sc[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";
    char h[32], pw[64], pw2[64], canon[16], hash[128], set[40], *c;
    unsigned char rnd[12];
    int i, n, ok, g, fd;
    FILE *f = 0;

    pp("\n|14New caller sign-up|07\n"
       "|08Telnet is not encrypted: use a password you use nowhere else.|07\n"
       "|11Pick a handle|07 (3-12 letters/digits): ");
    if (!getln(h, sizeof h)) return 0;
    ok = strlen(h) >= 3 && strlen(h) <= 12 && isalpha((unsigned char)h[0]);
    for (i = 0; h[i]; i++) if (!isalnum((unsigned char)h[i])) ok = 0;
    if (!ok) { pp("|12Letters and digits only, starting with a letter.|07\n"); return 0; }
    if (!strcasecmp(h, "new") || !strcasecmp(h, sysop) || user_find(h, canon, hash)) {
        pp("|12That handle is taken.|07\n");
        return 0;
    }
    getpw("|11Choose a password|07 (6+ characters): ", pw, sizeof pw);
    getpw("|11Again:|07 ", pw2, sizeof pw2);
    if (strlen(pw) < 6 || strcmp(pw, pw2)) { pp("|12Too short, or the two did not match.|07\n"); return 0; }

    fd = open("/dev/urandom", O_RDONLY);
    n = fd < 0 ? 0 : read(fd, rnd, sizeof rnd);
    if (fd >= 0) close(fd);
    strcpy(set, "$5$rounds=1000$");
    i = strlen(set);
    for (g = 0; g < 12; g++) set[i + g] = sc[rnd[g] & 63];
    set[i + 12] = 0;
    c = n == sizeof rnd ? crypt(pw, set) : 0;
    ok = c && c[0] == '$';
    if (ok) {
        g = glock();
        ok = !user_find(h, canon, hash) && (f = fopen("data/users", "a"));
        if (ok) { fprintf(f, "%s:%s\n", h, c); fclose(f); }
        if (g >= 0) close(g);
    }
    if (!ok) { pp("|12Sign-up failed - please try again.|07\n"); return 0; }
    snprintf(me, sizeof me, "%s", h);
    pp("|10Welcome aboard, %s!|07\n", me);
    return 1;
}

static void login(void)
{
    char h[32], pw[64], canon[16], hash[128], *c;
    int tries, ok = 0, keep = idle_secs;

    idle_secs = 90;                         /* don't let a silent socket hold a line */
    pp("\n|15%s|07  |08node %d of %d|07\n\n", bbsname, node, max_users);
    for (tries = 0; tries < 4 && !ok; tries++) {
        pp("|11Handle|07 (or |14NEW|07): ");
        if (!getln(h, sizeof h)) continue;
        if (!strcasecmp(h, "new")) { ok = newuser(); continue; }
        getpw("|11Password:|07 ", pw, sizeof pw);
        if (user_find(h, canon, hash) && (c = crypt(pw, hash)) && !strcmp(c, hash)) {
            snprintf(me, sizeof me, "%s", canon);
            ok = 1;
        } else {
            sleep(1);
            pp("|12Login incorrect.|07\n");
        }
    }
    if (!ok) { pp("|12Too many tries.|07\n"); bye(0); }
    idle_secs = keep;
    node_mark();
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    char *home = getenv("BBS_HOME");
    int k;
    FILE *f;

    if (chdir(home ? home : BBSDIR)) { perror("bbs: chdir"); return 1; }
    loadconf();
    mkdir("data", 0755);
    mkdir("data/nodes", 0755);
    mkdir("data/posts", 0755);
    mkdir("data/files", 0755);
    if (argc > 1 && !strcmp(argv[1], "-w")) {       /* bbsctl who */
        pp("%d of %d lines in use\n", who(1), max_users);
        oflush();
        return 0;
    }

    signal(SIGHUP, onsig);
    signal(SIGTERM, onsig);
    signal(SIGPIPE, onsig);
    telnet = 1;
    outs("\377\373\001\377\373\003");       /* IAC WILL ECHO, IAC WILL SGA: character mode */
    outs("\377\375\030");                   /* IAC DO TTYPE: to pick UTF-8 or CP437 */
    strcpy(me, "-login-");
    if (!node_take()) {
        pp("\n  All %d lines are busy - please call back later.\n\n", max_users);
        bye(1);
    }
    sprintf(myloc, "#%d", node);
    mud_clear();                            /* in case the last caller on this node dropped */
    login();
    if (cp437 < 0) {                        /* terminal type told us nothing: ask */
        pp("\n|07Is there one solid block between the brackets?  [\342\226\210]  (|14Y|07/|14N|07) ");
        k = tolower(waitkey());
        cp437 = k != 'y';
        outc('\n');
    }

    outs("\033[2J\033[H");
    showfile("welcome.txt", 30);
    if ((f = fopen("data/callers.log", "a"))) {
        fprintf(f, "  |15%-12s|07  node %d  |08%s|07\n", me, node, stamp(time(0)));
        fclose(f);
    }
    pause_key();

    for (;;) {
        pp("\n|09-=[ |15%s|09 ]=-|07  node %d of %d  |08-|07  %d online\n"
           " |14B|07ulletins  |14C|07hat  |14F|07iles  |14M|07UD  |14W|07ho  |14L|07ast callers  |14G|07oodbye\n|11Command:|07 ",
           bbsname, node, max_users, who(0));
        k = tolower(waitkey());
        if (k >= 32 && k < 127) outc(k);
        outc('\n');
        switch (k) {
        case 'b': board(); break;
        case 'c': chat(); break;
        case 'f': files(); break;
        case 'm': mud(); break;
        case 'w': pp("\n"); who(1); break;
        case 'l': pp("\n|09Last callers|07\n"); tail("data/callers.log", 500); break;
        case 'g': case 'q':
            pp("\n|14Thanks for calling, %s.|07\n\n|08NO CARRIER|07\n", me);
            bye(0);
        }
    }
}
