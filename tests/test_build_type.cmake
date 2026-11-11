# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
#
# test_build_type.cmake: u2ps sets an empty CMAKE_BUILD_TYPE to
# RelWithDebInfo only when it is the top-level project (CMakeLists.txt).
# Each case configures into WORK/<case> and reads CMAKE_BUILD_TYPE from the
# cache and as the integrator's project sees it after add_subdirectory():
# - u2ps as the top-level project, no build type: RelWithDebInfo;
# - u2ps added by a project, no build type: the cache and the project's
#   scope keep it empty, and the configure says that u2ps's libraries build
#   without optimisation;
# - u2ps added by a project with Debug: Debug stays, without that line;
# - u2ps added by a project that sets RelWithDebInfo in the cache before
#   project() (AGENTS.md 2.1): RelWithDebInfo.
# With ARM=ON the cases are ARM configures with the tree's toolchain files
# instead, each checked by the optimisation flags that compile_commands.json
# gives device/u2ps_dev.c (the last -O flag counts, as for both compilers):
# - ATfE, top-level, no build type: RelWithDebInfo, -Os -g -DNDEBUG;
# - GCC, top-level, no build type: RelWithDebInfo, -O2 -g -DNDEBUG;
# - a given build type is kept, with CMake's flags for it (ATfE Debug and
#   Release, GCC MinSizeRel), and ATfE's RelWithDebInfo flags given on the
#   command line win (-O2);
# - ATfE, top-level, no build type, U2PS_TEST_INJECT=ON: the DWC2 device
#   port builds, at -Os, with the hook;
# - ATfE, u2ps added by a project, no build type: stays empty, no -O flag.
# ATfE comes from ATFE_DIR or the environment's U2PS_ATFE_DIR, GCC from
# ARM_GCC or arm-none-eabi-gcc on the PATH; without either the ARM cases
# print "ARM cases skipped" and pass.
#
#   cmake -DU2PS_ROOT=<tree> -DWORK=<scratch directory> -DGENERATOR=<generator>
#         [-DMAKE_PROGRAM=<ninja>] [-DC_COMPILER=<cc>]
#         [-DARM=ON [-DATFE_DIR=<atfe-dir>] [-DARM_GCC=<arm-none-eabi-gcc>]]
#         -P test_build_type.cmake

if(NOT U2PS_ROOT OR NOT WORK OR NOT GENERATOR)
  message(FATAL_ERROR "test_build_type: set -DU2PS_ROOT=<tree>, -DWORK=<scratch directory> "
                      "and -DGENERATOR=<generator>")
endif()
set(failures 0)
set(tool_args -G ${GENERATOR} -DU2PS_DEVICE=ON -DU2PS_VBUS=app)
if(MAKE_PROGRAM)
  list(APPEND tool_args -DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM})
endif()
if(C_COMPILER)
  list(APPEND tool_args -DCMAKE_C_COMPILER=${C_COMPILER})
endif()

# The integrator's project: u2ps as a subdirectory, and the build type its
# own scope sees after it
set(parent ${WORK}/parent-src)
foreach(variant plain preset)
  set(preset "")
  if(variant STREQUAL "preset")
    set(preset "set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING \"Build type\")\n")
  endif()
  file(MAKE_DIRECTORY ${parent}-${variant})
  file(WRITE ${parent}-${variant}/CMakeLists.txt
    "cmake_minimum_required(VERSION 3.22)\n"
    "${preset}"
    "project(integrator C)\n"
    "add_subdirectory(\"${U2PS_ROOT}\" u2ps EXCLUDE_FROM_ALL)\n"
    "message(STATUS \"integrator build type: '\${CMAKE_BUILD_TYPE}'\")\n")
endforeach()
set(unoptimised "u2ps's libraries build without optimisation")

