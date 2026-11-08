/* rvemu-pico-shell: what core1 runs, linked into SRAM (copied there at
 * startup by core1::copy_core1_text). This script is given before
 * rp235x_riscv.x, so these patterns claim their input sections before
 * riscv-rt's `*(.text .text.*)` does; INSERT then places the section after
 * .data in SRAM, loaded from flash after .data's image. */
SECTIONS {
  .core1_text : AT(LOADADDR(.data) + SIZEOF(.data)) ALIGN(4)
  {
    __score1_text = .;
    *(.text.*cyw43* .text.*embassy* .text.*usb_device* .text.*usbd_serial*)
    *(.text.*rp235x_hal3usb* .text.*4wifi* .text.*6usbcon* .text.*5core1*)
    *(.text.*memcpy* .text.*memmove* .text.*memset* .text.__udivdi3 .text.__umoddi3 .text.*u64_div_rem*)
    *(.data.core1 .data.core1.*)
    *(.rodata.*cyw43* .rodata.*embassy* .rodata.*usb_device* .rodata.*usbd_serial*)
    *(.rodata.*rp235x_hal3usb* .rodata.*6usbcon* .rodata.*5core1*)
    . = ALIGN(4);
    __ecore1_text = .;
  } > RAM
} INSERT AFTER .data;
__sicore1_text = LOADADDR(.core1_text);

/* SRAM8/9 (0x20080000, 8 KB, one region here; memory.x has them as two):
 * core1's rings, zeroed by copy_core1_text. */
MEMORY { SRAM89 : ORIGIN = 0x20080000, LENGTH = 8K }
SECTIONS {
  .sram89 (NOLOAD) : ALIGN(4)
  {
    __ssram89 = .;
    *(.sram89 .sram89.*)
    . = ALIGN(4);
    __esram89 = .;
  } > SRAM89
} INSERT AFTER .stack;
