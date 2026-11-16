/* libpio's PIO assembler: pioasm's language, RP2350 (PIO version 1)
 * instructions included. pio_assemble() is in pio.h; pioasm-main.c is the
 * command. */
#define _GNU_SOURCE
#include "pio.h"
#include "pioasm.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { T_END, T_ID, T_NUM, T_OP };

typedef struct {
    int type;
    char s[32];
    double num;
} tok_t;

typedef struct {
    int lineno;
    int ntok;
    tok_t *tok;
    const char *text; /* `% c-sdk` lines: the line itself */
    int verbatim;     /* 1: inside a % c-sdk block */
} line_t;

typedef struct {
    char name[32];
    int value;
    int pub;
    int label;
} sym_t;

#define MAX_SYMS 128

typedef struct {
    char *err;
    size_t errlen;
    int failed;
    line_t *lines;
    int nlines;
    /* symbols: file-wide ones first (nglobal), then the program's */
    sym_t syms[MAX_SYMS];
    int nsyms, nglobal;
    /* the line being parsed */
    line_t *ln;
    int pos;
} asm_t;

static void fail(asm_t *a, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (a->failed)
        return;
    a->failed = 1;
    if (!a->err || !a->errlen)
        return;
    n = snprintf(a->err, a->errlen, "line %d: ", a->ln ? a->ln->lineno : 0);
    if (n < 0 || (size_t)n >= a->errlen)
        return;
    va_start(ap, fmt);
    vsnprintf(a->err + n, a->errlen - n, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------ lexing */

static int tokenize(const char *s, int lineno, line_t *out, int *in_comment)
{
    int cap = 8, n = 0;
    tok_t *t = malloc(cap * sizeof(*t));

    if (!t)
        return -1;
    out->lineno = lineno;
    out->text = s;
    out->verbatim = 0;
    while (*s) {
        tok_t k;

        if (*in_comment) {
            const char *e = strstr(s, "*/");
            if (!e)
                break;
            s = e + 2;
            *in_comment = 0;
            continue;
        }
        if (isspace((unsigned char)*s)) {
            s++;
            continue;
        }
        if ((s[0] == '/' && s[1] == '/') || s[0] == ';')
            break;
        if (s[0] == '/' && s[1] == '*') {
            *in_comment = 1;
            s += 2;
            continue;
        }
        memset(&k, 0, sizeof(k));
        if (isalpha((unsigned char)*s) || *s == '_' || (*s == '.' && isalpha((unsigned char)s[1]))) {
            int i = 0;
            k.type = T_ID;
            k.s[i++] = *s++;
            while ((isalnum((unsigned char)*s) || *s == '_') && i < 31)
                k.s[i++] = *s++;
            while (isalnum((unsigned char)*s) || *s == '_')
                s++;
        } else if (isdigit((unsigned char)*s)) {
            char *e;
            k.type = T_NUM;
            if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
                k.num = (double)strtoull(s + 2, &e, 2);
            } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                k.num = (double)strtoull(s, &e, 16);
            } else {
                k.num = strtod(s, &e);
            }
            s = e;
        } else {
            static const char *two[] = { "--", "!=", "::", "<<", ">>", NULL };
            k.type = T_OP;
            k.s[0] = *s;
            for (int i = 0; two[i]; i++)
                if (s[0] == two[i][0] && s[1] == two[i][1])
                    k.s[1] = s[1];
            s += k.s[1] ? 2 : 1;
        }
        if (n + 2 > cap) {
            tok_t *nt = realloc(t, (cap *= 2) * sizeof(*t));
            if (!nt) {
                free(t);
                return -1;
            }
            t = nt;
        }
        t[n++] = k;
    }
    memset(&t[n], 0, sizeof(t[n]));
    t[n].type = T_END;
    out->tok = t;
    out->ntok = n;
    return 0;
}