# Configures source 'src' as case 'name' with the options in ARGN, then
# checks that the cache holds 'want' and, for the integrator's project,
# that its scope sees 'want' too, and that the configure says the libraries
# are not optimised exactly when 'want' is empty
function(build_type name src want)
  set(dir ${WORK}/${name})
  file(REMOVE_RECURSE ${dir})
  execute_process(
    COMMAND ${CMAKE_COMMAND} -S ${src} -B ${dir} ${tool_args} ${ARGN}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL 0)
    message("FAIL ${name}: the configure stopped (exit ${rc}): ${out}")
    math(EXPR f "${failures} + 1")
    set(failures ${f} PARENT_SCOPE)
    return()
  endif()
  file(STRINGS ${dir}/CMakeCache.txt line REGEX "^CMAKE_BUILD_TYPE:")
  string(REGEX REPLACE "^CMAKE_BUILD_TYPE:[A-Z]*=" "" cached "${line}")
  set(ok TRUE)
  if(NOT cached STREQUAL want)
    message("FAIL ${name}: the cache holds CMAKE_BUILD_TYPE '${cached}', not '${want}'")
    set(ok FALSE)
  endif()
  string(FIND "${out}" "${unoptimised}" said)
  if(want STREQUAL "" AND said EQUAL -1)
    message("FAIL ${name}: the configure does not say that ${unoptimised}")
    set(ok FALSE)
  elseif(NOT want STREQUAL "" AND NOT said EQUAL -1)
    message("FAIL ${name}: the configure says that ${unoptimised}")
    set(ok FALSE)
  endif()
  if(NOT src STREQUAL U2PS_ROOT AND NOT out MATCHES "integrator build type: '${want}'")
    string(REGEX MATCH "integrator build type: '[^']*'" seen "${out}")
    message("FAIL ${name}: the integrator's project sees ${seen}, not '${want}'")
    set(ok FALSE)
  endif()
  if(ok)
    message("ok ${name}: CMAKE_BUILD_TYPE '${cached}'")
  else()
    math(EXPR f "${failures} + 1")
    set(failures ${f} PARENT_SCOPE)
  endif()
endfunction()

if(NOT ARM)
  build_type(top_level ${U2PS_ROOT} RelWithDebInfo)
  build_type(subproject ${parent}-plain "")
  build_type(subproject_debug ${parent}-plain Debug -DCMAKE_BUILD_TYPE=Debug)
  build_type(subproject_preset ${parent}-preset RelWithDebInfo)
else()
  if(NOT ATFE_DIR AND DEFINED ENV{U2PS_ATFE_DIR})
    set(ATFE_DIR "$ENV{U2PS_ATFE_DIR}")
  endif()
  if(NOT ARM_GCC)
    find_program(ARM_GCC arm-none-eabi-gcc)
  endif()
  if(NOT ATFE_DIR OR NOT EXISTS "${ATFE_DIR}/bin/clang" OR NOT ARM_GCC)
    message("test_build_type: ARM cases skipped: they need ATfE (ATFE_DIR or U2PS_ATFE_DIR, "
            "'${ATFE_DIR}') and arm-none-eabi-gcc ('${ARM_GCC}')")
    return()
  endif()
  get_filename_component(gcc_dir "${ARM_GCC}" DIRECTORY)
  set(arm_args -G ${GENERATOR} -DU2PS_CHIP=stm32u5a5 -DU2PS_DEVICE=ON -DU2PS_VBUS=app)
  if(MAKE_PROGRAM)
    list(APPEND arm_args -DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM})
  endif()
  set(atfe -DCMAKE_TOOLCHAIN_FILE=${U2PS_ROOT}/cmake/arm-none-eabi-clang.cmake -DU2PS_ATFE_DIR=${ATFE_DIR})
  set(gcc -DCMAKE_TOOLCHAIN_FILE=${U2PS_ROOT}/cmake/arm-none-eabi.cmake)
endif()

