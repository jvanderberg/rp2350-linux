OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
 . = 0;
 .text : { KEEP(*(.text.start)) *(.text .text.*) }
 .rodata : ALIGN(16) { *(.rodata .rodata.* .srodata .srodata.*) }
 .data : ALIGN(16) { *(.data .data.* .sdata .sdata.*) }
 .bss (NOLOAD) : ALIGN(16) { *(.bss .bss.* .sbss .sbss.* COMMON) }
 . = ALIGN(16); . += 0x20000; __stack_top = .;
 ASSERT(. < 0x100000, "guest overlaps workload config")
 /DISCARD/ : { *(.eh_frame*) }
}
