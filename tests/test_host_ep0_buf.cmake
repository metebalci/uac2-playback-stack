# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
#
# test_host_ep0_buf.cmake: the range of U2PS_HOST_EP0_BUF, a multiple of
# 64 from 256 to 65472 (u2ps_host.h), which a static assert of
# host/u2ps_host.c keeps: a control IN data stage reserves whole packets of
# up to 64 bytes (USB 2.0 5.5.3), so the buffer must end on one. The host
# stack's source compiles (syntax only) with 256, 448, 1024 and 65472, and
# stops at 192, 224, 416, 480, 65504 and 65536 with a message that names
# the constant and its range.
#
#   cmake -DU2PS_ROOT=<tree> -DC_COMPILER=<cc> -P test_host_ep0_buf.cmake

if(NOT U2PS_ROOT OR NOT C_COMPILER)
  message(FATAL_ERROR "test_host_ep0_buf: set -DU2PS_ROOT=<tree> and -DC_COMPILER=<cc>")
endif()
set(failures 0)
# U2PS_HOST_SUB_RANGE_MAX 21: its RANGE, 2 + 12 * 21 = 254 bytes, fits the
# smallest buffer
set(defs -DU2PS_HOST_ALT_MAX=16u -DU2PS_HOST_SUB_RANGE_MAX=21u
    -DU2PS_HOST_QUEUE_MAX=16u -DU2PS_HOST_DEBT_MAX=8u -DU2PS_HOST_CONTROL_MAX=5000u
    -DU2PS_HOST_IA_VBUS_RATED_MA=500u -DU2PS_HOST_ACCEPT_NO_IAD=0 -DU2PS_HOST_STRINGS=1
    -DU2PS_HOST_REFUSE_FS=0 -DU2PS_CONTRACT_CHECKS=0)
foreach(case 256:PASS 448:PASS 1024:PASS 65472:PASS 192:STOP 224:STOP 416:STOP 480:STOP
             65504:STOP 65536:STOP)
  string(REPLACE ":" ";" case ${case})
  list(GET case 0 bytes)
  list(GET case 1 want)
  execute_process(
    COMMAND ${C_COMPILER} -std=c11 -fsyntax-only ${defs} -DU2PS_HOST_EP0_BUF=${bytes}u
            -I${U2PS_ROOT}/host -I${U2PS_ROOT}/common -I${U2PS_ROOT}/testkit
            ${U2PS_ROOT}/host/u2ps_host.c
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(want STREQUAL "PASS" AND NOT rc EQUAL 0)
    message("FAIL ${bytes} bytes: the source did not compile: ${out}")
    math(EXPR failures "${failures} + 1")
  elseif(want STREQUAL "STOP" AND rc EQUAL 0)
    message("FAIL ${bytes} bytes: the source compiled")
    math(EXPR failures "${failures} + 1")
  elseif(want STREQUAL "STOP" AND
         NOT out MATCHES "U2PS_HOST_EP0_BUF must be a multiple of 64, 256\\.\\.65472")
    message("FAIL ${bytes} bytes: the stop does not name the constant and its range: ${out}")
    math(EXPR failures "${failures} + 1")
  else()
    message("ok ${bytes} bytes: ${want}")
  endif()
endforeach()
if(failures GREATER 0)
  message(FATAL_ERROR "test_host_ep0_buf: ${failures} failed")
endif()
message("test_host_ep0_buf: all cases passed")
