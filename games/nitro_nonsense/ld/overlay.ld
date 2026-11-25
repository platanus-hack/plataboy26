ENTRY( jump_reset )

/* Only this declared window may be rewritten by the resident overlay loader. */
PROVIDE(OVL_BASE = 0x2F80);
PROVIDE(KBASE = 0x3C40);
MEMORY
{
  FLASH (rx) : ORIGIN = 0, LENGTH = OVL_BASE
  WINDOW (rx) : ORIGIN = OVL_BASE, LENGTH = KBASE - OVL_BASE
  RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 2K
  PAYLOAD (r) : ORIGIN = 0x40000000, LENGTH = 64K
}

SECTIONS
{
  OVERLAY : NOCROSSREFS
  {
    .ov1 { LONG(1) KEEP(*(.ov1.text*)) KEEP(*(.ov1.rodata*)) . = ALIGN(4); }
    .ov2 { LONG(2) KEEP(*(.ov2.text*)) KEEP(*(.ov2.rodata*)) . = ALIGN(4); }
    .ov3 { LONG(3) KEEP(*(.ov3.text*)) KEEP(*(.ov3.rodata*)) . = ALIGN(4); }
    .ov4 { LONG(4) KEEP(*(.ov4.text*)) KEEP(*(.ov4.rodata*)) . = ALIGN(4); }
  } >WINDOW AT>PAYLOAD
  __nn_overlay_base = ORIGIN(WINDOW);
  __nn_overlay_size = LENGTH(WINDOW);
  ASSERT((OVL_BASE & 63) == 0 && (KBASE & 63) == 0, "Overlay window must be page aligned")
  ASSERT(OVL_BASE > 0 && KBASE == 0x3C40, "Overlay window must exclude the kernel")

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

  /* Patched after final linking; never relink against generated code addresses. */
  .nn_overlay_meta (READONLY) :
  {
    . = ALIGN(4);
    __nn_overlay_meta = .;
    LONG(0)
    . += 108;
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

  /* RAM code deliberately accesses the disjoint first scratch page. */
  _sbss = ALIGN(4);
  OVERLAY :
  {
    .nn_framebuffer { KEEP(*(.bss.nn_framebuffer)) }
    .ram_loader { . += 64; KEEP(*(.ram_loader.text*)) . = ALIGN(4); }
  } >RAM AT>PAYLOAD
  ASSERT(SIZEOF(.nn_framebuffer) == 1024, "Framebuffer must occupy exactly 1024 bytes")
  ASSERT(SIZEOF(.ram_loader) <= 1024, "RAM loader exceeds borrowed framebuffer")

  .bss :
  {
    . = ALIGN(4);

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
