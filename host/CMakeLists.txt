# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
# Host stack: enumeration, the configuration walk to the audio function, and
# the audio function (plays to a UAC2 device). It builds for every target;
# an ARM build for a DWC2 chip links the DWC2 host port, the only host port
# so far. The PC build links none, and each test supplies a fake port.
#
# U2PS_HOST_IA_VBUS_RATED_MA, which has no default, is the current in mA
# the board's VBUS supply of the root port is rated for (8 to 5,000): the
# power budget of the configurations the stack selects (u2ps_host.h).
# U2PS_HOST_ACCEPT_NO_IAD=ON accepts a configuration without an audio IAD
# if it has exactly one AudioControl interface, a deviation from UAC2 4.6
# (u2ps_host.h). U2PS_HOST_STRINGS=OFF reads no string descriptors.
# U2PS_HOST_REFUSE_FS=ON ends the session of a full- or low-speed device
# at once, a deviation from USB 2.0 7.1.2.3. U2PS_CONTRACT_CHECKS
# (CMakeLists.txt) checks the app's contracts at run time. The PC build of
# u2ps as the top-level project also builds, for its tests, u2ps_host_plain
# with the defaults, u2ps_host_no_iad with U2PS_HOST_ACCEPT_NO_IAD,
# u2ps_host_no_strings without strings and u2ps_host_refuse_fs with
# U2PS_HOST_REFUSE_FS, all with a 500 mA budget, u2ps_host_ma100, the
# defaults with a 100 mA budget, all with the contract checks, and
# u2ps_host_unchecked, u2ps_host_plain without them. Every PC build builds
# the PC test kit's host stack, u2ps_testkit_host_stack: u2ps_host with the
# contract checks, an object library (testkit/CMakeLists.txt).

option(U2PS_HOST_ACCEPT_NO_IAD "Host: accept a configuration without an audio IAD (one AudioControl interface)" OFF)
option(U2PS_HOST_STRINGS "Host: read the device's strings" ON)
option(U2PS_HOST_REFUSE_FS "Host: end the session of a full- or low-speed device at once" OFF)

set(U2PS_HOST_IA_VBUS_RATED_MA "" CACHE STRING
    "Host: mA the board's VBUS supply of the root port is rated for, 8..5000 (no default)")
if(U2PS_HOST_IA_VBUS_RATED_MA STREQUAL "")
  message(FATAL_ERROR "U2PS_HOST_IA_VBUS_RATED_MA is not set. Set "
                      "-DU2PS_HOST_IA_VBUS_RATED_MA=<mA>, 8 to 5000: the current the board's "
                      "VBUS supply of the host port is rated for (IA_VBUS_RATED), which the "
                      "host stack keeps each configuration's bMaxPower within (u2ps_host.h). "
                      "A device-only build leaves out -DU2PS_HOST=ON")
endif()
u2ps_check_number(U2PS_HOST_IA_VBUS_RATED_MA "none" u2ps_host.h)

# Build constants of the host library (u2ps_host.h). They are public
# compile definitions, so the app that links the library sees the same
# values.
set(U2PS_HOST_EP0_BUF 1024 CACHE STRING "Host: bytes of the control buffer, a multiple of 64, 256 to 65472, at least 2 + 12 * U2PS_HOST_SUB_RANGE_MAX")
set(U2PS_HOST_ALT_MAX 16 CACHE STRING "Host: most streaming alternate settings kept, 1 to 255")
set(U2PS_HOST_SUB_RANGE_MAX 32 CACHE STRING "Host: most sub-ranges of the clock's RANGE kept, 1 to (U2PS_HOST_EP0_BUF - 2) / 12")
set(U2PS_HOST_QUEUE_MAX 16 CACHE STRING "Host: most packets queued at once, 1 to 255")
set(U2PS_HOST_DEBT_MAX 8 CACHE STRING "Host: most microframes' worth of audio slots a hole leaves owed, 1 to 8000")
set(U2PS_HOST_CONTROL_MAX 5000 CACHE STRING "Host: milliseconds a control transfer may take, 1 to 65535")
u2ps_check_number(U2PS_HOST_EP0_BUF 1024 u2ps_host.h)
u2ps_check_number(U2PS_HOST_ALT_MAX 16 u2ps_host.h)
u2ps_check_number(U2PS_HOST_SUB_RANGE_MAX 32 u2ps_host.h)
u2ps_check_number(U2PS_HOST_QUEUE_MAX 16 u2ps_host.h)
u2ps_check_number(U2PS_HOST_DEBT_MAX 8 u2ps_host.h)
u2ps_check_number(U2PS_HOST_CONTROL_MAX 5000 u2ps_host.h)

