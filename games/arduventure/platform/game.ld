ENTRY( jump_reset )

/* Arduventure on the console: resident code, then the overlay window, then the loader
   kernel (KBASE). OVL_BASE and KBASE come from the makefile. */
MEMORY
{
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = OVL_BASE
  WINDOW (rx): ORIGIN = OVL_BASE, LENGTH = KBASE - OVL_BASE
  RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 2K
  FAR (r)    : ORIGIN = 0x30000000, LENGTH = 64K   /* data in the EEPROM (port/far.h) */
  OVLLMA (r) : ORIGIN = 0x40000000, LENGTH = 64K   /* overlay images (go to the EEPROM) */
}

SECTIONS
{
  .far : { KEEP(*(.far .far.*)) __far_end = .; } > FAR

  /* the overlay groups share one flash window; each image starts with OVL_TAG + its
     number (OVL_TAG: this build's ID in the upper 16 bits, from the makefile), so what
     another program or an older build left in the window never passes for an overlay.
     NOCROSSREFS: the linker refuses a reference from one group into another */
  OVERLAY : NOCROSSREFS
  {
    .ov1 { LONG(OVL_TAG + 1) KEEP(*(.ov1.text*)) . = ALIGN(4); }
    .ov2 { LONG(OVL_TAG + 2) KEEP(*(.ov2.text*)) . = ALIGN(4); }
    .ov3 { LONG(OVL_TAG + 3) KEEP(*(.ov3.text*)) . = ALIGN(4); }
    .ov4 { LONG(OVL_TAG + 4) KEEP(*(.ov4.text*)) . = ALIGN(4); }
  } > WINDOW AT > OVLLMA
  __ovl_base = ORIGIN(WINDOW);
  __ovl_size = LENGTH(WINDOW);

  .init :
  { 
    _sinit = .;
    . = ALIGN(4);
    KEEP(*(SORT_NONE(.init.jump)))
    KEEP(*(SORT_NONE(.init.vectors)))
    . = ALIGN(4);
    _einit = .;
  } >FLASH AT>FLASH

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
    PROVIDE( __global_pointer$ = . + 0x800 );
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
    *(.bss*)
    *(.gnu.linkonce.b.*)    
    *(COMMON*)
    . = ALIGN(4);
    PROVIDE( _ebss = .);
  } >RAM AT>FLASH

  PROVIDE( _end = _ebss);
  PROVIDE( end = . );
  PROVIDE( _eusrstack = ORIGIN(RAM) + LENGTH(RAM));	
}