# The compile command of the source whose path ends in 'file', from the
# compile_commands.json of 'dir', in 'out' (empty if none)
function(compile_command dir file out)
  set(${out} "" PARENT_SCOPE)
  if(NOT EXISTS ${dir}/compile_commands.json)
    return()
  endif()
  file(READ ${dir}/compile_commands.json json)
  string(JSON n LENGTH "${json}")
  math(EXPR last "${n} - 1")
  foreach(i RANGE ${last})
    string(JSON f GET "${json}" ${i} file)
    if(f MATCHES "/${file}$")
      string(JSON c GET "${json}" ${i} command)
      set(${out} "${c}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
endfunction()

# An ARM case: configures source 'src' as case 'name' with the options in
# ARGN (the toolchain's among them), then checks the cached build type
# ('want'), the optimisation flag that counts for device/u2ps_dev.c, the
# last -O flag or "none" ('opt'), and that its command holds each flag of
# the list 'flags'. BUILD=<target> (in ARGN) also builds that target, and
# FILE=<path> checks that target's source instead of device/u2ps_dev.c.
function(arm_case name src want opt flags)
  set(dir ${WORK}/${name})
  file(REMOVE_RECURSE ${dir})
  set(args "")
  set(target "")
  set(source device/u2ps_dev.c)
  foreach(a ${ARGN})
    if(a MATCHES "^BUILD=(.*)")
      set(target ${CMAKE_MATCH_1})
    elseif(a MATCHES "^FILE=(.*)")
      set(source ${CMAKE_MATCH_1})
    else()
      list(APPEND args ${a})
    endif()
  endforeach()
  set(ok TRUE)
  # GCC from ARM_GCC's directory, first on the PATH
  set(ENV{PATH} "${gcc_dir}:$ENV{PATH}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -S ${src} -B ${dir} ${arm_args} -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ${args}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL 0)
    message("FAIL ${name}: the configure stopped (exit ${rc}): ${out}")
    set(ok FALSE)
  elseif(target)
    execute_process(
      COMMAND ${CMAKE_COMMAND} --build ${dir} --target ${target}
      RESULT_VARIABLE rc OUTPUT_VARIABLE bout ERROR_VARIABLE bout)
    if(NOT rc EQUAL 0)
      message("FAIL ${name}: the build of ${target} stopped (exit ${rc}): ${bout}")
      set(ok FALSE)
    endif()
  endif()
  if(ok)
    file(STRINGS ${dir}/CMakeCache.txt line REGEX "^CMAKE_BUILD_TYPE:")
    string(REGEX REPLACE "^CMAKE_BUILD_TYPE:[A-Z]*=" "" cached "${line}")
    if(NOT cached STREQUAL want)
      message("FAIL ${name}: the cache holds CMAKE_BUILD_TYPE '${cached}', not '${want}'")
      set(ok FALSE)
    endif()
    compile_command(${dir} ${source} cmd)
    if(cmd STREQUAL "")
      message("FAIL ${name}: compile_commands.json has no command for ${source}")
      set(ok FALSE)
    else()
      string(REGEX MATCHALL "(^| )-O[^ ]*" os " ${cmd}")
      set(got none)
      foreach(o ${os})
        string(STRIP "${o}" got)
      endforeach()
      if(NOT got STREQUAL opt)
        message("FAIL ${name}: ${source} compiles with ${got}, not ${opt}: ${cmd}")
        set(ok FALSE)
      endif()
      foreach(f ${flags})
        if(NOT " ${cmd} " MATCHES " ${f} ")
          message("FAIL ${name}: the command for ${source} lacks ${f}: ${cmd}")
          set(ok FALSE)
        endif()
      endforeach()
    endif()
  endif()
  if(ok)
    message("ok ${name}: CMAKE_BUILD_TYPE '${cached}', ${source} at ${got}")
  else()
    math(EXPR f "${failures} + 1")
    set(failures ${f} PARENT_SCOPE)
  endif()
endfunction()

if(ARM)
  arm_case(atfe_top_level ${U2PS_ROOT} RelWithDebInfo -Os "-g;-DNDEBUG" ${atfe})
  arm_case(gcc_top_level ${U2PS_ROOT} RelWithDebInfo -O2 "-g;-DNDEBUG" ${gcc})
  arm_case(atfe_debug ${U2PS_ROOT} Debug none "-g" ${atfe} -DCMAKE_BUILD_TYPE=Debug)
  arm_case(atfe_release ${U2PS_ROOT} Release -O3 "-DNDEBUG" ${atfe} -DCMAKE_BUILD_TYPE=Release)
  arm_case(gcc_minsizerel ${U2PS_ROOT} MinSizeRel -Os "-DNDEBUG" ${gcc} -DCMAKE_BUILD_TYPE=MinSizeRel)
  arm_case(atfe_o2 ${U2PS_ROOT} RelWithDebInfo -O2 "-g;-DNDEBUG" ${atfe}
           "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g -DNDEBUG")
  arm_case(atfe_inject ${U2PS_ROOT} RelWithDebInfo -Os "-g;-DNDEBUG;-DU2PS_TEST_INJECT=1" ${atfe}
           -DU2PS_TEST_INJECT=ON BUILD=u2ps_dwc2_dcd FILE=port/dwc2/dcd/u2ps_dwc2_dcd.c)
  arm_case(atfe_subproject ${parent}-plain "" none "" ${atfe})
endif()

if(failures GREATER 0)
  message(FATAL_ERROR "test_build_type: ${failures} failed")
endif()
message("test_build_type: all cases passed")
