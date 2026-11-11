# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
#
# check_agents_citations.cmake: the citations of AGENTS.md, the integration
# guide, against the tree. Needs only CMake.
#
#   cmake -DU2PS_ROOT=<tree> [-DAGENTS=<guide>] -P tests/check_agents_citations.cmake
#
# The guide defaults to AGENTS.md at the tree's root; without it the check
# says so and passes (CTest counts it as skipped).
#
# 1. A citation `path:lines`, lines being a line number or a range a-b, or
#    several of them separated by commas, names a file of the tree, and
#    every line it cites lies within that file: 1 <= a <= b <= the file's
#    last line.
# 2. A path in backquotes whose first part is a file or directory at the
#    tree's root (common/..., device/..., README.md, ...) names a file or
#    directory of the tree.
# It checks that a citation points into its file, not that the lines say
# what the guide says; that is the reader's part.

if(NOT U2PS_ROOT)
  message(FATAL_ERROR "check_agents_citations: set -DU2PS_ROOT=<tree>")
endif()
get_filename_component(U2PS_ROOT "${U2PS_ROOT}" ABSOLUTE)
if(NOT AGENTS)
  set(AGENTS ${U2PS_ROOT}/AGENTS.md)
endif()
if(NOT EXISTS ${AGENTS})
  message("agents citations: AGENTS.md is absent (${AGENTS}): nothing to check")
  return()
endif()

# The guide's lines as a list: ';', '[' and ']' would split or join list
# elements, so they become other characters first
file(READ ${AGENTS} text)
string(REPLACE ";" "," text "${text}")
string(REPLACE "[" "(" text "${text}")
string(REPLACE "]" ")" text "${text}")
string(REPLACE "\n" ";" lines "${text}")

# The number of lines of a file of the tree, read once
function(line_count path out)
  string(MAKE_C_IDENTIFIER "n_${path}" var)
  if(NOT DEFINED ${var})
    file(READ ${U2PS_ROOT}/${path} body)
    string(REGEX MATCHALL "\n" nl "${body}")
    list(LENGTH nl n)
    string(LENGTH "${body}" len)
    if(len GREATER 0)
      math(EXPR last "${len} - 1")
      string(SUBSTRING "${body}" ${last} 1 tail)
      if(NOT tail STREQUAL "\n")
        math(EXPR n "${n} + 1")
      endif()
    endif()
    set(${var} ${n} PARENT_SCOPE)
    set(${out} ${n} PARENT_SCOPE)
  else()
    set(${out} ${${var}} PARENT_SCOPE)
  endif()
endfunction()

file(GLOB roots RELATIVE ${U2PS_ROOT} ${U2PS_ROOT}/*)
set(problems 0)
set(cites 0)
set(paths 0)
set(no 0)
foreach(line IN LISTS lines)
  math(EXPR no "${no} + 1")
  # 1. path:lines
  string(REGEX MATCHALL "`[A-Za-z0-9_./-]+:[0-9][0-9,-]*`" found "${line}")
  foreach(c IN LISTS found)
    math(EXPR cites "${cites} + 1")
    string(REGEX REPLACE "^`([^:]+):([0-9,-]+)`$" "\\1" path "${c}")
    string(REGEX REPLACE "^`([^:]+):([0-9,-]+)`$" "\\2" spec "${c}")
    if(NOT EXISTS ${U2PS_ROOT}/${path} OR IS_DIRECTORY ${U2PS_ROOT}/${path})
      message("AGENTS.md:${no}: ${c}: no file ${path} in the tree")
      math(EXPR problems "${problems} + 1")
      continue()
    endif()
    line_count(${path} n)
    string(REPLACE "," ";" parts "${spec}")
    foreach(part IN LISTS parts)
      if(NOT part MATCHES "^([0-9]+)(-([0-9]+))?$")
        message("AGENTS.md:${no}: ${c}: '${part}' is not a line or a range a-b")
        math(EXPR problems "${problems} + 1")
        continue()
      endif()
      set(a ${CMAKE_MATCH_1})
      if(CMAKE_MATCH_3)
        set(b ${CMAKE_MATCH_3})
      else()
        set(b ${a})
      endif()
      if(a LESS 1 OR b LESS a OR b GREATER n)
        message("AGENTS.md:${no}: ${c}: lines ${a}-${b} do not lie within ${path}, which has ${n} lines")
        math(EXPR problems "${problems} + 1")
      endif()
    endforeach()
  endforeach()
  # 2. paths of the tree in backquotes
  string(REGEX MATCHALL "`[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]*`" found "${line}")
  foreach(c IN LISTS found)
    string(REGEX REPLACE "^`([^/]+)/(.*)`$" "\\1" first "${c}")
    list(FIND roots "${first}" at)
    if(at EQUAL -1)
      continue()
    endif()
    math(EXPR paths "${paths} + 1")
    string(REGEX REPLACE "^`(.*)`$" "\\1" path "${c}")
    if(NOT EXISTS ${U2PS_ROOT}/${path})
      message("AGENTS.md:${no}: ${c}: no file or directory ${path} in the tree")
      math(EXPR problems "${problems} + 1")
    endif()
  endforeach()
endforeach()

if(problems GREATER 0)
  message(FATAL_ERROR "agents citations: ${problems} problems in ${cites} citations and ${paths} paths")
endif()
message("agents citations: every one of ${cites} citations and ${paths} paths lies in the tree")
