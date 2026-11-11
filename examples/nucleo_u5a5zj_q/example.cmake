# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
# The NUCLEO-U5A5ZJ-Q example's chip and defaults, which the top-level
# CMakeLists.txt reads before the stacks' options: the app is a DAC (the
# device stack), and the board has VBUS on PC2, which its app reads (VBUS
# mode app, device/CMakeLists.txt).

set(u2ps_example_chip stm32u5a5)
if(CMAKE_CROSSCOMPILING AND U2PS_EXAMPLES AND U2PS_CHIP STREQUAL u2ps_example_chip)
  set(u2ps_example_dir ${CMAKE_CURRENT_LIST_DIR})
  if(NOT DEFINED CACHE{U2PS_DEVICE})
    set(U2PS_DEVICE ON CACHE BOOL "Build the device stack, for a DAC (a UAC2 sink)")
  elseif(NOT U2PS_DEVICE)
    message(FATAL_ERROR "The NUCLEO-U5A5ZJ-Q example is a DAC and needs the device stack, not "
                        "U2PS_DEVICE=${U2PS_DEVICE}: set -DU2PS_DEVICE=ON or leave it unset (the "
                        "example then sets it), or build without -DU2PS_EXAMPLES=ON")
  endif()
  if("${U2PS_VBUS}" STREQUAL "")
    set(U2PS_VBUS app CACHE STRING "VBUS mode: app or bus (no default)" FORCE)
  elseif(NOT "${U2PS_VBUS}" STREQUAL "app")
    message(FATAL_ERROR "The NUCLEO-U5A5ZJ-Q example needs U2PS_VBUS=app, not '${U2PS_VBUS}': "
                        "set -DU2PS_VBUS=app or leave it unset (the example then sets app), or "
                        "build without -DU2PS_EXAMPLES=ON")
  endif()
endif()
