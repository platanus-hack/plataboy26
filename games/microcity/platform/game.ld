ENTRY( jump_reset )

/* MicroCity on the console: the vectors, the overlay window (from 0x40, whole 64-byte
   flash pages), the resident code, then the loader kernel (KBASE, from the makefile).
   The window comes first because linker relaxation only shortens code: jumps between
   the window and the resident code then only get shorter (a window above the resident
   code moves away from it, and a jump relaxed to 2 bytes can end up out of range). */
MEMORY
{
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = KBASE
  RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 2K
  EEDATA (r) : ORIGIN = 0x30000000, LENGTH = 64K   /* data in the EEPROM (port/avr/pgmspace.h) */
  OVLLMA (r) : ORIGIN = 0x40000000, LENGTH = 64K   /* overlay images (go to the EEPROM) */
}

SECTIONS
{
  .eedata : { KEEP(*(.eedata .eedata.*)) } > EEDATA

  .init :
  { 
    _sinit = .;
    . = ALIGN(4);
    KEEP(*(SORT_NONE(.init.jump)))
    KEEP(*(SORT_NONE(.init.vectors)))
    . = ALIGN(4);
    _einit = .;
  } >FLASH AT>FLASH

  /* the overlay groups share one flash window; each image starts with OVL_TAG + its
     number (OVL_TAG: this build's ID in the upper 16 bits, from the makefile), so what
     another program or an older build left in the window never passes for an overlay.
     NOCROSSREFS: the linker refuses a reference from one group into another */
  OVERLAY 0x40 : NOCROSSREFS AT (0x40000000)
  {
    .ov1 { LONG(OVL_TAG + 1) KEEP(*(.ov1.text* .ov1.rodata*)) . = ALIGN(4); }
    .ov2 { LONG(OVL_TAG + 2) KEEP(*(.ov2.text* .ov2.rodata*)) . = ALIGN(4); }
    .ov3 { LONG(OVL_TAG + 3) KEEP(*(.ov3.text* .ov3.rodata*)) . = ALIGN(4); }
  }
  __ovl_base = 0x40;
  __ovl_end = ALIGN(0x40 + MAX(SIZEOF(.ov1), MAX(SIZEOF(.ov2), SIZEOF(.ov3))), 64);
  ASSERT(_einit <= 0x40, "the vectors reach into the overlay window")
  .window 0x40 (NOLOAD) : { . = __ovl_end - 0x40; } >FLASH   /* the window's room in FLASH */

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text.*)
    *(.rodata)
    *(.rodata*)
    *(.gnu.linkonce.t.*)
    . = ALIGN(4);
  } >FLASH AT>FLASH 

  .fini :
  {
    KEEP(*(SORT_NONE(.fini)))
    . = ALIGN(4);
  } >FLASH AT>FLASH

  PROVIDE( _etext = . );
  PROVIDE( _eitcm = . );  

  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } >FLASH AT>FLASH 
  
  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
    KEEP(*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
    PROVIDE_HIDDEN(__init_array_end = .);
  } >FLASH AT>FLASH 
  
  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
    KEEP(*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } >FLASH AT>FLASH 
  
  .ctors :
  {
    KEEP(*crtbegin.o(.ctors))
    KEEP(*crtbegin?.o(.ctors))
    KEEP(*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
    KEEP(*(SORT(.ctors.*)))
    KEEP(*(.ctors))
  } >FLASH AT>FLASH 
  
  .dtors :
  {
    KEEP(*crtbegin.o(.dtors))
    KEEP(*crtbegin?.o(.dtors))
    KEEP(*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
    KEEP(*(SORT(.dtors.*)))
    KEEP(*(.dtors))
  } >FLASH AT>FLASH 

  .dalign :
  {
    . = ALIGN(4);
    PROVIDE(_data_vma = .);
  } >RAM AT>FLASH  

  .dlalign :
  {
    . = ALIGN(4); 
    PROVIDE(_data_lma = .);
  } >FLASH AT>FLASH

  .data :
  {
    . = ALIGN(4);
    *(.gnu.linkonce.r.*)
    *(.data .data.*)
    *(.gnu.linkonce.d.*)
    . = ALIGN(8);
    /* gp in the middle of the 2 KiB: every variable can be reached from it in one
       instruction (the linker relaxes to gp-relative only well within +-2 KiB) */
    PROVIDE( __global_pointer$ = ORIGIN(RAM) + 0x400 );
    *(.sdata .sdata.*)
    *(.sdata2*)
    *(.gnu.linkonce.s.*)
    . = ALIGN(8);
    *(.srodata.cst16)
    *(.srodata.cst8)
    *(.srodata.cst4)
    *(.srodata.cst2)
    *(.srodata .srodata.*)
    . = ALIGN(4);
    PROVIDE( _edata = .);
  } >RAM AT>FLASH

  .bss :
  {
    . = ALIGN(4);
    PROVIDE( _sbss = .);
    *(.sbss*)
    *(.gnu.linkonce.sb.*)
    *(.bss .bss.[!~]*)
    *(.gnu.linkonce.b.*)    
    *(COMMON*)
    *(.bss.~*)                  /* the page buffer last: next to the stack (port/platform.cpp) */
    . = ALIGN(4);
    PROVIDE( _ebss = .);
  } >RAM AT>FLASH

  PROVIDE( _end = _ebss);
  PROVIDE( end = . );
  PROVIDE( _eusrstack = ORIGIN(RAM) + LENGTH(RAM));	
}
