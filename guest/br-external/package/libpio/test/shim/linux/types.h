/* For building libpio's host tests where there are no Linux headers (macOS):
 * just what rvemu-pio.h uses. The Makefile adds this directory only there. */
#ifndef SHIM_LINUX_TYPES_H
#define SHIM_LINUX_TYPES_H
#include <stdint.h>
typedef uint32_t __u32;
typedef uint8_t __u8;
#endif
