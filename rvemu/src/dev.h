/* Platform devices: 8250 UART, SiFive-style PLIC, host console. */
#pragma once
#include "cpu.h"

#define PLIC_BASE 0xF0000000u
#define PLIC_SIZE 0x04000000u
#define UART_BASE 0xF4000000u
#define UART_SIZE 0x00000100u
#define UART_IRQ 1

void dev_init(cpu_t *c, bool interactive, const char *until);
void dev_shutdown(void);
int mmio_load(cpu_t *c, uint32_t pa, int size, uint32_t *v);  /* 0 or exc */
int mmio_store(cpu_t *c, uint32_t pa, int size, uint32_t v); /* 0 or exc */
void dev_poll(cpu_t *c); /* periodic: host input, interrupt lines */
void dev_wfi(cpu_t *c);  /* guest is idle */
void console_putc(cpu_t *c, uint8_t ch);
int console_getc(cpu_t *c); /* -1 if nothing buffered */
void console_flush(void);
