# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
#
# test_agents_citations.cmake: the citation check
# (check_agents_citations.cmake) against scratch guides in WORK, which cite
# this tree's LICENSE (its last line is 'n'). The check must pass on a guide
# whose citations all lie in the tree (one line, the last line, a range,
# several ranges, a path of the tree), skip without a guide, and fail,
# naming the citation, on a file that does not exist, line 0, a line past
# the end, a range past the end, a range backwards, and a path of the
# tree that does not exist.
#
#   cmake -DU2PS_ROOT=<tree> -DWORK=<scratch directory> -P test_agents_citations.cmake

if(NOT U2PS_ROOT OR NOT WORK)
  message(FATAL_ERROR "test_agents_citations: set -DU2PS_ROOT=<tree> and -DWORK=<scratch directory>")
endif()
get_filename_component(U2PS_ROOT "${U2PS_ROOT}" ABSOLUTE)
set(check ${CMAKE_CURRENT_LIST_DIR}/check_agents_citations.cmake)
set(failures 0)
file(READ ${U2PS_ROOT}/LICENSE body)
string(REGEX MATCHALL "\n" nl "${body}")
list(LENGTH nl n)
math(EXPR past "${n} + 1")

# Runs the check on guide text 'text' (or on no guide, if text is NONE) as
# case 'name'; 'want' is PASS, SKIP or FAIL; on FAIL the output must hold
# the text that follows
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
    if(NOT rc EQUAL 0 OR NOT out MATCHES "lies in the tree")
      set(ok FALSE)
    endif()
  elseif(want STREQUAL "SKIP")
    if(NOT rc EQUAL 0 OR NOT out MATCHES "AGENTS.md is absent")
      set(ok FALSE)
    endif()
  else()
    string(FIND "${out}" "${ARGN}" at)
    if(rc EQUAL 0 OR at EQUAL -1)
      set(ok FALSE)
    endif()
  endif()
  if(ok)
    message("ok ${name}: ${want}")
  else()
    message("FAIL ${name}: want ${want}, exit ${rc}:\n${out}")
    math(EXPR failures "${failures} + 1")
    set(failures ${failures} PARENT_SCOPE)
  endif()
endfunction()

run(good PASS "Cited: (`LICENSE:1`, `LICENSE:${n}`, `LICENSE:1-${n}`, `LICENSE:1-2,4,5-${n}`), and `tests/check.h`; not a path: `bin/clang`.\n")
run(none SKIP NONE)
run(missing FAIL "A file that is not there (`device/no_such_file.h:1`).\n" "`device/no_such_file.h:1`: no file")
run(zero FAIL "Line 0 (`LICENSE:0`).\n" "`LICENSE:0`: lines 0-0 do not lie")
run(past FAIL "One past the end (`LICENSE:${past}`).\n" "`LICENSE:${past}`: lines ${past}-${past} do not lie")
run(range_past FAIL "A range past the end (`LICENSE:2-${past}`).\n" "`LICENSE:2-${past}`: lines 2-${past} do not lie")
run(backwards FAIL "Backwards (`LICENSE:5-3`).\n" "`LICENSE:5-3`: lines 5-3 do not lie")
run(second_part FAIL "The second range wrong (`LICENSE:1-2,${past}`).\n" "`LICENSE:1-2,${past}`: lines ${past}-${past} do not lie")
run(no_path FAIL "A path of the tree that is not there: `tests/no_such_dir/x.c`.\n" "`tests/no_such_dir/x.c`: no file or directory")
if(failures GREATER 0)
  message(FATAL_ERROR "test_agents_citations: ${failures} failed")
endif()
message("test_agents_citations: all cases passed")
