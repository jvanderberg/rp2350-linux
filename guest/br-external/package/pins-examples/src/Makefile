# The pins examples: make (cross: make CC=riscv32-...-gcc), or on the board
# with tcc: tcc -o gpio-blink gpio-blink.c
CC ?= cc
CFLAGS ?= -O2 -Wall
PROGS = gpio-blink gpio-edge i2c-scan spi-xfer adc-read

all: $(PROGS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGS)
