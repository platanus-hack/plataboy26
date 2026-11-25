/* plataboy26 loader kernel: linked at KBASE (passed by the makefile) */
MEMORY
{
  KFLASH (rx) : ORIGIN = KBASE, LENGTH = 0x3FC0 - KBASE
}
SECTIONS
{
  __kbase = ORIGIN(KFLASH);
  .text : { KEEP(*(.entry)) *(.text .text.* .rodata .rodata.* .srodata .srodata.*) } > KFLASH
  .data : { *(.data .data.* .sdata .sdata.* .bss .bss.* .sbss .sbss.* COMMON) } > KFLASH
  ASSERT(SIZEOF(.data) == 0, "the kernel must not have .data/.bss")
  /DISCARD/ : { *(.comment .riscv.attributes .note*) }
}
