# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
# DWC2 code that the device and host ports share: registers, the chip's
# facts, core reset, the start-up for the F723, U5 and H7S3, FIFO sizing,
# the DMA memory rule. The FIFO sizing, the DMA memory rule and the start-
# up's pure parts are logic without register access and build on the PC
# too, for the tests; the rest is built in ARM builds only.

add_library(u2ps_dwc2_common STATIC
  u2ps_dwc2_dma.c
  u2ps_dwc2_fifo.c
  u2ps_dwc2_start.c)
target_include_directories(u2ps_dwc2_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(u2ps_dwc2_common PUBLIC u2ps_common)
if(CMAKE_CROSSCOMPILING)
  target_sources(u2ps_dwc2_common PRIVATE u2ps_dwc2.c u2ps_dwc2_phy.c)
  # The chip, for the code and headers that differ between the chips
  # (u2ps_dwc2_chip.h)
  string(TOUPPER "${U2PS_CHIP}" chip)
  target_compile_definitions(u2ps_dwc2_common PUBLIC U2PS_CHIP_${chip}=1)
  # Per chip: the HSE frequency the PHY takes its reference from, and the
  # highest core clock, for the port's time bounds, which it counts in DWT
  # cycles; each with the frequencies u2ps_dwc2_phy.c and u2ps_dwc2.c take
  # - stm32f723: HSE 25 MHz on the STM32F723E-DISCO (12, 12.5, 16, 24 or
  #   25 MHz); core 216 MHz (30 MHz to 216 MHz)
  # - stm32u5a5: HSE 16 MHz on the NUCLEO-U5A5ZJ-Q and the STM32U5G9J-DK2
  #   (16, 19.2, 20, 24, 26 or 32 MHz); core 160 MHz (30 MHz to 160 MHz)
  # - stm32h7s3: HSE 24 MHz on the NUCLEO-H7S3L8 (16, 19.2, 20, 24, 26 or
  #   32 MHz); core 600 MHz (30 MHz to 600 MHz)
  if(U2PS_CHIP STREQUAL "stm32f723")
    set(hse_default 25000000)
    set(hse_values "12000000, 12500000, 16000000, 24000000 or 25000000")
    set(core_default 216000000)
    set(core_range "above 30 MHz, at most 216 MHz")
  elseif(U2PS_CHIP STREQUAL "stm32u5a5")
    set(hse_default 16000000)
    set(hse_values "16000000, 19200000, 20000000, 24000000, 26000000 or 32000000")
    set(core_default 160000000)
    set(core_range "above 30 MHz, at most 160 MHz")
  elseif(U2PS_CHIP STREQUAL "stm32h7s3")
    set(hse_default 24000000)
    set(hse_values "16000000, 19200000, 20000000, 24000000, 26000000 or 32000000")
    set(core_default 600000000)
    set(core_range "above 30 MHz, at most 600 MHz")
  else()
    message(FATAL_ERROR "port/dwc2/common: no HSE and core clock defaults for U2PS_CHIP="
                        "${U2PS_CHIP}: the DWC2 chips are stm32f723, stm32u5a5 and stm32h7s3; "
                        "set -DU2PS_CHIP to one of them")
  endif()
  set(U2PS_DWC2_HSE_HZ ${hse_default} CACHE STRING
      "HSE frequency in Hz, the DWC2 PHY's reference: ${hse_values} on ${U2PS_CHIP}")
  u2ps_check_number(U2PS_DWC2_HSE_HZ ${hse_default} u2ps_dwc2_phy.c)
  target_compile_definitions(u2ps_dwc2_common PRIVATE U2PS_DWC2_HSE_HZ=${U2PS_DWC2_HSE_HZ})
  set(U2PS_DWC2_CORE_HZ ${core_default} CACHE STRING
      "Highest core clock in Hz, for the DWC2 port's time bounds: ${core_range} on ${U2PS_CHIP}")
  u2ps_check_number(U2PS_DWC2_CORE_HZ ${core_default} u2ps_dwc2.h)
  target_compile_definitions(u2ps_dwc2_common PUBLIC U2PS_DWC2_CORE_HZ=${U2PS_DWC2_CORE_HZ}u)
endif()