set(u2ps_host_sources
  u2ps_host.c
  u2ps_host_stream.c
  u2ps_host_walk.c)

# A host library with the no-IAD option or not, strings or not, the
# contract checks or not, a power budget in mA, and full speed refused or
# not; a static library, or the kind given after them
function(u2ps_host_library name no_iad strings checks ma refuse_fs)
  set(kind STATIC)
  if(ARGN)
    set(kind ${ARGN})
  endif()
  add_library(${name} ${kind} ${u2ps_host_sources})
  target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(${name} PUBLIC
    U2PS_HOST_EP0_BUF=${U2PS_HOST_EP0_BUF}u
    U2PS_HOST_ALT_MAX=${U2PS_HOST_ALT_MAX}u
    U2PS_HOST_SUB_RANGE_MAX=${U2PS_HOST_SUB_RANGE_MAX}u
    U2PS_HOST_QUEUE_MAX=${U2PS_HOST_QUEUE_MAX}u
    U2PS_HOST_DEBT_MAX=${U2PS_HOST_DEBT_MAX}u
    U2PS_HOST_CONTROL_MAX=${U2PS_HOST_CONTROL_MAX}u
    U2PS_HOST_ACCEPT_NO_IAD=${no_iad}
    U2PS_HOST_STRINGS=${strings}
    U2PS_HOST_IA_VBUS_RATED_MA=${ma}u
    U2PS_HOST_REFUSE_FS=${refuse_fs}
    U2PS_CONTRACT_CHECKS=${checks})
  target_link_libraries(${name} PUBLIC u2ps_common)
  # The host port's configuration header, u2ps_hport_cfg.h: the DWC2 host
  # port's, ChipIdea's (no host port yet: its constants only), or on the
  # PC the fake host port's of the PC test kit
  if(CMAKE_CROSSCOMPILING AND U2PS_PORT STREQUAL "dwc2")
    target_link_libraries(${name} PUBLIC u2ps_dwc2_hcd)
  elseif(CMAKE_CROSSCOMPILING)
    target_include_directories(${name} PUBLIC ${PROJECT_SOURCE_DIR}/port/${U2PS_PORT})
  else()
    target_include_directories(${name} PUBLIC ${PROJECT_SOURCE_DIR}/testkit)
  endif()
endfunction()

if(U2PS_HOST_ACCEPT_NO_IAD)
  set(no_iad 1)
else()
  set(no_iad 0)
endif()
if(U2PS_HOST_STRINGS)
  set(strings 1)
else()
  set(strings 0)
endif()
if(U2PS_HOST_REFUSE_FS)
  set(refuse_fs 1)
else()
  set(refuse_fs 0)
endif()
set(ma ${U2PS_HOST_IA_VBUS_RATED_MA})
u2ps_host_library(u2ps_host ${no_iad} ${strings} ${u2ps_checks} ${ma} ${refuse_fs})
if(NOT CMAKE_CROSSCOMPILING AND PROJECT_IS_TOP_LEVEL)
  u2ps_host_library(u2ps_host_plain 0 1 1 500 0)
  u2ps_host_library(u2ps_host_no_iad 1 1 1 500 0)
  u2ps_host_library(u2ps_host_no_strings 0 0 1 500 0)
  u2ps_host_library(u2ps_host_refuse_fs 0 1 1 500 1)
  u2ps_host_library(u2ps_host_ma100 0 1 1 100 0)
  u2ps_host_library(u2ps_host_unchecked 0 1 0 500 0)
endif()
if(NOT CMAKE_CROSSCOMPILING)
  u2ps_host_library(u2ps_testkit_host_stack ${no_iad} ${strings} 1 ${ma} ${refuse_fs} OBJECT)
endif()
