# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
# ChipIdea device port. Its decision logic (u2ps_chipidea_iso.c) has no
# register access and builds on the PC too, for the tests; the rest, and
# the port's configuration header in port/chipidea/, belong to ARM builds
# only. The port has no register code yet, so an app does not link to it.
#
# U2PS_CHIPIDEA_FN_UNIT names the unit of the dTD's Frame Number field,
# which the reference manual does not state: none (the fallback count,
# until a board measurement decides), uframes or frames.

add_library(u2ps_chipidea_dcd STATIC
  u2ps_chipidea_iso.c)
target_include_directories(u2ps_chipidea_dcd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(u2ps_chipidea_dcd PUBLIC u2ps_common)
if(CMAKE_CROSSCOMPILING)
  set(U2PS_CHIPIDEA_FN_UNIT none CACHE STRING "ChipIdea dTD Frame Number unit: none, uframes or frames")
  set_property(CACHE U2PS_CHIPIDEA_FN_UNIT PROPERTY STRINGS none uframes frames)
  if(U2PS_CHIPIDEA_FN_UNIT STREQUAL "none")
    set(fn_unit 0)
  elseif(U2PS_CHIPIDEA_FN_UNIT STREQUAL "uframes")
    set(fn_unit 1)
  elseif(U2PS_CHIPIDEA_FN_UNIT STREQUAL "frames")
    set(fn_unit 2)
  else()
    message(FATAL_ERROR "U2PS_CHIPIDEA_FN_UNIT must be none, uframes or frames, not "
                        "'${U2PS_CHIPIDEA_FN_UNIT}': set -DU2PS_CHIPIDEA_FN_UNIT=none (the default) "
                        "until a board measurement has decided the unit")
  endif()
  target_sources(u2ps_chipidea_dcd PRIVATE u2ps_chipidea_dcd.c)
  message(STATUS "u2ps: the ChipIdea device port has no register code yet; an app for "
                 "${U2PS_CHIP} builds against it but does not link")
  # u2ps_dport_cfg.h, for the port, the device library and the app; the
  # unit is public, so all of them see the same U2PS_DPORT_UFRAME_EXACT
  target_include_directories(u2ps_chipidea_dcd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_compile_definitions(u2ps_chipidea_dcd PUBLIC U2PS_CHIPIDEA_FN_UNIT=${fn_unit})
  # The device library's build constants (U2PS_ISO_OUT_COUNT_MAX sizes the
  # stream's slots) and, later, its upcalls: the two libraries link each
  # other
  target_link_libraries(u2ps_chipidea_dcd PUBLIC u2ps_device)
else()
  # The PC build has no device library here: the slot bound for the tests
  target_compile_definitions(u2ps_chipidea_dcd PUBLIC U2PS_ISO_OUT_COUNT_MAX=32u)
endif()
