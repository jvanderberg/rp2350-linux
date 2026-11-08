CC ?= cc
CFLAGS ?= -O2 -g
# Declare GNU libc helpers such as asprintf when building on Linux.
CPPFLAGS += -D_GNU_SOURCE
CFLAGS += -std=gnu11 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion

SRCS := src/cpu.c src/dev.c src/main.c
OBJS := $(SRCS:src/%.c=build/%.o)

GUEST ?= ../guest/out

all: rvemu

rvemu: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

build/%.o: src/%.c src/cpu.h src/dev.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

# Boot the locally built initramfs guest interactively.
run: rvemu
	./rvemu -k $(GUEST)/Image -i $(GUEST)/rootfs.cpio

# Deterministic boot-to-login measurement with the RP2350 timing model.
bench: rvemu
	./rvemu -B -M -k $(GUEST)/Image -i $(GUEST)/rootfs.cpio -u "login:"

clean:
	rm -rf build rvemu

.PHONY: all run bench clean
