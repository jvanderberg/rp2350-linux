#!/bin/sh
# install-headers.sh MUSL_INCLUDE STAGING_INCLUDE TARGET_INCLUDE WORKDIR
# Copies every musl header and the kernel UAPI headers listed below, with
# all the headers they include, from STAGING_INCLUDE to TARGET_INCLUDE.
# $CC (the target gcc) finds the includes: a stub that includes them all
# (written to WORKDIR) is run through `$CC -M`.
set -e
MUSL=$1 STAGING=$2 TARGET=$3 W=$4
UAPI="linux/gpio.h linux/i2c.h linux/i2c-dev.h linux/spi/spidev.h
linux/input.h linux/input-event-codes.h linux/uinput.h linux/iio/events.h
linux/iio/types.h linux/watchdog.h linux/rtc.h linux/serial.h linux/fb.h
linux/if.h linux/if_ether.h linux/sockios.h linux/random.h linux/limits.h
linux/errno.h linux/types.h linux/ioctl.h"
{
	(cd "$MUSL" && find . -name '*.h' | sed 's|^\./||' | sort)
	for h in $UAPI; do echo "$h"; done
} | sed 's|.*|#include <&>|' > "$W/headers.c"
$CC -M -E -D_GNU_SOURCE "$W/headers.c" -o "$W/headers.d"
sed 's/[\\ ]/\n/g' "$W/headers.d" | grep "^$STAGING/" | sort -u | while read -r f; do
	rel=${f#"$STAGING"/}
	mkdir -p "$TARGET/$(dirname "$rel")"
	cp "$f" "$TARGET/$rel"
done
