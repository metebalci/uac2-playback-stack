# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
#
# check_agents_sync.cmake: AGENTS.md, the integration guide, against the
# app headers and the PC test kit's headers, both ways. Needs only CMake.
#
#   cmake -DU2PS_ROOT=<tree> [-DAGENTS=<guide>] -P tests/check_agents_sync.cmake
#
# The guide defaults to AGENTS.md at the tree's root; without it the check
# says so and passes (CTest counts it as skipped).
#
# 1. Every public symbol of the app headers, device/u2ps_dev.h,
#    host/u2ps_host.h and common/u2ps_types.h, and of the test kit's
#    headers, testkit/u2ps_testkit.h, u2ps_testkit_dev.h,
#    u2ps_testkit_host.h and u2ps_testkit_loopback.h, appears in the guide
#    as a whole word, outside comments of the headers as declared:
#    - calls and callbacks: every function they declare (u2ps_dev_*,
#      u2ps_host_*, u2ps_app_*, u2ps_app_host_*);
#    - types: every typedef name (u2ps_*_t);
#    - enum values: every enumerator (U2PS_*);
#    - macros: every #define U2PS_* but the include guards;
#    - build constants: every U2PS_* in the headers' defined() checks.
#    A pattern such as U2PS_HOST_END_* names none of them.
# 2. Every u2ps_* and U2PS_* word of the guide exists: as a word outside
#    comments in a public header (a .h file under common/, device/, host/,
#    port/, testkit/ or examples/, but not a *_priv.h), or as a word in a
#    CMake file of the tree (CMakeLists.txt, *.cmake). A word followed by .h, .c,
#    .cmake, .txt or .ld is a file name and must name a file of the tree; a
#    word followed by another .suffix is a symbol (u2ps_stream_cfg_t.count
#    names u2ps_stream_cfg_t). -D and -U before U2PS_ are a command line's.
#    Words that end in _ (a pattern's prefix) are not checked. This
#    check's own parameter U2PS_ROOT counts as known, so the guide can quote
#    the command above (AGENTS has no u2ps prefix and is never checked).

if(NOT U2PS_ROOT)
  message(FATAL_ERROR "check_agents_sync: set -DU2PS_ROOT=<tree>")
endif()
get_filename_component(U2PS_ROOT "${U2PS_ROOT}" ABSOLUTE)
if(NOT AGENTS)
  set(AGENTS ${U2PS_ROOT}/AGENTS.md)
endif()
if(NOT EXISTS ${AGENTS})
  message("agents sync: AGENTS.md is absent (${AGENTS}): nothing to check")
  return()
endif()

# 'text' without its C comments
function(strip_comments out text)
  set(kept "")
  while(TRUE)
    string(FIND "${text}" "/*" a)
    if(a EQUAL -1)
      break()
    endif()
    string(SUBSTRING "${text}" 0 ${a} before)
    string(APPEND kept "${before} ")
    math(EXPR a "${a} + 2")
    string(SUBSTRING "${text}" ${a} -1 text)
    string(FIND "${text}" "*/" b)
    if(b EQUAL -1)
      set(text "")
      break()
    endif()
    math(EXPR b "${b} + 2")
    string(SUBSTRING "${text}" ${b} -1 text)
  endwhile()
  string(APPEND kept "${text}")
  set(${out} "${kept}" PARENT_SCOPE)
endfunction()

# Appends to list 'out' the matches of 'regex' in 'text', each without
# the parts that 'strip' matches
function(collect out regex strip text)
  string(REGEX MATCHALL "${regex}" found "${text}")
  set(list ${${out}})
  foreach(f IN LISTS found)
    string(REGEX REPLACE "${strip}" "" f "${f}")
    list(APPEND list ${f})
  endforeach()
  set(${out} ${list} PARENT_SCOPE)
endfunction()

# ---- 1. The symbols of the app headers ----
set(classes calls callbacks types enum_values macros build_constants)
foreach(c IN LISTS classes)
  set(${c} "")
