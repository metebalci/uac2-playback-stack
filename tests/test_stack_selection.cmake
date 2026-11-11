# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
#
# test_stack_selection.cmake: the stack selection of a PC configure,
# U2PS_DEVICE and U2PS_HOST (CMakeLists.txt). Each case configures the tree
# into WORK/<case> and checks the result:
# - must stop: neither stack, naming both options and what each is for;
#   the device stack without U2PS_VBUS, naming U2PS_VBUS; the host stack
#   without U2PS_HOST_IA_VBUS_RATED_MA, naming it and saying that a
#   device-only build leaves out -DU2PS_HOST=ON, or with one that is no
#   number, naming it;
# - must configure: the host stack alone without U2PS_VBUS, the device
#   stack alone, and both; each builds the libraries of its stacks and
#   their ports only (the targets in the generated build files), and the
#   kit of its stacks.
#
#   cmake -DU2PS_ROOT=<tree> -DWORK=<scratch directory> -DGENERATOR=<generator>
#         [-DMAKE_PROGRAM=<ninja>] [-DC_COMPILER=<cc>] -P test_stack_selection.cmake

if(NOT U2PS_ROOT OR NOT WORK OR NOT GENERATOR)
  message(FATAL_ERROR "test_stack_selection: set -DU2PS_ROOT=<tree>, -DWORK=<scratch directory> "
                      "and -DGENERATOR=<generator>")
endif()
set(failures 0)
set(tool_args -G ${GENERATOR})
if(MAKE_PROGRAM)
  list(APPEND tool_args -DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM})
endif()
if(C_COMPILER)
  list(APPEND tool_args -DCMAKE_C_COMPILER=${C_COMPILER})
endif()

# Configures case 'name' with the options in ARGN; 'want' is PASS or STOP.
# Sets out (the output) and gen (the generated build files) in the
# caller's scope.
function(configure name want)
  set(dir ${WORK}/${name})
  file(REMOVE_RECURSE ${dir})
  execute_process(
    COMMAND ${CMAKE_COMMAND} -S ${U2PS_ROOT} -B ${dir} ${tool_args} ${ARGN}
    RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE o)
  string(REGEX REPLACE "[ \t\r\n]+" " " o "${o}")
  set(out "${o}" PARENT_SCOPE)
  # For the messages: the stop's text, or the output's end
  string(FIND "${o}" "CMake Error" at)
  if(at EQUAL -1)
    string(LENGTH "${o}" n)
    if(n GREATER 300)
      math(EXPR at "${n} - 300")
    else()
      set(at 0)
    endif()
  endif()
  string(SUBSTRING "${o}" ${at} 300 o)
  set(brief "${o}" PARENT_SCOPE)
  set(g "")
  foreach(f build.ninja Makefile)
    if(EXISTS ${dir}/${f})
      file(READ ${dir}/${f} g)
    endif()
  endforeach()
  set(gen "${g}" PARENT_SCOPE)
  if(want STREQUAL "PASS" AND NOT rc EQUAL 0)
    message("FAIL ${name}: the configure stopped (exit ${rc}): ${o}")
    math(EXPR f "${failures} + 1")
    set(failures ${f} PARENT_SCOPE)
  elseif(want STREQUAL "STOP" AND rc EQUAL 0)
    message("FAIL ${name}: the configure did not stop")
    math(EXPR f "${failures} + 1")
    set(failures ${f} PARENT_SCOPE)
  else()
    message("ok ${name}: configure exit ${rc}")
  endif()
endfunction()

# The output of the last case matches each regex in ARGN
function(says name)
  foreach(re IN LISTS ARGN)
    if(NOT out MATCHES "${re}")
      message("FAIL ${name}: the output does not say '${re}': ${brief}")
      math(EXPR f "${failures} + 1")
      set(failures ${f} PARENT_SCOPE)
    endif()
  endforeach()
endfunction()

# The generated build files of the last case have (has) or lack (lacks)
# each library in ARGN
function(builds name want)
  foreach(lib IN LISTS ARGN)
    string(FIND "${gen}" "lib${lib}.a" at)
    if(want STREQUAL "has" AND at EQUAL -1)
      message("FAIL ${name}: no lib${lib}.a in the build files")
      math(EXPR f "${failures} + 1")
      set(failures ${f} PARENT_SCOPE)
    elseif(want STREQUAL "lacks" AND NOT at EQUAL -1)
      message("FAIL ${name}: lib${lib}.a is in the build files")
      math(EXPR f "${failures} + 1")
      set(failures ${f} PARENT_SCOPE)
    endif()
  endforeach()
endfunction()

# Must stop: no stack selected, with or without U2PS_VBUS
configure(none STOP -DU2PS_VBUS=app)
says(none "U2PS_DEVICE" "U2PS_HOST" "DAC" "player")
configure(none_off STOP -DU2PS_DEVICE=OFF -DU2PS_HOST=OFF)
says(none_off "U2PS_DEVICE" "U2PS_HOST" "DAC" "player")
# Must stop: the device stack without U2PS_VBUS
configure(device_novbus STOP -DU2PS_DEVICE=ON)
says(device_novbus "U2PS_VBUS is not set")
configure(both_novbus STOP -DU2PS_DEVICE=ON -DU2PS_HOST=ON -DU2PS_HOST_IA_VBUS_RATED_MA=500)
says(both_novbus "U2PS_VBUS is not set")
# Must stop: the host stack without its power budget, or with one that is
# no number
configure(host_nobudget STOP -DU2PS_HOST=ON)
says(host_nobudget "U2PS_HOST_IA_VBUS_RATED_MA is not set" "8 to 5000"
     "A device-only build leaves out -DU2PS_HOST=ON")
configure(both_nobudget STOP -DU2PS_DEVICE=ON -DU2PS_HOST=ON -DU2PS_VBUS=app)
says(both_nobudget "U2PS_HOST_IA_VBUS_RATED_MA is not set"
     "A device-only build leaves out -DU2PS_HOST=ON")
configure(host_budget_abc STOP -DU2PS_HOST=ON -DU2PS_HOST_IA_VBUS_RATED_MA=500mA)
says(host_budget_abc "U2PS_HOST_IA_VBUS_RATED_MA must be a decimal integer")

# Must configure: the host stack alone, without U2PS_VBUS
configure(host_only PASS -DU2PS_HOST=ON -DU2PS_HOST_IA_VBUS_RATED_MA=500)
says(host_only "u2ps: stacks: host")
builds(host_only has u2ps_host u2ps_testkit_host)
builds(host_only lacks u2ps_device u2ps_testkit_device u2ps_testkit_loopback u2ps_dwc2_dcd
       u2ps_chipidea_dcd)
# The device stack alone
configure(device_only PASS -DU2PS_DEVICE=ON -DU2PS_VBUS=bus)
says(device_only "u2ps: stacks: device")
builds(device_only has u2ps_device u2ps_testkit_device)
builds(device_only lacks u2ps_host u2ps_testkit_host u2ps_testkit_loopback u2ps_dwc2_hcd)
# Both
configure(both PASS -DU2PS_DEVICE=ON -DU2PS_HOST=ON -DU2PS_VBUS=app
          -DU2PS_HOST_IA_VBUS_RATED_MA=500)
says(both "u2ps: stacks: device host")
builds(both has u2ps_device u2ps_host u2ps_testkit_device u2ps_testkit_host
       u2ps_testkit_loopback)

if(failures GREATER 0)
  message(FATAL_ERROR "test_stack_selection: ${failures} failed")
endif()
message("test_stack_selection: all cases passed")
