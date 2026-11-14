# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
#
# u2ps: one CMake project for two kinds of build, of the stacks that
# U2PS_DEVICE (a DAC) and U2PS_HOST (a player) select.
# - PC build (host compiler, no toolchain file): common, device and host
#   libraries with ASan and UBSan, the PC test kit (testkit/), and, when
#   u2ps is the top-level project, its tests under CTest. Of the ports only
#   the decision logic of the DWC2 and ChipIdea device ports and of the
#   DWC2 host port is built, which has no register access; the tests and
#   the kit supply fake ports.
# - ARM build (cmake/arm-none-eabi-clang.cmake for ATfE, or
#   cmake/arm-none-eabi.cmake for GCC; U2PS_CHIP set): static libraries
#   only, the device library linked to the chip's port, the host library to
#   the DWC2 host port on a DWC2 chip.

cmake_minimum_required(VERSION 3.22)
project(u2ps VERSION 0.1.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The build type of u2ps's own builds; an integrator's project keeps its
# own, empty or not (tests/test_build_type.cmake)
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
if(NOT PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "u2ps: CMAKE_BUILD_TYPE is empty, so u2ps's libraries build without "
                 "optimisation: set -DCMAKE_BUILD_TYPE=RelWithDebInfo, or set it before project()")
endif()

set(U2PS_CHIP "" CACHE STRING "Target chip of an ARM build: stm32f723, stm32u5a5, stm32h7s3 or imxrt1062")
set_property(CACHE U2PS_CHIP PROPERTY STRINGS "" stm32f723 stm32u5a5 stm32h7s3 imxrt1062)
set(U2PS_PORT "" CACHE STRING "Port of the device library: dwc2 or chipidea (default: the chip's)")
set_property(CACHE U2PS_PORT PROPERTY STRINGS "" dwc2 chipidea)
option(U2PS_EXAMPLES "ARM build: also build the chip's example app (stm32u5a5: the NUCLEO-U5A5ZJ-Q test app)" OFF)

if(CMAKE_CROSSCOMPILING)
  # The controller each chip carries
  if(U2PS_CHIP STREQUAL "stm32f723" OR U2PS_CHIP STREQUAL "stm32u5a5" OR
     U2PS_CHIP STREQUAL "stm32h7s3")
    set(chip_port dwc2)
  elseif(U2PS_CHIP STREQUAL "imxrt1062")
    set(chip_port chipidea)
  else()
    message(FATAL_ERROR "U2PS_CHIP must be stm32f723, stm32u5a5, stm32h7s3 or imxrt1062, not "
                        "'${U2PS_CHIP}': set -DU2PS_CHIP to one of them for an ARM build")
  endif()
  if(U2PS_PORT STREQUAL "")
    set(U2PS_PORT ${chip_port} CACHE STRING "Port of the device library: dwc2 or chipidea (default: the chip's)" FORCE)
  elseif(NOT U2PS_PORT STREQUAL chip_port)
    message(FATAL_ERROR "U2PS_PORT=${U2PS_PORT} does not match U2PS_CHIP=${U2PS_CHIP}, whose "
                        "controller is ${chip_port}: leave U2PS_PORT unset (it defaults to the chip's) "
                        "or set -DU2PS_PORT=${chip_port}")
  endif()
else()
  if(NOT U2PS_CHIP STREQUAL "" OR NOT U2PS_PORT STREQUAL "")
    message(FATAL_ERROR "U2PS_CHIP and U2PS_PORT apply to ARM builds only: add "
                        "-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-clang.cmake (ATfE, with "
                        "-DU2PS_ATFE_DIR) or cmake/arm-none-eabi.cmake (GCC) for one, or leave both "
                        "unset for the PC build, which uses fake ports (-UU2PS_CHIP -UU2PS_PORT "
                        "clears them in an existing build directory)")
  endif()
endif()

add_compile_options(
  -Wall -Wextra -Werror
  -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wwrite-strings
  -Wdouble-promotion)

if(CMAKE_CROSSCOMPILING)
  add_compile_options(-ffunction-sections -fdata-sections)
else()
  # Any sanitizer report ends the program with a failing exit code
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined -fno-sanitize-recover=all)
endif()

# A build constant given on the command line must be a decimal integer;
# the C code checks its range, and the header named here states it
function(u2ps_check_number var default header)
  if(NOT "${${var}}" MATCHES "^[0-9]+$")
    message(FATAL_ERROR "${var} must be a decimal integer, not '${${var}}': set -D${var} to one "
                        "(default ${default}; ${header} gives its range)")
  endif()
endfunction()

