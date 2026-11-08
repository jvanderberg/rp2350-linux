/* Guest physical RAM starts at 0 (1 MiB). The emulator puts the DTB at
   ram_size - 64 KiB, so everything here stays below 0xE0000. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
  . = 0x0;
  .text : { KEEP(*(.text.start)) *(.text .text.*) }
  . = ALIGN(4096);
  .alias_text : { __alias_page = .; KEEP(*(.alias_text)) }
  . = ALIGN(4096);
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
  .data : ALIGN(16) { *(.data .data.* .sdata .sdata.*) }
  .bss (NOLOAD) : ALIGN(4096) {
    __bss_start = .;
    *(.bss .bss.* .sbss .sbss.* COMMON)
    . = ALIGN(16);
    __bss_end = .;
  }
  . = ALIGN(16);
  . += 0x8000;
  __stack_top = .;
  ASSERT(. < 0xE0000, "guest image too large for 1 MiB RAM")
  /DISCARD/ : { *(.eh_frame*) }
}
