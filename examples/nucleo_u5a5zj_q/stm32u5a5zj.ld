/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Mete Balci */
/* STM32U5A5ZJ: 4 MB flash at 0x0800_0000; SRAM1, SRAM2, SRAM3 and SRAM5
 * form one 2496 KB block from 0x2000_0000 (RM0456 2.3.2, Table 6). The
 * OTG_HS DMA reaches all of it, so the whole block is DMA memory; SRAM4
 * (0x2800_0000), which the DMA does not reach (RM0456 2.1.6), is not
 * used. The image runs with TrustZone disabled (TZEN = 0). */

ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 4096K
  RAM   (rw) : ORIGIN = 0x20000000, LENGTH = 2496K
}

_stack_size = 16K;
_estack = ORIGIN(RAM) + LENGTH(RAM);
_sstack = _estack - _stack_size;  /* MSPLIM */

SECTIONS
{
  .isr_vector :
  {
    KEEP(*(.isr_vector))
  } > FLASH

  .text :
  {
    *(.text .text.*)
    *(.glue_7) *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
  } > FLASH

  .rodata :
  {
    *(.rodata .rodata.*)
    . = ALIGN(4);
  } > FLASH

  .ARM.exidx :
  {
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
  } > FLASH

  /* newlib's __libc_init_array (GCC) calls the two lists by their own
   * bounds, picolibc's (ATfE) as one list from __bothinit_array_start to
   * __bothinit_array_end, which needs them adjacent (asserted below) */
  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    PROVIDE_HIDDEN(__bothinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } > FLASH

  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
    PROVIDE_HIDDEN(__bothinit_array_end = .);
  } > FLASH
  ASSERT(__preinit_array_end == __init_array_start,
         "the preinit and init lists must be adjacent for picolibc's __libc_init_array")

  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } > FLASH

  /* Copied from flash to RAM by the startup code */
  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data .data.*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH

  /* Zeroed by the startup code. DMA memory first: the device stack's
   * control buffer and the port's memory (U2PS_DMA_SECTION) */
  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss.u2ps_dma)
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } > RAM

  /* The stack grows down from _estack; this only reserves its room */
  .stack (NOLOAD) :
  {
    . = ALIGN(8);
    . = . + _stack_size;
  } > RAM

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
