# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
# Device stack: Chapter 9 and the UAC2 function in one module (a UAC2 sink),
# with files split by concern. An ARM build links the port that U2PS_PORT
# names; the PC build links none, and each test supplies a fake port.
#
# U2PS_VBUS, which has no default, says how the stack learns of VBUS: app
# (the app reports it with u2ps_dev_vbus()) or bus (a bus-powered device,
# so VBUS is always present); port (the port senses it) is not available
# yet.
# U2PS_REFUSE_FS=ON builds a device that never enumerates at full speed.
# U2PS_CONTROL_HOOK=ON calls the app's u2ps_app_on_control() for every
# SETUP the stack answers. U2PS_CONTRACT_CHECKS (CMakeLists.txt) checks
# the app's contracts at run time. The PC build of u2ps as the top-level
# project also builds, for its tests, u2ps_device_app and u2ps_device_bus,
# each in its VBUS mode, u2ps_device_refuse_fs, with full speed refused,
# u2ps_device_limits, with small stream limits, and u2ps_device_hook, with
# the control hook; all but u2ps_device_bus in VBUS mode app, and all with
# the contract checks. One more, u2ps_device_unchecked, is u2ps_device_app
# without them. Every PC build builds the PC test kit's device stack,
# u2ps_testkit_device_stack: u2ps_device with the contract checks, an
# object library (testkit/CMakeLists.txt).

set(U2PS_VBUS "" CACHE STRING "VBUS mode: app or bus (no default)")
set_property(CACHE U2PS_VBUS PROPERTY STRINGS app bus port)
if(U2PS_VBUS STREQUAL "")
  message(FATAL_ERROR "U2PS_VBUS is not set. Set -DU2PS_VBUS=app if the app reports VBUS "
                      "with u2ps_dev_vbus(), or -DU2PS_VBUS=bus for a bus-powered device "
                      "whose pull-up takes its supply from VBUS (u2ps_dev.h)")
elseif(U2PS_VBUS STREQUAL "port")
  message(FATAL_ERROR "U2PS_VBUS=port, VBUS sensed by the port, is not available yet: set "
                      "-DU2PS_VBUS=app if the app reports VBUS with u2ps_dev_vbus(), or "
                      "-DU2PS_VBUS=bus for a bus-powered device (u2ps_dev.h)")
elseif(NOT U2PS_VBUS MATCHES "^(app|bus)$")
  message(FATAL_ERROR "U2PS_VBUS must be app or bus, not '${U2PS_VBUS}': set -DU2PS_VBUS=app "
                      "if the app reports VBUS with u2ps_dev_vbus(), or -DU2PS_VBUS=bus for a "
                      "bus-powered device (u2ps_dev.h)")
endif()

option(U2PS_REFUSE_FS "Refuse full speed: stall every GET_DESCRIPTOR at FS" OFF)
option(U2PS_CONTROL_HOOK "Call u2ps_app_on_control() for every SETUP the stack answers" OFF)

# Build constants of the device library (u2ps_dev.h). They are public
# compile definitions, so the app that links the library sees the same
# values.
set(U2PS_EP0_BUF 512 CACHE STRING "Bytes of the control buffer: a multiple of 32, 64 to 65504")
set(U2PS_GAP_PAUSE 400 CACHE STRING "NO_PACKET microframes longer than this are a pause, 1 to 8191")
set(U2PS_CLOSE_MAX 10 CACHE STRING "Milliseconds a stream's close may take, 1 to 65535")
set(U2PS_START_MAX 30 CACHE STRING "Milliseconds SET_INTERFACE's status may wait for a stream start, 1 to 65535")
set(U2PS_ISO_OUT_MPS_MAX 1024 CACHE STRING "Largest wMaxPacketSize of a stream, 1 to 1024")
set(U2PS_ISO_OUT_COUNT_MAX 16 CACHE STRING "Most buffers of a stream, the port's minimum (DWC2 2, ChipIdea 3) to 65535")
u2ps_check_number(U2PS_EP0_BUF 512 u2ps_dev.h)
u2ps_check_number(U2PS_GAP_PAUSE 400 u2ps_dev.h)
u2ps_check_number(U2PS_CLOSE_MAX 10 u2ps_dev.h)
u2ps_check_number(U2PS_START_MAX 30 u2ps_dev.h)
u2ps_check_number(U2PS_ISO_OUT_MPS_MAX 1024 u2ps_dev.h)
u2ps_check_number(U2PS_ISO_OUT_COUNT_MAX 16 u2ps_dev.h)

set(u2ps_device_sources
  u2ps_dev.c
  u2ps_dev_audio.c
  u2ps_dev_ctrl.c
  u2ps_dev_desc.c
  u2ps_dev_std.c)

# A device library with full speed refused or not, the two stream limits,
# the control hook or not, a VBUS mode (app or bus) and the contract
# checks or not; a static library, or the kind given after them
function(u2ps_device_library name refuse_fs mps_max count_max hook vbus checks)
  string(TOUPPER ${vbus} vbus)
  set(kind STATIC)
  if(ARGN)
    set(kind ${ARGN})
  endif()
  add_library(${name} ${kind} ${u2ps_device_sources})
  target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(${name} PUBLIC
    U2PS_VBUS=U2PS_VBUS_${vbus}
    U2PS_REFUSE_FS=${refuse_fs}
    U2PS_CONTROL_HOOK=${hook}
    U2PS_CONTRACT_CHECKS=${checks}
    U2PS_EP0_BUF=${U2PS_EP0_BUF}u
    U2PS_GAP_PAUSE=${U2PS_GAP_PAUSE}u
    U2PS_CLOSE_MAX=${U2PS_CLOSE_MAX}u
    U2PS_START_MAX=${U2PS_START_MAX}u
    U2PS_ISO_OUT_MPS_MAX=${mps_max}u
    U2PS_ISO_OUT_COUNT_MAX=${count_max}u)
  target_link_libraries(${name} PUBLIC u2ps_common)
  if(CMAKE_CROSSCOMPILING)
    target_link_libraries(${name} PUBLIC u2ps_${U2PS_PORT}_dcd)
  endif()
endfunction()

set(limits ${U2PS_ISO_OUT_MPS_MAX} ${U2PS_ISO_OUT_COUNT_MAX})
if(U2PS_REFUSE_FS)
  set(refuse 1)
else()
  set(refuse 0)
endif()
if(U2PS_CONTROL_HOOK)
  set(hook 1)
else()
  set(hook 0)
endif()
u2ps_device_library(u2ps_device ${refuse} ${limits} ${hook} ${U2PS_VBUS} ${u2ps_checks})
if(NOT CMAKE_CROSSCOMPILING AND PROJECT_IS_TOP_LEVEL)
  u2ps_device_library(u2ps_device_app 0 ${limits} 0 app 1)
  u2ps_device_library(u2ps_device_bus 0 ${limits} 0 bus 1)
  u2ps_device_library(u2ps_device_refuse_fs 1 ${limits} 0 app 1)
  # Small stream limits for the tests of U2PS_ISO_OUT_MPS_MAX and
  # U2PS_ISO_OUT_COUNT_MAX
  u2ps_device_library(u2ps_device_limits 0 388 4 0 app 1)
  u2ps_device_library(u2ps_device_hook 0 ${limits} 1 app 1)
  u2ps_device_library(u2ps_device_unchecked 0 ${limits} 0 app 0)
endif()
if(NOT CMAKE_CROSSCOMPILING)
  u2ps_device_library(u2ps_testkit_device_stack ${refuse} ${limits} ${hook} ${U2PS_VBUS} 1 OBJECT)
endif()
