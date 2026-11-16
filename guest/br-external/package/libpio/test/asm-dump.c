/* A .pio file on stdin, each program on one line: name, wrap target and
 * source, side-set bits/optional/pindirs, then its instructions in hex,
 * programs sorted by name. corpus.expect is pio-parser's output in this
 * format (rvemu-rs/rvemu's pio_corpus test makes it again and compares);
 * micropython-machine/test uses this to check rp2's assembler. */
#include "pioasm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int by_name(const void *a, const void *b)
{
    return strcmp(((const pio_asm_program_t *)a)->name, ((const pio_asm_program_t *)b)->name);
}

int main(void)
{
    static char src[1 << 20];
    char err[256];
    size_t n = fread(src, 1, sizeof(src) - 1, stdin);
    src[n] = 0;
    pioasm_file_t *f = pioasm_parse(src, err, sizeof(err));
    if (!f) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    qsort(f->programs, f->count, sizeof(*f->programs), by_name);
    for (int k = 0; k < f->count; k++) {
        pio_asm_program_t *p = &f->programs[k];
        printf("%s wrap %u %u side %u %u %u :", p->name, p->wrap_target, p->wrap, p->sideset_bits,
               p->sideset_opt, p->sideset_pindirs);
        for (int i = 0; i < p->program.length; i++)
            printf(" %04x", p->instructions[i]);
        printf("\n");
    }
    pioasm_free(f);
    return 0;
}
