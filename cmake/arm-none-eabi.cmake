# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
#
# Toolchain: GNU Arm Embedded (arm-none-eabi-gcc) on the PATH. U2PS_CHIP
# selects the CPU, FPU and float ABI:
# - stm32f723: Cortex-M7 with a single-precision FPU (DS11853 Rev 10,
#   section 2 "Description").
# - stm32u5a5: Cortex-M33 with FPU (RM0456 Rev 7, section 2.1); the
#   Cortex-M33 FPU is single precision (Arm Cortex-M33 Processor Technical
#   Reference Manual).
# - stm32h7s3: Cortex-M7 with single- and double-precision FPU (DS14359
#   Rev 7, section 2 "Description", p. 15).
# - imxrt1062: Cortex-M7 with single- and double-precision FPU (IMXRT1060RM
#   Rev. 3, section 2.2, p. 31).
# The hard-float ABI passes floating-point arguments in FPU registers.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY arm-none-eabi-objcopy)
set(CMAKE_SIZE arm-none-eabi-size)

# No host can run what CMake's checks build
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# CMake's checks read this file again; pass the chip to them
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES U2PS_CHIP)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

if(U2PS_CHIP STREQUAL "stm32f723")
  set(U2PS_CPU_FLAGS "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard")
elseif(U2PS_CHIP STREQUAL "stm32u5a5")
  set(U2PS_CPU_FLAGS "-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard")
elseif(U2PS_CHIP STREQUAL "stm32h7s3")
  set(U2PS_CPU_FLAGS "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard")
elseif(U2PS_CHIP STREQUAL "imxrt1062")
  set(U2PS_CPU_FLAGS "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard")
else()
  message(FATAL_ERROR "U2PS_CHIP must be stm32f723, stm32u5a5, stm32h7s3 or imxrt1062, not "
                      "'${U2PS_CHIP}': set -DU2PS_CHIP to one of them; the toolchain file is for "
                      "ARM builds only")
endif()

set(CMAKE_C_FLAGS_INIT "${U2PS_CPU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${U2PS_CPU_FLAGS}")
