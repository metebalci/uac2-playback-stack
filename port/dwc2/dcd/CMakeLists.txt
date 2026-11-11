# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
# DWC2 device port. Its decision logic (u2ps_dwc2_iso.c) has no register
# access and builds on the PC too, for the tests; the rest, and the port's
# configuration header in port/dwc2/, belong to ARM builds only.
#
# U2PS_DWC2_FORCE_FS=ON makes the core enumerate at full speed only
# (OTG_DCFG.DSPD = 01), for bench checks of full-speed behaviour.
# U2PS_DWC2_OUT_FOLLOW=OFF starts the port with the data endpoint's parity
# follow off (u2ps_dwc2_dcd_mode_t.out_follow), for an A/B check against
# the parity guessed once at the open.
#
# Test builds, never a release: U2PS_TEST_INJECT=ON builds the injection
# hook (late re-arms on purpose, lost ACKs, the handler's pass profile;
# u2ps_dwc2_dcd.h); U2PS_TEST_OLD_RULE=ON, only with it, arms the data
# endpoint by the rule before B-next, for the comparison.

add_library(u2ps_dwc2_dcd STATIC
  u2ps_dwc2_iso.c)
target_include_directories(u2ps_dwc2_dcd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(u2ps_dwc2_dcd PUBLIC u2ps_dwc2_common)
if(CMAKE_CROSSCOMPILING)
  option(U2PS_DWC2_FORCE_FS "DWC2 device port: enumerate at full speed only" OFF)
  target_sources(u2ps_dwc2_dcd PRIVATE u2ps_dwc2_dcd.c)
  # u2ps_dport_cfg.h, for the port, the device library and the app
  target_include_directories(u2ps_dwc2_dcd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
  # The port interface header, the device library's build constants and its
  # upcalls: the two libraries link each other
  target_link_libraries(u2ps_dwc2_dcd PUBLIC u2ps_device)
  if(U2PS_DWC2_FORCE_FS)
    target_compile_definitions(u2ps_dwc2_dcd PRIVATE U2PS_DWC2_FORCE_FS=1)
  endif()
  option(U2PS_DWC2_OUT_FOLLOW "DWC2 device port: the data parity follows the bus until a packet" ON)
  if(NOT U2PS_DWC2_OUT_FOLLOW)
    target_compile_definitions(u2ps_dwc2_dcd PRIVATE U2PS_DWC2_OUT_FOLLOW=0)
  endif()
  option(U2PS_TEST_INJECT "Test build: the DWC2 port's injection hook" OFF)
  option(U2PS_TEST_OLD_RULE "Test build: the re-arm rule before B-next (needs U2PS_TEST_INJECT)" OFF)
  if(U2PS_TEST_OLD_RULE AND NOT U2PS_TEST_INJECT)
    message(FATAL_ERROR "U2PS_TEST_OLD_RULE=ON needs -DU2PS_TEST_INJECT=ON (a test build): set "
                        "both, or -DU2PS_TEST_OLD_RULE=OFF")
  endif()
  if(U2PS_TEST_INJECT AND CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$")
    message(FATAL_ERROR "U2PS_TEST_INJECT=ON is for test builds, not for CMAKE_BUILD_TYPE="
                        "${CMAKE_BUILD_TYPE}: set -DU2PS_TEST_INJECT=OFF for a release, or "
                        "-DCMAKE_BUILD_TYPE=RelWithDebInfo (the default) or Debug for a test build")
  endif()
  if(U2PS_TEST_INJECT)
    message(STATUS "u2ps: test build with the injection hook")
    # Public: the app offers the hook's commands
    target_compile_definitions(u2ps_dwc2_dcd PUBLIC U2PS_TEST_INJECT=1)
  endif()
  if(U2PS_TEST_OLD_RULE)
    message(STATUS "u2ps: test build with the re-arm rule before B-next")
    target_compile_definitions(u2ps_dwc2_dcd PUBLIC U2PS_TEST_OLD_RULE=1)
    target_compile_definitions(u2ps_dwc2_dcd PRIVATE U2PS_DWC2_OLD_RULE=1)
  endif()
endif()
