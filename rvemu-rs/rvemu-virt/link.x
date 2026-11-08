/* QEMU virt, -bios none: RAM at 0x80000000. Guest RAM is placed by the run
   script at 0x81000000 (and a reference copy at 0x85000000). */
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
  . = 0x80000000;
  .text : { KEEP(*(.text.start)) *(.text .text.*) }
  .rodata : ALIGN(8) { *(.rodata .rodata.* .srodata .srodata.*) }
  .data : ALIGN(8) { *(.data .data.* .sdata .sdata.*) }
  .bss (NOLOAD) : ALIGN(16) {
    __bss_start = .;
    *(.bss .bss.* .sbss .sbss.* COMMON)
    . = ALIGN(16);
    __bss_end = .;
  }
  . = ALIGN(16);
  __stack_bottom = .;
  . += 0x40000;
  __stack_top = .;
  . += 0x4000;
  __trap_stack_top = .;
  ASSERT(. < 0x80F00000, "image overlaps boot info")
  /DISCARD/ : { *(.eh_frame*) }
}
