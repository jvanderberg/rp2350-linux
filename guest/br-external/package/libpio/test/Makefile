# libpio's host tests: the library against a fake /dev/pio, the assembler
# against pio-parser's output and on bad input, and pioasm's headers through
# the compiler. On the Mac or Linux, not the board:
#
#   make -C guest/br-external/package/libpio/test test
#
# Built with ASan and UBSan (SAN= to build without). The tests on a real
# block, in the emulator and on the board, are in guest/tests/pio.
CC ?= cc
SRC = ../src
UAPI = ../../../../linux/drivers/rvemu
SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
CFLAGS = -g -O1 -Wall -Werror -U_FORTIFY_SOURCE $(SAN) -I$(SRC) -I$(UAPI) -I.
ifeq ($(shell uname),Darwin)
CFLAGS += -Ishim
endif
# pio.c's system calls, to the fake (fakedev.c)
RENAME = -DLIBPIO_PLAIN_REGISTERS -Dopen=fake_open -Dclose=fake_close -Dioctl=fake_ioctl -Dmmap=fake_mmap \
	-Dmunmap=fake_munmap -Dpoll=fake_poll -Dread=fake_read
B = build

all: $(B)/test-pio $(B)/test-asm $(B)/asm-dump $(B)/pioasm

$(B):
	mkdir -p $@

$(B)/pio-fake.o: $(SRC)/pio.c $(SRC)/pio.h | $(B)
	$(CC) $(CFLAGS) $(RENAME) -c -o $@ $<

$(B)/test-pio: test-pio.c fakedev.c fakedev.h $(B)/pio-fake.o $(SRC)/pioasm.c | $(B)
	$(CC) $(CFLAGS) -DLIBPIO_PLAIN_REGISTERS -o $@ test-pio.c fakedev.c $(B)/pio-fake.o $(SRC)/pioasm.c

$(B)/test-asm: test-asm.c $(SRC)/pioasm.c $(SRC)/pio.h | $(B)
	$(CC) $(CFLAGS) -o $@ test-asm.c $(SRC)/pioasm.c

$(B)/asm-dump: asm-dump.c $(SRC)/pioasm.c $(SRC)/pioasm.h | $(B)
	$(CC) $(CFLAGS) -o $@ asm-dump.c $(SRC)/pioasm.c

$(B)/pioasm: $(SRC)/pioasm-main.c $(SRC)/pioasm.c | $(B)
	$(CC) $(CFLAGS) -o $@ $(SRC)/pioasm-main.c $(SRC)/pioasm.c

test: all
	$(B)/test-pio
	$(B)/test-asm
	$(B)/asm-dump < corpus.pio | diff -u corpus.expect -
	@echo "corpus: the same as pio-parser"
	@# headers pioasm writes must compile: every example, and the corner
	@# cases (an integer .clock_div once came out as "2f")
	for f in $(SRC)/examples/*.pio headers.pio; do \
		$(B)/pioasm $$f $(B)/h.h && printf '#include "h.h"\n' > $(B)/h.c && \
		$(CC) -Wall -Werror -fsyntax-only -I$(SRC) -I$(B) $(B)/h.c || exit 1; \
	done
	@echo "headers: compile"

clean:
	rm -rf $(B)

.PHONY: all test clean
