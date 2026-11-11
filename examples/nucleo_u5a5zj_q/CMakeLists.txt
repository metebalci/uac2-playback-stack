# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
# The u2ps test app for the NUCLEO-U5A5ZJ-Q: an ELF, a BIN for
# 0x0800_0000 and a HEX, built with either toolchain file (ATfE or
# arm-none-eabi-gcc). Arm's CMSIS core headers and ST's CMSIS device
# files for the STM32U5 (headers, system and startup files) are fetched at
# configure time, pinned to fixed commits; none of them is in the tree.

include(FetchContent)

# Arm CMSIS 5, CMSIS/Core/Include
FetchContent_Declare(cmsis_5
  URL https://codeload.github.com/ARM-software/CMSIS_5/tar.gz/2b7495b8535bdcb306dac29b9ded4cfb679d7e5c
  URL_HASH SHA256=9f7f0080795b1635b82d9f7b084d37011866cd78365749997391ba0f9602f9fe
  DOWNLOAD_NAME CMSIS_5-2b7495b.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR no-cmake)
# ST cmsis_device_u5
FetchContent_Declare(cmsis_device_u5
  URL https://codeload.github.com/STMicroelectronics/cmsis_device_u5/tar.gz/6e67187dec98035893692ab2923914cb5f4e0117
  URL_HASH SHA256=77ca7b2274c842599d4af3ef5f65ddb9a1cb80d3fbf192596728a26831ebf685
  DOWNLOAD_NAME cmsis_device_u5-6e67187.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR no-cmake)
# SOURCE_SUBDIR names a directory without a CMakeLists.txt, so the
# sources are only fetched, never added to the build as projects
FetchContent_MakeAvailable(cmsis_5 cmsis_device_u5)

enable_language(ASM)

set(vendor_sources
  ${cmsis_device_u5_SOURCE_DIR}/Source/Templates/system_stm32u5xx.c
  ${cmsis_device_u5_SOURCE_DIR}/Source/Templates/gcc/startup_stm32u5a5xx.s)
# Vendor code is used as it is; its warnings are not ours
set_source_files_properties(${vendor_sources} PROPERTIES COMPILE_OPTIONS "-w")

set(app u2ps-test-nucleo_u5a5zj_q)
set(linker_script ${CMAKE_CURRENT_SOURCE_DIR}/stm32u5a5zj.ld)
add_executable(${app} main.c board.c ${vendor_sources})
set_target_properties(${app} PROPERTIES SUFFIX .elf)
# The serial number's prefix: the shared VID/PID pair 16C0:27E0 asks for a
# serial number that starts with the distributor's domain or e-mail address
set(U2PS_EXAMPLE_SERIAL_PREFIX "u2ps.metebalci.com:" CACHE STRING
    "Prefix of the example's USB serial number: your domain or e-mail and a colon")
target_compile_definitions(${app} PRIVATE STM32U5A5xx
  U2PS_EXAMPLE_SERIAL_PREFIX="${U2PS_EXAMPLE_SERIAL_PREFIX}")
# Bench test, never a release: the stall mode (app_stall.h), in which the
# app holds every packet it may hold for 1 s every 3 s
option(U2PS_EXAMPLE_STALL "Bench test: the example holds every packet it may hold for 1 s every 3 s" OFF)
if(U2PS_EXAMPLE_STALL)
  target_compile_definitions(${app} PRIVATE U2PS_EXAMPLE_STALL=1)
endif()
# Vendor headers as system headers, so that their warnings stay out of ours
target_include_directories(${app} SYSTEM PRIVATE
  ${cmsis_5_SOURCE_DIR}/CMSIS/Core/Include
  ${cmsis_device_u5_SOURCE_DIR}/Include)
target_link_libraries(${app} PRIVATE u2ps_device u2ps_dwc2_dcd)
# The C library: with ATfE (clang and lld) its default, picolibc; with
# arm-none-eabi-gcc newlib-nano. The app takes only memcpy, memset and
# __libc_init_array from it, which the startup file calls
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
  set(libc_options "")
else()
  set(libc_options --specs=nano.specs)
endif()
target_link_options(${app} PRIVATE
  -T${linker_script} -nostartfiles ${libc_options}
  -Wl,--gc-sections -Wl,-Map=${app}.map -Wl,--print-memory-usage)
# The masked regions of the device stack are timed (main.c) by a wrap of
# the port's mask call, which link-time optimisation (U2PS_LTO) bypasses:
# there the calls are resolved inside the optimised libraries
if(U2PS_LTO)
  target_compile_definitions(${app} PRIVATE U2PS_EXAMPLE_MASK_TIMED=0)
else()
  target_compile_definitions(${app} PRIVATE U2PS_EXAMPLE_MASK_TIMED=1)
  target_link_options(${app} PRIVATE -Wl,--wrap=u2ps_dport_irq_mask)
endif()
set_property(TARGET ${app} APPEND PROPERTY LINK_DEPENDS ${linker_script})
add_custom_command(TARGET ${app} POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary ${app}.elf ${app}.bin
  COMMAND ${CMAKE_OBJCOPY} -O ihex ${app}.elf ${app}.hex
  COMMAND ${CMAKE_SIZE} ${app}.elf
  BYPRODUCTS ${app}.bin ${app}.hex)
