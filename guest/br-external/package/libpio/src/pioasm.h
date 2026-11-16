/* libpio's assembler, a whole file at a time (for pioasm-main.c). */
#ifndef LIBPIO_PIOASM_H
#define LIBPIO_PIOASM_H
#include "pio.h"

typedef struct {
    int count;
    pio_asm_program_t *programs;
    /* each program's `% c-sdk { ... %}` text: sdk_text + sdk_start[k],
     * sdk_len[k] bytes */
    char *sdk_text;
    size_t *sdk_start;
    size_t *sdk_len;
    /* each program's instructions as written */
    char (*listing)[PIO_INSTRUCTION_COUNT][64];
} pioasm_file_t;

/* NULL, with a message in err, on an error. */
pioasm_file_t *pioasm_parse(const char *source, char *err, size_t errlen);
void pioasm_free(pioasm_file_t *f);
#endif