static tok_t *peek(asm_t *a) { return &a->ln->tok[a->pos]; }
static tok_t *next(asm_t *a)
{
    tok_t *t = &a->ln->tok[a->pos];
    if (t->type != T_END)
        a->pos++;
    return t;
}
static int is_id(tok_t *t, const char *s) { return t->type == T_ID && !strcasecmp(t->s, s); }
static int is_op(tok_t *t, const char *s) { return t->type == T_OP && !strcmp(t->s, s); }
static int accept_id(asm_t *a, const char *s)
{
    if (is_id(peek(a), s)) {
        a->pos++;
        return 1;
    }
    return 0;
}
static int accept_op(asm_t *a, const char *s)
{
    if (is_op(peek(a), s)) {
        a->pos++;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ symbols and expressions */

static sym_t *find_sym(asm_t *a, const char *name)
{
    /* the program's own first, then the file's */
    for (int i = a->nsyms - 1; i >= 0; i--)
        if (!strcmp(a->syms[i].name, name))
            return &a->syms[i];
    return NULL;
}

static sym_t *add_sym(asm_t *a, const char *name, int value, int pub, int label)
{
    for (int i = a->nglobal; i < a->nsyms; i++)
        if (!strcmp(a->syms[i].name, name)) {
            fail(a, "'%s' is already defined", name);
            return NULL;
        }
    if (a->nsyms >= MAX_SYMS) {
        fail(a, "too many symbols");
        return NULL;
    }
    sym_t *s = &a->syms[a->nsyms++];
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->value = value;
    s->pub = pub;
    s->label = label;
    return s;
}

static double expr(asm_t *a);

static double primary(asm_t *a)
{
    tok_t *t = next(a);

    if (t->type == T_NUM)
        return t->num;
    if (t->type == T_ID) {
        sym_t *s = find_sym(a, t->s);
        if (!s) {
            fail(a, "unknown symbol '%s'", t->s);
            return 0;
        }
        return s->value;
    }
    if (is_op(t, "(")) {
        double v = expr(a);
        if (!accept_op(a, ")"))
            fail(a, "expected ')'");
        return v;
    }
    if (is_op(t, "-"))
        return -primary(a);
    if (is_op(t, "~"))
        return (double)(int32_t)~(uint32_t)(int64_t)primary(a);
    if (is_op(t, "::")) {
        uint32_t v = (uint32_t)(int64_t)primary(a), r = 0;
        for (int i = 0; i < 32; i++)
            r |= (v >> i & 1) << (31 - i);
        return (double)r;
    }
    fail(a, t->type == T_END ? "expected a value" : "unexpected '%s'", t->s);
    return 0;
}

/* precedence, loosest first: | ^ & << >> + - * / */
static double binary(asm_t *a, int level)
{
    static const char *ops[][3] = { { "|" }, { "^" }, { "&" }, { "<<", ">>" }, { "+", "-" }, { "*", "/" } };
    double l;

    if (level == 6)
        return primary(a);
    l = binary(a, level + 1);
    for (;;) {
        tok_t *t = peek(a);
        int k = -1;
        for (int i = 0; i < 3 && ops[level][i]; i++)
            if (is_op(t, ops[level][i]))
                k = i;
        if (k < 0)
            return l;
        a->pos++;
        double r = binary(a, level + 1);
        int64_t li = (int64_t)l, ri = (int64_t)r;
        switch (ops[level][k][0]) {
        case '|': l = (double)(li | ri); break;
        case '^': l = (double)(li ^ ri); break;
        case '&': l = (double)(li & ri); break;
        case '<': l = (double)(int32_t)((uint32_t)li << (ri & 31)); break;
        case '>': l = (double)((uint32_t)li >> (ri & 31)); break;
        case '+': l = l + r; break;
        case '-': l = l - r; break;
        case '*': l = l * r; break;
        case '/':
            if (r == 0) {
                fail(a, "division by zero");
                return 0;
            }
            l = (double)(li / ri);
            break;
        }
    }
}

static double expr(asm_t *a) { return binary(a, 0); }

static int iexpr(asm_t *a) { return (int)(int64_t)expr(a); }

/* An expression that must be in [lo, hi]. */
static int ranged(asm_t *a, int lo, int hi, const char *what)
{
    int v = iexpr(a);
    if (!a->failed && (v < lo || v > hi))
        fail(a, "%s %d is out of range (%d to %d)", what, v, lo, hi);
    return v;
}

/* ------------------------------------------------------------ programs */

typedef struct {
    pio_asm_program_t *p;
    char (*listing)[64]; /* each instruction's source, or NULL */
    int ninstr;
    int version;      /* -1 not given */
    int needs_v1;
    int have_wrap, have_target;
} prog_t;

static int expect_comma(asm_t *a)
{
    accept_op(a, ",");
    return 1;
}

/* ALL of an instruction after its operands: side-set and delay. */
static uint16_t side_delay(asm_t *a, prog_t *g)
{
    pio_asm_program_t *p = g->p;
    int side = -1, delay = 0;
    int sbits = p->sideset_bits + p->sideset_opt;

    for (;;) {
        if (accept_id(a, "side") || accept_id(a, "sideset") || accept_id(a, "side_set")) {
            if (!sbits) {
                fail(a, "side-set needs .side_set");
                return 0;
            }
            side = ranged(a, 0, (1 << p->sideset_bits) - 1, "side-set value");
        } else if (accept_op(a, "[")) {
            delay = ranged(a, 0, (1 << (5 - sbits)) - 1, "delay");
            if (!accept_op(a, "]"))
                fail(a, "expected ']'");
        } else {
            break;
        }
    }
    if (peek(a)->type != T_END) {
        tok_t *t = peek(a);
        if (t->type == T_NUM)
            fail(a, "unexpected %g", t->num);
        else
            fail(a, "unexpected '%s'", t->s);
        return 0;
    }
    if (side < 0 && sbits && !p->sideset_opt) {
        fail(a, "this instruction needs a side-set value (.side_set is not opt)");
        return 0;
    }
    uint field = delay;
    if (side >= 0) {
        uint s = (uint)side;
        if (p->sideset_opt)
            s |= 1u << p->sideset_bits;
        field |= s << (5 - sbits);
    }
    return (uint16_t)(field << 8);
}

static int lookup(tok_t *t, const char *const *names)
{
    for (int i = 0; names[i]; i++)
        if (names[i][0] && is_id(t, names[i]))
            return i;
    return -1;
}

/* index of a `rxfifo[...]`: 0-3, or 4 for [y]; -1 not rxfifo */
static int rxfifo_index(asm_t *a)
{
    int idx;

    if (!accept_id(a, "rxfifo"))
        return -1;
    if (!accept_op(a, "[")) {
        fail(a, "expected '[' after rxfifo");
        return 0;
    }
    if (accept_id(a, "y"))
        idx = 4;
    else
        idx = ranged(a, 0, 3, "rxfifo index");
    if (!accept_op(a, "]"))
        fail(a, "expected ']'");
    return idx;
}

static void instruction(asm_t *a, prog_t *g, uint16_t *out)
{
    static const char *const in_src[] = { "pins", "x", "y", "null", "", "", "isr", "osr", NULL };
    static const char *const out_dst[] = { "pins", "x", "y", "null", "pindirs", "pc", "isr", "exec", NULL };
    static const char *const mov_dst[] = { "pins", "x", "y", "pindirs", "exec", "pc", "isr", "osr", NULL };
    static const char *const mov_src[] = { "pins", "x", "y", "null", "", "status", "isr", "osr", NULL };
    static const char *const set_dst[] = { "pins", "x", "y", "", "pindirs", NULL };
    tok_t *op = next(a);
    uint ins = 0;
    int k;

    if (is_id(op, "nop")) {
        ins = 0xa042; /* mov y, y */
    } else if (is_id(op, "jmp")) {
        int cond = 0;
        tok_t *t = peek(a);
        if (is_op(t, "!")) {
            a->pos++;
            if (accept_id(a, "x"))
                cond = 1;
            else if (accept_id(a, "y"))
                cond = 3;
            else if (accept_id(a, "osre"))
                cond = 7;
            else
                fail(a, "unknown jmp condition");
        } else if (is_id(t, "x") || is_id(t, "y")) {
            int x = is_id(t, "x");
            a->pos++;
            if (accept_op(a, "--"))
                cond = x ? 2 : 4;
            else if (x && accept_op(a, "!=") && accept_id(a, "y"))
                cond = 5;
            else
                fail(a, "unknown jmp condition");
        } else if (accept_id(a, "pin")) {
            cond = 6;
        }
        if (cond)
            expect_comma(a);
        int addr = ranged(a, 0, 31, "jmp target");
        ins = 0u << 13 | cond << 5 | addr;
    } else if (is_id(op, "wait")) {
        int pol = 1, src, idx = 0;
        tok_t *t = peek(a);
        if (!is_id(t, "gpio") && !is_id(t, "pin") && !is_id(t, "irq") && !is_id(t, "jmppin"))
            pol = ranged(a, 0, 1, "wait polarity");
        if (accept_id(a, "gpio")) {
            src = 0;
            expect_comma(a);
            /* of the block's 32 pins, from GPIOBASE */
            idx = ranged(a, 0, 31, "gpio");
        } else if (accept_id(a, "pin")) {
            src = 1;
            expect_comma(a);
            idx = ranged(a, 0, 31, "pin");
        } else if (accept_id(a, "irq")) {
            src = 2;
            int mode = 0;
            expect_comma(a);
            if (accept_id(a, "prev"))
                mode = 1;
            else if (accept_id(a, "next"))
                mode = 3;
            idx = ranged(a, 0, 7, "irq");
            if (accept_id(a, "rel")) {
                if (mode)
                    fail(a, "rel with prev/next");
                mode = 2;
            }
            if (mode == 1 || mode == 3)
                g->needs_v1 = 1;
            idx |= mode << 3;
        } else if (accept_id(a, "jmppin")) {
            src = 3;
            g->needs_v1 = 1;
            if (accept_op(a, "+"))
                idx = ranged(a, 0, 3, "jmppin offset");
        } else {
            fail(a, "wait needs gpio, pin, irq or jmppin");
            return;
        }
        ins = 1u << 13 | (pol << 2 | src) << 5 | idx;
    } else if (is_id(op, "in")) {
        if ((k = lookup(peek(a), in_src)) < 0) {
            fail(a, "unknown in source");
            return;
        }
        a->pos++;
        expect_comma(a);
        int n = ranged(a, 1, 32, "bit count");
        ins = 2u << 13 | k << 5 | (n & 31);
    } else if (is_id(op, "out")) {
        if ((k = lookup(peek(a), out_dst)) < 0) {
            fail(a, "unknown out destination");
            return;
        }
        a->pos++;
        expect_comma(a);
        int n = ranged(a, 1, 32, "bit count");
        ins = 3u << 13 | k << 5 | (n & 31);
    } else if (is_id(op, "push") || is_id(op, "pull")) {
        int pull = is_id(op, "pull"), cond = 0, block = 1;
        for (;;) {
            if (accept_id(a, pull ? "ifempty" : "iffull"))
                cond = 1;
            else if (accept_id(a, "block"))
                block = 1;
            else if (accept_id(a, "noblock"))
                block = 0;
            else
                break;
        }
        ins = 4u << 13 | (pull << 2 | cond << 1 | block) << 5;
    } else if (is_id(op, "mov")) {
        int ridx = rxfifo_index(a);
        if (ridx >= 0) {
            /* mov rxfifo[i], isr */
            expect_comma(a);
            if (!accept_id(a, "isr"))
                fail(a, "only isr moves to the RX FIFO");
            g->needs_v1 = 1;
            ins = 4u << 13 | 0x10 | (ridx == 4 ? 0 : 8 | ridx);
        } else {
            if ((k = lookup(peek(a), mov_dst)) < 0) {
                fail(a, "unknown mov destination");
                return;
            }
            a->pos++;
            expect_comma(a);
            if (k == 7 && is_id(peek(a), "rxfifo")) {
                /* mov osr, rxfifo[i] */
                ridx = rxfifo_index(a);
                g->needs_v1 = 1;
                ins = 4u << 13 | 4 << 5 | 0x10 | (ridx == 4 ? 0 : 8 | ridx);
            } else {
                int mop = 0, s;
                if (accept_op(a, "!") || accept_op(a, "~"))
                    mop = 1;
                else if (accept_op(a, "::"))
                    mop = 2;
                if ((s = lookup(peek(a), mov_src)) < 0) {
                    fail(a, "unknown mov source");
                    return;
                }
                a->pos++;
                if (k == 3)
                    g->needs_v1 = 1;
                ins = 5u << 13 | k << 5 | mop << 3 | s;
            }
        }
    } else if (is_id(op, "irq")) {
        int clear = 0, wait = 0, mode = 0;
        for (;;) {
            if (accept_id(a, "set") || accept_id(a, "nowait"))
                clear = wait = 0;
            else if (accept_id(a, "wait"))
                wait = 1;
            else if (accept_id(a, "clear"))
                clear = 1;
            else if (accept_id(a, "prev"))
                mode = 1;
            else if (accept_id(a, "next"))
                mode = 3;
            else
                break;
        }
        int n = ranged(a, 0, 7, "irq");
        if (accept_id(a, "rel")) {
            if (mode)
                fail(a, "rel with prev/next");
            mode = 2;
        }
        if (mode == 1 || mode == 3)
            g->needs_v1 = 1;
        ins = 6u << 13 | (clear << 1 | wait) << 5 | mode << 3 | n;
    } else if (is_id(op, "set")) {
        if ((k = lookup(peek(a), set_dst)) < 0) {
            fail(a, "unknown set destination");
            return;
        }
        a->pos++;
        expect_comma(a);
        int v = ranged(a, 0, 31, "set value");
        ins = 7u << 13 | k << 5 | v;
    } else {
        fail(a, op->type == T_END ? "expected an instruction" : "unknown instruction '%s'", op->s);
        return;
    }
    if (a->failed)
        return;
    ins |= side_delay(a, g);
    *out = (uint16_t)ins;
}

/* Is this line a label? Returns its name and moves past it. */
static const char *label(asm_t *a, int *pub)
{
    tok_t *t = &a->ln->tok[a->pos];
    *pub = 0;
    if (is_id(t, "public") && t[1].type == T_ID && is_op(&t[2], ":")) {
        *pub = 1;
        a->pos += 3;
        return t[1].s;
    }
    if (t->type == T_ID && t->s[0] != '.' && is_op(&t[1], ":")) {
        a->pos += 2;
        return t->s;
    }
    return NULL;
}

static void add_define(prog_t *g, const char *name, int value)
{
    pio_asm_program_t *p = g->p;
    if (p->ndefines < PIO_ASM_MAX_DEFINES) {
        snprintf(p->defines[p->ndefines].name, sizeof(p->defines[0].name), "%s", name);
        p->defines[p->ndefines++].value = value;
    }
}

/* .define [public] NAME EXPR */
static void define(asm_t *a, prog_t *g)
{
    int pub = accept_id(a, "public");
    tok_t *n = next(a);
    if (n->type != T_ID) {
        fail(a, ".define needs a name");
        return;
    }
    int v = iexpr(a);
    if (a->failed)
        return;
    if (add_sym(a, n->s, v, pub, 0) && pub && g)
        add_define(g, n->s, v);
}

static void directive(asm_t *a, prog_t *g, tok_t *d)
{
    pio_asm_program_t *p = g->p;

    if (is_id(d, ".define")) {
        define(a, g);
    } else if (is_id(d, ".origin")) {
        p->program.origin = (int8_t)ranged(a, 0, 31, "origin");
    } else if (is_id(d, ".side_set")) {
        if (g->ninstr) {
            fail(a, ".side_set must come before the instructions");
            return;
        }
        int n = ranged(a, 0, 5, "side-set bits");
        p->sideset_bits = n;
        for (;;) {
            if (accept_id(a, "opt"))
                p->sideset_opt = 1;
            else if (accept_id(a, "pindirs"))
                p->sideset_pindirs = 1;
            else
                break;
        }
        if (n + p->sideset_opt > 5)
            fail(a, "side-set takes at most 5 bits with opt");
    } else if (is_id(d, ".wrap_target")) {
        if (g->have_target)
            fail(a, ".wrap_target twice");
        p->wrap_target = g->ninstr;
        g->have_target = 1;
    } else if (is_id(d, ".wrap")) {
        if (!g->ninstr)
            fail(a, ".wrap before any instruction");
        if (g->have_wrap)
            fail(a, ".wrap twice");
        p->wrap = g->ninstr - 1;
        g->have_wrap = 1;
    } else if (is_id(d, ".lang_opt")) {
        a->pos = a->ln->ntok;
    } else if (is_id(d, ".pio_version")) {
        tok_t *t = peek(a);
        if (is_id(t, "rp2040")) {
            a->pos++;
            g->version = 0;
        } else if (is_id(t, "rp2350")) {
            a->pos++;
            g->version = 1;
        } else {
            g->version = ranged(a, 0, 1, "PIO version");
        }
    } else if (is_id(d, ".clock_div")) {
        double v = expr(a);
        if (v < 1 || v > 65536)
            fail(a, "clock divider %g is out of range (1 to 65536)", v);
        p->clock_div = (float)v;
        p->has |= 32;
    } else if (is_id(d, ".fifo")) {
        static const char *const f[] = { "txrx", "tx", "rx", "", "txget", "", "", "", "txput", "", "", "", "putget", NULL };
        int k = lookup(peek(a), f);
        if (k < 0) {
            fail(a, ".fifo needs txrx, tx, rx, txput, txget or putget");
            return;
        }
        a->pos++;
        if (k >= 4)
            g->needs_v1 = 1;
        p->fifo = k;
        p->has |= 8;
    } else if (is_id(d, ".mov_status")) {
        if (accept_id(a, "rxfifo") || is_id(peek(a), "txfifo")) {
            int rx = !accept_id(a, "txfifo");
            if (!accept_op(a, "<"))
                fail(a, "expected '<'");
            p->mov_status_type = rx ? STATUS_RX_LESSTHAN : STATUS_TX_LESSTHAN;
            p->mov_status_n = ranged(a, 0, 31, "status level");
        } else if (accept_id(a, "irq")) {
            int mode = 0;
            if (accept_id(a, "prev"))
                mode = 1;
            else if (accept_id(a, "next"))
                mode = 3;
            if (!accept_id(a, "set"))
                fail(a, "expected 'set'");
            p->mov_status_type = STATUS_IRQ_SET;
            p->mov_status_n = ranged(a, 0, 7, "irq") | mode << 3;
            g->needs_v1 = 1;
        } else {
            fail(a, ".mov_status needs rxfifo, txfifo or irq");
        }
        p->has |= 16;
    } else if (is_id(d, ".in") || is_id(d, ".out")) {
        int in = is_id(d, ".in");
        int n = ranged(a, in ? 1 : 0, 32, "pin count"), right = 1, aut = 0, th = 32;
        if (accept_id(a, "left"))
            right = 0;
        else
            accept_id(a, "right");
        if (accept_id(a, "auto"))
            aut = 1;
        else
            accept_id(a, "manual");
        if (peek(a)->type != T_END)
            th = ranged(a, 1, 32, "threshold");
        if (in) {
            if (n != 32)
                g->needs_v1 = 1;
            p->in_count = n & 31;
            p->in_right = right;
            p->in_auto = aut;
            p->in_threshold = th & 31;
            p->has |= 1;
        } else {
            p->out_count = n;
            p->out_right = right;
            p->out_auto = aut;
            p->out_threshold = th & 31;
            p->has |= 2;
        }
    } else if (is_id(d, ".set")) {
        p->set_count = ranged(a, 0, 5, "pin count");
        p->has |= 4;
    } else if (is_id(d, ".word")) {
        /* handled with the instructions */
    } else {
        fail(a, "unknown directive '%s'", d->s);
    }
}

/* Assemble lines [from, to) of one program into g. */
static void program(asm_t *a, prog_t *g, int from, int to)
{
    pio_asm_program_t *p = g->p;
    int n = 0, pub;
    const char *l;

    /* labels first: their instruction numbers */
    for (int i = from; i < to && !a->failed; i++) {
        a->ln = &a->lines[i];
        a->pos = 0;
        if (a->ln->verbatim)
            continue;
        if ((l = label(a, &pub))) {
            if (add_sym(a, l, n, pub, 1) && pub) {
                char nm[40];
                snprintf(nm, sizeof(nm), "offset_%s", l);
                add_define(g, nm, n);
            }
        }
        tok_t *t = peek(a);
        if (t->type == T_ID && (t->s[0] != '.' || is_id(t, ".word")))
            n++;
    }
    if (n > PIO_INSTRUCTION_COUNT) {
        fail(a, "%d instructions: at most 32 fit", n);
        return;
    }
    /* then directives and instructions, in order */
    for (int i = from; i < to && !a->failed; i++) {
        a->ln = &a->lines[i];
        a->pos = 0;
        if (a->ln->verbatim)
            continue;
        label(a, &pub);
        tok_t *t = peek(a);
        if (t->type == T_END)
            continue;
        if (is_id(t, ".word")) {
            a->pos++;
            int v = ranged(a, 0, 0xffff, "word");
            if (!a->failed && peek(a)->type != T_END)
                fail(a, "unexpected text after .word");
            p->instructions[g->ninstr++] = (uint16_t)v;
        } else if (t->type == T_ID && t->s[0] == '.') {
            a->pos++;
            directive(a, g, t);
            if (!a->failed && peek(a)->type != T_END)
                fail(a, "unexpected text after %s", t->s);
        } else {
            if (g->listing) {
                /* the line without its label and comment */
                const char *src = a->ln->text, *c;
                char *d = g->listing[g->ninstr];
                if ((c = strchr(src, ':')) && a->pos > 0)
                    src = c + 1;
                while (*src == ' ' || *src == '\t')
                    src++;
                snprintf(d, 64, "%s", src);
                for (char *q = d; *q; q++)
                    if (*q == ';' || (q[0] == '/' && (q[1] == '/' || q[1] == '*')))
                        *q = 0;
                for (size_t n = strlen(d); n && (d[n - 1] == ' ' || d[n - 1] == '\t' || d[n - 1] == '\r'); n--)
                    d[n - 1] = 0;
            }
            instruction(a, g, &p->instructions[g->ninstr++]);
        }
    }
    a->ln = &a->lines[to > from ? to - 1 : from];
    if (a->failed)
        return;
    if (!g->ninstr) {
        fail(a, "program '%s' has no instructions", p->name);
        return;
    }
    if (!g->have_wrap)
        p->wrap = g->ninstr - 1;
    if (g->version == 0 && g->needs_v1) {
        fail(a, "program '%s' uses RP2350 instructions but says .pio_version 0", p->name);
        return;
    }
    p->program.instructions = p->instructions;
    p->program.length = g->ninstr;
    p->program.pio_version = g->version >= 0 ? g->version : g->needs_v1;
}

/* ------------------------------------------------------------ files */

void pioasm_free(pioasm_file_t *f)
{
    if (!f)
        return;
    free(f->programs);
    free(f->sdk_text);
    free(f->sdk_start);
    free(f->sdk_len);
    free(f->listing);
    free(f);
}

pioasm_file_t *pioasm_parse(const char *source, char *err, size_t errlen)
{
    asm_t *a = calloc(1, sizeof(*a));
    pioasm_file_t *f = calloc(1, sizeof(*f));
    char *buf = strdup(source ? source : "");
    int cap = 64, in_comment = 0, verbatim = 0;
    int nprog = 0;
    int starts[64];
    size_t sdk_cap = 256, sdk_used = 0;

    if (err && errlen)
        err[0] = 0;
    if (!a || !f || !buf)
        goto oom;
    a->err = err;
    a->errlen = errlen;
    a->lines = malloc(cap * sizeof(line_t));
    f->sdk_text = malloc(sdk_cap);
    if (!a->lines || !f->sdk_text)
        goto oom;
    f->sdk_text[0] = 0;
    /* lines */
    {
        char *s = buf;
        int lineno = 0;
        while (s) {
            char *e = strchr(s, '\n');
            const char *q;
            if (e)
                *e = 0;
            lineno++;
            if (a->nlines + 1 >= cap) {
                line_t *nl = realloc(a->lines, (cap *= 2) * sizeof(line_t));
                if (!nl)
                    goto oom;
                a->lines = nl;
            }
            line_t *ln = &a->lines[a->nlines];
            memset(ln, 0, sizeof(*ln));
            for (q = s; *q == ' ' || *q == '\t'; q++)
                ;
            if (!in_comment && q[0] == '%') {
                /* % c-sdk { ... %}: kept for pioasm's header; other
                 * languages' blocks are dropped */
                if (q[1] == '}') {
                    verbatim = 0;
                } else {
                    verbatim = strstr(q, "c-sdk") ? 1 : 2;
                }
                ln->lineno = lineno;
                ln->verbatim = 3; /* the % line itself: nothing */
                ln->tok = NULL;
                a->nlines++;
            } else if (verbatim) {
                ln->lineno = lineno;
                ln->verbatim = verbatim;
                ln->text = s;
                a->nlines++;
            } else {
                if (tokenize(s, lineno, ln, &in_comment) < 0)
                    goto oom;
                a->nlines++;
            }
            s = e ? e + 1 : NULL;
        }
    }
    /* file-wide defines, and where each .program starts */
    for (int i = 0; i < a->nlines && !a->failed; i++) {
        line_t *ln = &a->lines[i];
        a->ln = ln;
        a->pos = 0;
        if (ln->verbatim || !ln->ntok)
            continue;
        if (is_id(&ln->tok[0], ".program")) {
            if (nprog >= 64) {
                fail(a, "too many programs");
                break;
            }
            starts[nprog++] = i;
        } else if (!nprog && is_id(&ln->tok[0], ".define")) {
            a->pos = 1;
            define(a, NULL);
        }
    }
    a->nglobal = a->nsyms;
    if (a->failed)
        goto out;
    /* no .program: the whole file is one, "program", whose defines are its
     * own */
    int whole = !nprog;
    if (whole) {
        nprog = 1;
        a->nsyms = a->nglobal = 0;
    }
    f->programs = calloc(nprog, sizeof(*f->programs));
    f->sdk_start = calloc(nprog, sizeof(size_t));
    f->sdk_len = calloc(nprog, sizeof(size_t));
    f->listing = calloc(nprog, sizeof(*f->listing));
    if (!f->programs || !f->sdk_start || !f->sdk_len || !f->listing)
        goto oom;
    f->count = nprog;
    for (int k = 0; k < nprog && !a->failed; k++) {
        int from = whole ? 0 : starts[k] + 1;
        int to = whole || k + 1 == nprog ? a->nlines : starts[k + 1];
        pio_asm_program_t *p = &f->programs[k];
        prog_t g = { p, f->listing[k], 0, -1, 0, 0, 0 };

        p->program.origin = -1;
        if (whole) {
            snprintf(p->name, sizeof(p->name), "program");
        } else {
            a->ln = &a->lines[starts[k]];
            a->pos = 1;
            tok_t *nm = next(a);
            if (nm->type != T_ID || peek(a)->type != T_END) {
                fail(a, ".program needs a name");
                break;
            }
            snprintf(p->name, sizeof(p->name), "%s", nm->s);
            for (int j = 0; j < k; j++)
                if (!strcmp(f->programs[j].name, p->name))
                    fail(a, "program '%s' twice", p->name);
        }
        a->nsyms = a->nglobal;
        /* the file's public defines are in each program's header too */
        for (int i = 0; i < a->nglobal; i++)
            if (a->syms[i].pub)
                add_define(&g, a->syms[i].name, a->syms[i].value);
        program(a, &g, from, to);
        /* its c-sdk text */
        f->sdk_start[k] = sdk_used;
        for (int i = from; i < to; i++) {
            line_t *ln = &a->lines[i];
            if (ln->verbatim != 1)
                continue;
            size_t n = strlen(ln->text);
            while (sdk_used + n + 2 > sdk_cap) {
                char *nb = realloc(f->sdk_text, sdk_cap *= 2);
                if (!nb)
                    goto oom;
                f->sdk_text = nb;
            }
            memcpy(f->sdk_text + sdk_used, ln->text, n);
            sdk_used += n;
            f->sdk_text[sdk_used++] = '\n';
            f->sdk_text[sdk_used] = 0;
        }
        f->sdk_len[k] = sdk_used - f->sdk_start[k];
    }
out:
    for (int i = 0; i < a->nlines; i++)
        free(a->lines[i].tok);
    if (a->failed) {
        free(a->lines);
        free(a);
        free(buf);
        pioasm_free(f);
        return NULL;
    }
    free(a->lines);
    free(a);
    free(buf);
    /* point each program at its own instructions */
    for (int k = 0; k < f->count; k++)
        f->programs[k].program.instructions = f->programs[k].instructions;
    return f;
oom:
    if (err && errlen)
        snprintf(err, errlen, "out of memory");
    if (a) {
        for (int i = 0; i < a->nlines; i++)
            free(a->lines[i].tok);
        free(a->lines);
    }
    free(a);
    free(buf);
    pioasm_free(f);
    return NULL;
}

int pio_assemble(const char *source, const char *name, pio_asm_program_t *out, char *err, size_t errlen)
{
    pioasm_file_t *f = pioasm_parse(source, err, errlen);
    int k;

    if (!f)
        return -1;
    for (k = 0; k < f->count; k++)
        if (!name || !strcmp(f->programs[k].name, name))
            break;
    if (k == f->count) {
        if (err && errlen)
            snprintf(err, errlen, "no program '%s'", name);
        pioasm_free(f);
        return -1;
    }
    *out = f->programs[k];
    out->program.instructions = out->instructions;
    pioasm_free(f);
    return 0;
}