# Example apps, ARM builds only: each examples/<board>/example.cmake names
# the chip its app runs on (u2ps_example_chip) and, in an ARM build of
# that chip with U2PS_EXAMPLES=ON, selects the stack the app runs and sets
# the constants its board needs (u2ps_example_dir names the app's
# directory then). examples/pc_testkit/ is not a board's and has none.
set(u2ps_example_chips "")
set(u2ps_example_dir "")
file(GLOB u2ps_example_files LIST_DIRECTORIES false
     ${PROJECT_SOURCE_DIR}/examples/*/example.cmake)
list(SORT u2ps_example_files)
foreach(f ${u2ps_example_files})
  set(u2ps_example_chip "")
  include(${f})
  list(APPEND u2ps_example_chips ${u2ps_example_chip})
endforeach()

# Contract checks (u2ps_types.h, u2ps_contract_t) for the device and the
# host library alike: the app must then define u2ps_app_on_contract().
# The libraries the tests link have them on in any case.
option(U2PS_CONTRACT_CHECKS "Check the app's contracts at run time and report violations through u2ps_app_on_contract()" OFF)
if(U2PS_CONTRACT_CHECKS)
  set(u2ps_checks 1)
else()
  set(u2ps_checks 0)
endif()
# The stacks: the device stack for a DAC, the host stack for a player, or
# both. The build has no default: an integration names what it links.
option(U2PS_DEVICE "Build the device stack, for a DAC (a UAC2 sink): u2ps_device" OFF)
option(U2PS_HOST "Build the host stack, for a player (plays to a UAC2 device): u2ps_host" OFF)
if(NOT U2PS_DEVICE AND NOT U2PS_HOST)
  message(FATAL_ERROR "No stack selected: set -DU2PS_DEVICE=ON for a DAC (the device stack, a "
                      "UAC2 sink, CMake target u2ps_device), -DU2PS_HOST=ON for a player (the "
                      "host stack, which plays to a UAC2 device, CMake target u2ps_host), or "
                      "both. U2PS_VBUS is needed only with U2PS_DEVICE=ON.")
endif()
# The STM32H7S3 has the host stack only, for now: its errata sheet lists two
# OTG_HS faults of IN endpoints whose workarounds the DWC2 device port does
# not have yet (ES0596 Rev 10, 2.21.1 and 2.21.3; u2ps_dwc2_chip.h)
if(CMAKE_CROSSCOMPILING AND U2PS_CHIP STREQUAL "stm32h7s3" AND U2PS_DEVICE)
  message(FATAL_ERROR "U2PS_DEVICE=ON is not available for U2PS_CHIP=stm32h7s3 yet: the DWC2 "
                      "device port has no workaround for errata ES0596 2.21.1 (an isochronous "
                      "IN endpoint disabled during a packet fetch sends the previous packet) and "
                      "2.21.3 (an unexpected packet instead of a zero-length packet). Build the "
                      "host stack alone: -DU2PS_HOST=ON -DU2PS_DEVICE=OFF")
endif()
set(u2ps_stacks "")
if(U2PS_DEVICE)
  list(APPEND u2ps_stacks device)
endif()
if(U2PS_HOST)
  list(APPEND u2ps_stacks host)
endif()

add_subdirectory(common)
add_subdirectory(port)
if(U2PS_DEVICE)
  add_subdirectory(device)
endif()
# The host library builds for every target; only DWC2 has a host port so
# far, which it links (host/CMakeLists.txt)
if(U2PS_HOST)
  add_subdirectory(host)
endif()

# Link-time optimisation of the libraries an integration links, through
# CMake's INTERPROCEDURAL_OPTIMIZATION. Their objects then hold the
# compiler's intermediate code only, so the app's link must run LTO with
# the same compiler: each library passes the LTO flag on to the link of
# whatever links it.
# - GCC: -flto=auto -fno-fat-lto-objects, archived with gcc-ar; the link
#   gets -flto=auto.
# - Clang (ATfE): -flto=thin, archived with llvm-ar; the link gets
#   -flto=thin, so that clang hands lld's LTO the CPU and the optimisation
#   level.
# The sections of the packet path (U2PS_FAST_TEXT, U2PS_FAST_BSS) stay
# named.
option(U2PS_LTO "Link-time optimisation (-flto) of u2ps's libraries; the app's link then runs LTO" OFF)
if(U2PS_LTO)
  if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set(u2ps_lto_link ${CMAKE_C_COMPILE_OPTIONS_IPO})
  else()
    set(u2ps_lto_link -flto=auto)
  endif()
  foreach(lib u2ps_device u2ps_host u2ps_dwc2_common u2ps_dwc2_dcd u2ps_dwc2_hcd u2ps_chipidea_dcd)
    if(TARGET ${lib})
      set_property(TARGET ${lib} PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
      target_link_options(${lib} INTERFACE ${u2ps_lto_link})
    endif()
  endforeach()
  message(STATUS "u2ps: link-time optimisation of the libraries (U2PS_LTO), the link gets ${u2ps_lto_link}")
endif()

if(NOT CMAKE_CROSSCOMPILING)
  # The PC test kit, for integrators too (testkit/CMakeLists.txt)
  add_subdirectory(testkit)
  # u2ps's own tests and the worked example of the kit, only when u2ps is
  # the top-level project, not when an integration adds it
  if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(tests)
    if(U2PS_DEVICE AND U2PS_HOST)
      add_subdirectory(examples/pc_testkit)
    endif()
  endif()
endif()

# Example apps: ARM builds only, one board per chip so far
if(CMAKE_CROSSCOMPILING AND U2PS_EXAMPLES)
  if(u2ps_example_dir)
    add_subdirectory(${u2ps_example_dir})
  else()
    list(JOIN u2ps_example_chips ", " chips)
    message(FATAL_ERROR "U2PS_EXAMPLES=ON: no example app for U2PS_CHIP=${U2PS_CHIP} yet (the "
                        "chips with one: ${chips}): set -DU2PS_EXAMPLES=OFF, or -DU2PS_CHIP to "
                        "one of them")
  endif()
endif()

if(CMAKE_CROSSCOMPILING AND U2PS_DEVICE)
  message(STATUS "u2ps: ARM build for ${U2PS_CHIP}, device port ${U2PS_PORT}")
elseif(CMAKE_CROSSCOMPILING)
  message(STATUS "u2ps: ARM build for ${U2PS_CHIP}, port ${U2PS_PORT}")
else()
  message(STATUS "u2ps: PC build with ASan and UBSan, fake ports")
endif()
list(JOIN u2ps_stacks " " stacks)
message(STATUS "u2ps: stacks: ${stacks}")
