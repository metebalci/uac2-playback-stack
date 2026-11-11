# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
#
# Toolchain: Arm Toolchain for Embedded (ATfE), Arm's LLVM toolchain:
# clang, the lld linker, llvm-ar and the LLVM binutils, with picolibc as
# its default C library. U2PS_ATFE_DIR names its installation directory,
# the one that holds bin/clang (<atfe-dir>, such as a directory
# ATfE-23.1.0-Linux-x86_64), on the command line
# (-DU2PS_ATFE_DIR=<atfe-dir>) or in the environment. U2PS_CHIP selects the
# target triple, CPU, FPU and float ABI, the same CPU flags as
# cmake/arm-none-eabi.cmake (GCC), from which the chip facts come:
# - stm32f723: Cortex-M7 (Armv7E-M) with a single-precision FPU.
# - stm32u5a5: Cortex-M33 (Armv8-M Mainline) with a single-precision FPU.
# - stm32h7s3: Cortex-M7 (Armv7E-M) with single- and double-precision FPU.
# - imxrt1062: Cortex-M7 (Armv7E-M) with single- and double-precision FPU.
# Clang picks its C library variant (multilib) from these flags. The
# hard-float ABI passes floating-point arguments in FPU registers.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT U2PS_ATFE_DIR AND DEFINED ENV{U2PS_ATFE_DIR})
  set(U2PS_ATFE_DIR "$ENV{U2PS_ATFE_DIR}")
endif()
if(NOT U2PS_ATFE_DIR OR NOT EXISTS "${U2PS_ATFE_DIR}/bin/clang")
  message(FATAL_ERROR "U2PS_ATFE_DIR must name the directory of Arm Toolchain for Embedded that "
                      "holds bin/clang, not '${U2PS_ATFE_DIR}': set -DU2PS_ATFE_DIR=<dir> or the "
                      "environment variable U2PS_ATFE_DIR, or use cmake/arm-none-eabi.cmake for "
                      "arm-none-eabi-gcc")
endif()

set(CMAKE_C_COMPILER "${U2PS_ATFE_DIR}/bin/clang")
set(CMAKE_ASM_COMPILER "${U2PS_ATFE_DIR}/bin/clang")
# The archivers of plain and of LTO builds, and the binutils
set(CMAKE_AR "${U2PS_ATFE_DIR}/bin/llvm-ar")
set(CMAKE_RANLIB "${U2PS_ATFE_DIR}/bin/llvm-ranlib")
set(CMAKE_C_COMPILER_AR "${U2PS_ATFE_DIR}/bin/llvm-ar")
set(CMAKE_C_COMPILER_RANLIB "${U2PS_ATFE_DIR}/bin/llvm-ranlib")
set(CMAKE_OBJCOPY "${U2PS_ATFE_DIR}/bin/llvm-objcopy")
set(CMAKE_SIZE "${U2PS_ATFE_DIR}/bin/llvm-size")

# No host can run what CMake's checks build
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# CMake's checks read this file again; pass the chip and the toolchain to them
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES U2PS_CHIP U2PS_ATFE_DIR)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

if(U2PS_CHIP STREQUAL "stm32f723")
  set(U2PS_TARGET thumbv7em-none-eabihf)
  set(U2PS_CPU_FLAGS "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard")
elseif(U2PS_CHIP STREQUAL "stm32u5a5")
  set(U2PS_TARGET thumbv8m.main-none-eabihf)
  set(U2PS_CPU_FLAGS "-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard")
elseif(U2PS_CHIP STREQUAL "stm32h7s3")
  set(U2PS_TARGET thumbv7em-none-eabihf)
  set(U2PS_CPU_FLAGS "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard")
elseif(U2PS_CHIP STREQUAL "imxrt1062")
  set(U2PS_TARGET thumbv7em-none-eabihf)
  set(U2PS_CPU_FLAGS "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard")
else()
  message(FATAL_ERROR "U2PS_CHIP must be stm32f723, stm32u5a5, stm32h7s3 or imxrt1062, not "
                      "'${U2PS_CHIP}': set -DU2PS_CHIP to one of them; the toolchain file is for "
                      "ARM builds only")
endif()

set(CMAKE_C_COMPILER_TARGET ${U2PS_TARGET})
set(CMAKE_ASM_COMPILER_TARGET ${U2PS_TARGET})
set(CMAKE_C_FLAGS_INIT "${U2PS_CPU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${U2PS_CPU_FLAGS}")
# lld, ATfE's linker (clang's default for these targets, named here so
# that no other linker is taken)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
# ATfE builds default to -Os: RelWithDebInfo, u2ps's default build type
# (CMakeLists.txt), is -Os -g -DNDEBUG here instead of CMake's -O2 -g
# -DNDEBUG, the injection build of a test included. A cache default, so
# -DCMAKE_C_FLAGS_RELWITHDEBINFO=... wins (tests/test_build_type.cmake)
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-Os -g -DNDEBUG" CACHE STRING
    "Flags used by the C compiler during RELWITHDEBINFO builds (ATfE: -Os)")
