/* A static Linux executable: one read+execute segment, one read+write one. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
  . = 0x10000 + SIZEOF_HEADERS;
  .text : { KEEP(*(.text._start)) *(.text .text.*) }
  /* each segment on its own pages: the kernel maps them with different permissions */
  . = ALIGN(4096);
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
  . = ALIGN(4096);
  .data : { *(.data .data.* .sdata .sdata.*) }
  .bss (NOLOAD) : ALIGN(16) { *(.bss .bss.* .sbss .sbss.* COMMON) }
  /DISCARD/ : { *(.eh_frame*) *(.riscv.attributes) *(.comment) }
}
