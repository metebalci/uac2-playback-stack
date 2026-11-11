# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
#
# test_host_budget.cmake: the range of U2PS_HOST_IA_VBUS_RATED_MA, 8 to
# 5,000 mA (u2ps_host.h), which a static assert of host/u2ps_host.c keeps:
# the host stack's source compiles (syntax only) with 8 and 5000, and stops
# at 7 and 5001 with a message that names the constant.
#
#   cmake -DU2PS_ROOT=<tree> -DC_COMPILER=<cc> -P test_host_budget.cmake

if(NOT U2PS_ROOT OR NOT C_COMPILER)
  message(FATAL_ERROR "test_host_budget: set -DU2PS_ROOT=<tree> and -DC_COMPILER=<cc>")
endif()
set(failures 0)
set(defs -DU2PS_HOST_EP0_BUF=1024u -DU2PS_HOST_ALT_MAX=16u -DU2PS_HOST_SUB_RANGE_MAX=32u
    -DU2PS_HOST_QUEUE_MAX=16u -DU2PS_HOST_DEBT_MAX=8u -DU2PS_HOST_CONTROL_MAX=5000u
    -DU2PS_HOST_ACCEPT_NO_IAD=0 -DU2PS_HOST_STRINGS=1 -DU2PS_HOST_REFUSE_FS=0
    -DU2PS_CONTRACT_CHECKS=0)
foreach(case 8:PASS 5000:PASS 7:STOP 5001:STOP)
  string(REPLACE ":" ";" case ${case})
  list(GET case 0 ma)
  list(GET case 1 want)
  execute_process(
    COMMAND ${C_COMPILER} -std=c11 -fsyntax-only ${defs} -DU2PS_HOST_IA_VBUS_RATED_MA=${ma}u
            -I${U2PS_ROOT}/host -I${U2PS_ROOT}/common -I${U2PS_ROOT}/testkit
            ${U2PS_ROOT}/host/u2ps_host.c
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(want STREQUAL "PASS" AND NOT rc EQUAL 0)
    message("FAIL ${ma} mA: the source did not compile: ${out}")
    math(EXPR failures "${failures} + 1")
  elseif(want STREQUAL "STOP" AND rc EQUAL 0)
    message("FAIL ${ma} mA: the source compiled")
    math(EXPR failures "${failures} + 1")
  elseif(want STREQUAL "STOP" AND NOT out MATCHES "U2PS_HOST_IA_VBUS_RATED_MA must be 8\\.\\.5000")
    message("FAIL ${ma} mA: the stop does not name the constant and its range: ${out}")
    math(EXPR failures "${failures} + 1")
  else()
    message("ok ${ma} mA: ${want}")
  endif()
endforeach()
if(failures GREATER 0)
  message(FATAL_ERROR "test_host_budget: ${failures} failed")
endif()
message("test_host_budget: all cases passed")
