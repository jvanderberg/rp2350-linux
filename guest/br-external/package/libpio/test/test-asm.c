/* The assembler on bad input: each case must fail with its message, not
 * crash (built with the sanitizers) or assemble something.
 *
 *   make -C guest/br-external/package/libpio/test test
 */
#include "pio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

static void bad(const char *what, const char *src, const char *want)
{
    char err[256] = "";
    pio_asm_program_t p;
    checks++;
    if (pio_assemble(src, NULL, &p, err, sizeof(err)) == 0) {
        fails++;
        printf("FAIL %s: assembled\n", what);
    } else if (!strstr(err, want)) {
        fails++;
        printf("FAIL %s: \"%s\", want \"%s\"\n", what, err, want);
    }
}

static char *repeat(const char *head, const char *s, int n, const char *mid, const char *t, const char *tail)
{
    size_t len = strlen(head) + (strlen(s) + strlen(t)) * n + strlen(mid) + strlen(tail) + 1;
    char *b = malloc(len);
    strcpy(b, head);
    for (int i = 0; i < n; i++)
        strcat(b, s);
    strcat(b, mid);
    for (int i = 0; i < n; i++)
        strcat(b, t);
    strcat(b, tail);
    return b;
}

int main(void)
{
    /* the review's finding: deep nesting overflowed the stack */
    char *deep = repeat(".program p\nset x, ", "(", 10000, "1", ")", "\n");
    bad("10000 nested parentheses", deep, "nested too deeply");
    free(deep);
    char *neg = repeat(".program p\nset x, ", "- ", 10000, "1", "", "\n");
    bad("10000 unary minus signs", neg, "nested too deeply");
    free(neg);
    char *tilde = repeat(".program p\nset x, ", "~", 10000, "1", "", "\n");
    bad("10000 complements", tilde, "nested too deeply");
    free(tilde);

    /* the documentation pass's finding: defines past 16 were dropped */
    char many[2048] = ".program p\n";
    for (int i = 0; i < 17; i++)
        snprintf(many + strlen(many), sizeof(many) - strlen(many), ".define public D%d %d\n", i, i);
    strcat(many, "nop\n");
    bad("17 public defines", many, "more than 16 public");

    bad("set value too big", ".program p\nset x, 32\n", "out of range");
    bad("unknown label", ".program p\njmp nowhere\n", "unknown symbol");
    bad("label twice", ".program p\na:\nnop\na:\nnop\n", "already defined");
    char *big = repeat(".program p\n", "nop\n", 33, "", "", "");
    bad("33 instructions", big, "");
    free(big);
    bad("delay too long with side-set", ".program p\n.side_set 4\nnop side 0 [2]\n", "");
    bad("unterminated expression", ".program p\nset x, (1 + \n", "");
    bad("division by zero", ".program p\nset x, 1 / 0\n", "division by zero");
    /* found writing these tests: it was dropped without a word */
    bad("an instruction before .program", "set x, 1\n.program p\nnop\n", "outside a .program");
    bad("a label before .program", "top:\n.program p\njmp top\n", "outside a .program");

    printf("test-asm: %d of %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
