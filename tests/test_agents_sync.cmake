# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
#
# test_agents_sync.cmake: the sync check (check_agents_sync.cmake) against
# scratch guides in WORK. The complete guide is the three app headers and
# the four headers of the PC test kit themselves, which name every symbol
# they declare; the check must pass on
# it, skip without a guide, and fail, naming the symbol, on a guide that
# lacks one symbol of each class, renames one, or names one, or a file,
# that the tree does not have; the check's own parameters are no such
# names.
#
#   cmake -DU2PS_ROOT=<tree> -DWORK=<scratch directory> -P test_agents_sync.cmake

if(NOT U2PS_ROOT OR NOT WORK)
  message(FATAL_ERROR "test_agents_sync: set -DU2PS_ROOT=<tree> and -DWORK=<scratch directory>")
endif()
set(check ${CMAKE_CURRENT_LIST_DIR}/check_agents_sync.cmake)
set(failures 0)

set(full "")
foreach(h device/u2ps_dev.h host/u2ps_host.h common/u2ps_types.h testkit/u2ps_testkit.h
          testkit/u2ps_testkit_dev.h testkit/u2ps_testkit_host.h testkit/u2ps_testkit_loopback.h)
  file(READ ${U2PS_ROOT}/${h} text)
  string(APPEND full "${text}\n")
endforeach()
# How a guide writes options on a command line
string(APPEND full "cmake -DU2PS_EP0_BUF=512 -UU2PS_CHIP\n")

# Runs the check on guide text 'text' (or on no guide, if text is NONE) as
# case 'name'; 'want' is PASS, SKIP or FAIL; on FAIL the output must name
# each of the symbols that follow
function(run name want text)
  set(dir ${WORK}/${name})
  file(MAKE_DIRECTORY ${dir})
  file(REMOVE ${dir}/AGENTS.md)
  if(NOT text STREQUAL "NONE")
    file(WRITE ${dir}/AGENTS.md "${text}")
  endif()
  execute_process(
    COMMAND ${CMAKE_COMMAND} -DU2PS_ROOT=${U2PS_ROOT} -DAGENTS=${dir}/AGENTS.md -P ${check}
    WORKING_DIRECTORY ${WORK}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  set(ok TRUE)
  if(want STREQUAL "PASS")
    if(NOT rc EQUAL 0 OR NOT out MATCHES "AGENTS.md is in sync")
      set(ok FALSE)
    endif()
  elseif(want STREQUAL "SKIP")
    if(NOT rc EQUAL 0 OR NOT out MATCHES "AGENTS.md is absent")
      set(ok FALSE)
    endif()
  else()
    if(rc EQUAL 0)
      set(ok FALSE)
    endif()
    foreach(sym IN LISTS ARGN)
      if(NOT out MATCHES "[^A-Za-z0-9_]${sym}[^A-Za-z0-9_]")
        set(ok FALSE)
        message("  ${name}: the output does not name ${sym}")
      endif()
    endforeach()
  endif()
  if(ok)
    message("ok   ${name} (${want})")
  else()
    message("FAIL ${name}: want ${want}, exit ${rc}, output:\n${out}")
    math(EXPR f "${failures} + 1")
    set(failures ${f} PARENT_SCOPE)
  endif()
endfunction()

# 'text' with every whole-word 'sym' replaced by 'by'
function(replace_word out text sym by)
  string(REGEX REPLACE "([^A-Za-z0-9_])${sym}([^A-Za-z0-9_])" "\\1${by}\\2" text "${text}")
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

run(complete PASS "${full}")
# The tree given as a relative path, as from its root: -DU2PS_ROOT=.
set(root ${U2PS_ROOT})
file(RELATIVE_PATH U2PS_ROOT ${WORK} ${root})
run(relative-root PASS "${full}")
set(U2PS_ROOT ${root})
run(absent SKIP NONE)

# One symbol missing, of each class the check reads
foreach(sym
    u2ps_dev_stream_open           # a call of the device stack
    u2ps_host_drain                # a call of the host stack
    u2ps_app_on_pause              # a callback of the device stack
    u2ps_app_host_on_sent          # a callback of the host stack
    u2ps_stream_cfg_t              # a type
    u2ps_host_stop_request_t       # a type
    U2PS_DESCRIPTION_MANUFACTURER  # an enum value
    U2PS_HOST_STOP_REQUEST_SET_INTERFACE # an enum value
    U2PS_MISS_CORRUPTED            # an enum value (u2ps_types.h)
    U2PS_PAUSE_LONG                # a macro
    U2PS_REQ_DEFER                 # a macro
    U2PS_START_MAX                 # a build constant of the device stack
    U2PS_HOST_DEBT_MAX             # a build constant of the host stack
    u2ps_testkit_dev_hole          # a call of the test kit's device side
    u2ps_testkit_host_inject_late  # a call of the test kit's host side
    u2ps_testkit_loopback_reset    # a call of the test kit's loopback
    u2ps_testkit_call_t            # a type of the test kit
    U2PS_TESTKIT_ON_HOLE           # an enum value of the test kit
    U2PS_TESTKIT_CHECK)            # a macro of the test kit
  replace_word(text "${full}" ${sym} "gone")
  run(missing-${sym} FAIL "${text}" ${sym})
endforeach()

# A renamed symbol: the old name is missing, the new one unknown
replace_word(text "${full}" u2ps_dev_release u2ps_dev_give_back)
run(renamed FAIL "${text}" u2ps_dev_release u2ps_dev_give_back)

# Names the tree does not have: a call, a constant, a header
run(unknown-call FAIL "${full}\nu2ps_host_pause()\n" u2ps_host_pause)
run(unknown-constant FAIL "${full}\n-DU2PS_HOST_RETRIES=3\n" U2PS_HOST_RETRIES)
run(unknown-file FAIL "${full}\nsee device/u2ps_dev_api.h\n" u2ps_dev_api.h)
# A private header's name is not a public symbol
run(private FAIL "${full}\nu2ps_dev_description_check()\n" u2ps_dev_description_check)
run(private-kit FAIL "${full}\nu2ps_testkit_record()\n" u2ps_testkit_record)
# Include guards are no symbols; a constant named only on a command line
# counts
set(text "${full}")
foreach(guard U2PS_DEV_H U2PS_HOST_H U2PS_TYPES_H)
  replace_word(text "${text}" ${guard} "GUARD")
endforeach()
run(guards PASS "${text}")
replace_word(text "${full}" U2PS_HOST_DEBT_MAX "gone")
run(command-line PASS "${text}\n-DU2PS_HOST_DEBT_MAX=8\n")
# A field after its type, and files that exist, are fine
run(fields PASS "${full}\nu2ps_stream_cfg_t.count, device/u2ps_dport.h, cmake/arm-none-eabi.cmake\n")
# The check's own command line, with its parameters, may be quoted
run(own-command PASS
    "${full}\ncmake -DU2PS_ROOT=. -DAGENTS=AGENTS.md -P tests/check_agents_sync.cmake\n")

if(failures GREATER 0)
  message(FATAL_ERROR "test_agents_sync: ${failures} cases failed")
endif()
message("test_agents_sync: every case passed")
