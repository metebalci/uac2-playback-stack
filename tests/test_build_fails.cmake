# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
#
# test_build_fails.cmake: a build that must stop at a given message. Builds
# TARGET (excluded from the default build) in the build tree BUILD and
# passes only if the build fails and its output contains EXPECT, so that
# the failure is the one the test means and not another error.
#
#   cmake -DBUILD=<dir> -DTARGET=<target> -DEXPECT=<text> -P tests/test_build_fails.cmake

foreach(v BUILD TARGET EXPECT)
  if(NOT ${v})
    message(FATAL_ERROR "test_build_fails: set -D${v}")
  endif()
endforeach()
execute_process(COMMAND ${CMAKE_COMMAND} --build ${BUILD} --target ${TARGET}
                RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE out)
string(FIND "${out}" "${EXPECT}" at)
if(code EQUAL 0)
  message(FATAL_ERROR "test_build_fails: ${TARGET} built, but must stop with '${EXPECT}'")
elseif(at EQUAL -1)
  message(FATAL_ERROR "test_build_fails: ${TARGET} failed without '${EXPECT}':\n${out}")
endif()
message("test_build_fails: ${TARGET} stops with '${EXPECT}', as it must")
