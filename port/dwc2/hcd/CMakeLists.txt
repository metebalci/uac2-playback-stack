# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
# DWC2 host port. Its decision logic (u2ps_dwc2_hiso.c for the stream,
# u2ps_dwc2_hctl.c for the root port and the control channel) has no
# register access and builds on the PC too, for the tests; the rest, and
# the port's configuration header in port/dwc2/, belong to ARM builds only.
#
# Test builds, never a release: U2PS_TEST_INJECT=ON builds the injection
# hook (late re-arms of the data channel on purpose, the handler's pass
# profile; u2ps_dwc2_hcd.h), as it does for the device port.

add_library(u2ps_dwc2_hcd STATIC
  u2ps_dwc2_hiso.c
  u2ps_dwc2_hctl.c)
target_include_directories(u2ps_dwc2_hcd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(u2ps_dwc2_hcd PUBLIC u2ps_dwc2_common)
if(CMAKE_CROSSCOMPILING)
  target_sources(u2ps_dwc2_hcd PRIVATE u2ps_dwc2_hcd.c)
  # u2ps_hport_cfg.h, for the port, the host library and the app
  target_include_directories(u2ps_dwc2_hcd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
  # The port interface header, the host library's build constants and its
  # upcalls: the two libraries link each other
  target_link_libraries(u2ps_dwc2_hcd PUBLIC u2ps_host)
  option(U2PS_TEST_INJECT "Test build: the DWC2 port's injection hook" OFF)
  if(U2PS_TEST_INJECT AND CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$")
    message(FATAL_ERROR "U2PS_TEST_INJECT=ON is for test builds, not for CMAKE_BUILD_TYPE="
                        "${CMAKE_BUILD_TYPE}: set -DU2PS_TEST_INJECT=OFF for a release, or "
                        "-DCMAKE_BUILD_TYPE=RelWithDebInfo (the default) or Debug for a test build")
  endif()
  if(U2PS_TEST_INJECT)
    # Public: the app offers the hook's commands
    target_compile_definitions(u2ps_dwc2_hcd PUBLIC U2PS_TEST_INJECT=1)
  endif()
endif()