endforeach()
foreach(h device/u2ps_dev.h host/u2ps_host.h common/u2ps_types.h testkit/u2ps_testkit.h
          testkit/u2ps_testkit_dev.h testkit/u2ps_testkit_host.h testkit/u2ps_testkit_loopback.h)
  file(READ ${U2PS_ROOT}/${h} text)
  strip_comments(text "${text}")
  set(functions "")
  collect(functions "u2ps_[a-z0-9_]+\\(" "\\($" "${text}")
  foreach(f IN LISTS functions)
    if(f MATCHES "^u2ps_app_")
      list(APPEND callbacks ${f})
    else()
      list(APPEND calls ${f})
    endif()
  endforeach()
  collect(types "}[ \t\r\n]*u2ps_[a-z0-9_]+_t[ \t\r\n]*;" "^}[ \t\r\n]*|[ \t\r\n]*;$" "${text}")
  string(REGEX MATCHALL "typedef[ \t\r\n]+enum[ \t\r\n]*{[^}]*}" enums "${text}")
  foreach(e IN LISTS enums)
    collect(enum_values "U2PS_[A-Z0-9_]+" "^$" "${e}")
  endforeach()
  set(defines "")
  collect(defines "#[ \t]*define[ \t]+U2PS_[A-Z0-9_]+" "^#[ \t]*define[ \t]+" "${text}")
  foreach(d IN LISTS defines)
    if(NOT d MATCHES "_H$")
      list(APPEND macros ${d})
    endif()
  endforeach()
  collect(build_constants "defined[ \t]*\\([ \t]*U2PS_[A-Z0-9_]+" "^defined[ \t]*\\([ \t]*" "${text}")
endforeach()

# ---- The words of the guide ----
file(READ ${AGENTS} guide)
string(REGEX MATCHALL "[A-Za-z0-9_]+(\\.[A-Za-z0-9_]+)*" tokens "${guide}")
set(words "")
set(files "")
foreach(t IN LISTS tokens)
  string(REGEX REPLACE "^[DU](U2PS_)" "\\1" t "${t}")
  if(NOT t MATCHES "^(u2ps|U2PS)_")
    continue()
  endif()
  if(t MATCHES "\\.(h|c|cmake|txt|ld)$")
    list(APPEND files ${t})
    continue()
  endif()
  string(REGEX REPLACE "\\..*$" "" t "${t}")
  if(NOT t MATCHES "_$")
    list(APPEND words ${t})
  endif()
endforeach()
if(words)
  list(REMOVE_DUPLICATES words)
endif()
if(files)
  list(REMOVE_DUPLICATES files)
endif()

set(problems 0)
set(summary "")
foreach(c IN LISTS classes)
  if(${c})
    list(REMOVE_DUPLICATES ${c})
  endif()
  list(LENGTH ${c} n)
  string(APPEND summary " ${n} ${c}")
  if(n EQUAL 0)
    message("agents sync: no ${c} found in the headers: the check no longer reads them")
    math(EXPR problems "${problems} + 1")
  endif()
  foreach(s IN LISTS ${c})
    list(FIND words ${s} at)
    if(at EQUAL -1)
      message("missing from the guide: ${s} (${c})")
      math(EXPR problems "${problems} + 1")
    endif()
  endforeach()
endforeach()

# ---- 2. The guide's names against the tree ----
file(GLOB_RECURSE sources RELATIVE ${U2PS_ROOT}
  ${U2PS_ROOT}/common/* ${U2PS_ROOT}/device/* ${U2PS_ROOT}/host/* ${U2PS_ROOT}/port/*
  ${U2PS_ROOT}/testkit/* ${U2PS_ROOT}/examples/* ${U2PS_ROOT}/cmake/* ${U2PS_ROOT}/tests/*)
list(APPEND sources CMakeLists.txt)
set(known "")
set(names "")
foreach(f IN LISTS sources)
  get_filename_component(name ${f} NAME)
  list(APPEND names ${name})
  if(f MATCHES "^tests/")
    continue()
  endif()
  if(name MATCHES "\\.h$" AND NOT name MATCHES "_priv\\.h$")
    file(READ ${U2PS_ROOT}/${f} text)
    strip_comments(text "${text}")
  elseif(name STREQUAL "CMakeLists.txt" OR name MATCHES "\\.cmake$")
    file(READ ${U2PS_ROOT}/${f} text)
  else()
    continue()
  endif()
  string(REGEX MATCHALL "(u2ps|U2PS)_[A-Za-z0-9_]*" found "${text}")
  list(APPEND known ${found})
endforeach()
# This check's own parameter, as the command at the top names it
list(APPEND known U2PS_ROOT)
list(REMOVE_DUPLICATES known)
foreach(w IN LISTS words)
  list(FIND known ${w} at)
  if(at EQUAL -1)
    message("not in the tree: ${w} (no public header or CMake file has it)")
    math(EXPR problems "${problems} + 1")
  endif()
endforeach()
foreach(f IN LISTS files)
  list(FIND names ${f} at)
  if(at EQUAL -1)
    message("not in the tree: ${f} (no file of that name)")
    math(EXPR problems "${problems} + 1")
  endif()
endforeach()

list(LENGTH words nwords)
if(problems GREATER 0)
  message(FATAL_ERROR "agents sync: ${problems} problems; the headers have${summary}; "
                      "the guide names ${nwords} symbols")
endif()
message("agents sync: AGENTS.md is in sync with the app and test kit headers:${summary}; "
        "the guide names ${nwords} symbols")
