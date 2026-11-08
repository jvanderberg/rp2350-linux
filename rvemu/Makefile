CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion

SRCS := src/cpu.c src/dev.c src/main.c
OBJS := $(SRCS:src/%.c=build/%.o)

PREBUILT := ../ref/prebuilt

all: rvemu

rvemu: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

build/%.o: src/%.c src/cpu.h src/dev.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

# Boot semu's prebuilt kernel + rootfs interactively.
run: rvemu
	./rvemu -k $(PREBUILT)/Image -i $(PREBUILT)/rootfs.cpio

# Deterministic boot-to-login measurement with the RP2350 timing model.
bench: rvemu
	./rvemu -B -M -k $(PREBUILT)/Image -i $(PREBUILT)/rootfs.cpio -u "login:"

clean:
	rm -rf build rvemu

.PHONY: all run bench clean
