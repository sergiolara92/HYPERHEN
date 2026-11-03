ENTRY(hhp_raidougie_bridge)
SECTIONS {
  . = 0;
  .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) }
  .data : { *(.data*) *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
  ASSERT(SIZEOF(.text)>0 && SIZEOF(.text)<=4096,"Rai process-info gate exceeds its page")
  ASSERT(SIZEOF(.data)==0,"Rai process-info gate must keep no mutable state")
}
