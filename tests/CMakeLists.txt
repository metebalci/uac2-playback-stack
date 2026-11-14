# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
# Unit tests, PC build only: one executable per test, run by CTest, for
# the stacks the build selects (U2PS_DEVICE, U2PS_HOST). The
# build has ASan and UBSan with recovery off, so any sanitizer report ends
# the test with a failing exit code; the output is also matched for one.
# The stacks' libraries have the contract checks on (U2PS_CONTRACT_CHECKS),
# and a contract violation that a test does not expect fails it too: the
# tests' hook prints it (contract_log.c).

function(u2ps_test name)
  cmake_parse_arguments(T "" "SOURCE" "LIBS;EXTRA" ${ARGN})
  if(NOT T_SOURCE)
    set(T_SOURCE ${name}.c)
  endif()
  add_executable(${name} ${T_SOURCE} ${T_EXTRA})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${kit})
  target_compile_definitions(${name} PRIVATE
    U2PS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  target_link_libraries(${name} PRIVATE ${T_LIBS})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
    FAIL_REGULAR_EXPRESSION "runtime error:;AddressSanitizer;LeakSanitizer;unexpected u2ps contract violation")
endfunction()

# The fake ports and the host's device model are the PC test kit's
# (testkit/); each test compiles them against the library it links
set(kit ${PROJECT_SOURCE_DIR}/testkit)

u2ps_test(test_uframe LIBS u2ps_common)
# The DWC2 ports' DMA memory rule (STM32U5, STM32F723, STM32H7S3), without
# registers
u2ps_test(test_dwc2_dma LIBS u2ps_dwc2_common)
# The DWC2 chip facts (u2ps_dwc2_chip.h) and the pure parts of the start-up,
# once per chip; the port's configuration headers, not the test kit's
foreach(variant u5a5 f723 h7s3)
  string(TOUPPER ${variant} chip)
  u2ps_test(test_dwc2_chip_${variant} SOURCE test_dwc2_chip.c LIBS u2ps_dwc2_common)
  target_include_directories(test_dwc2_chip_${variant} BEFORE PRIVATE ../port/dwc2)
  target_compile_definitions(test_dwc2_chip_${variant} PRIVATE U2PS_CHIP_STM32${chip}=1
                             TEST_${chip}=1)
endforeach()
# An unknown chip macro, and two chip macros: the build must stop at the
# chip header's #error (test_build_fails.cmake checks the failure and its
# message)
foreach(variant unknown two)
  add_executable(test_dwc2_chip_${variant} EXCLUDE_FROM_ALL test_dwc2_chip.c)
  target_include_directories(test_dwc2_chip_${variant} BEFORE PRIVATE ../port/dwc2
                             ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(test_dwc2_chip_${variant} PRIVATE u2ps_dwc2_common)
endforeach()
target_compile_definitions(test_dwc2_chip_unknown PRIVATE U2PS_CHIP_STM32F999=1 TEST_UNKNOWN=1)
target_compile_definitions(test_dwc2_chip_two PRIVATE U2PS_CHIP_STM32U5A5=1
                           U2PS_CHIP_STM32H7S3=1 TEST_TWO=1)
add_test(NAME test_dwc2_chip_unknown
  COMMAND ${CMAKE_COMMAND} -DBUILD=${CMAKE_BINARY_DIR} -DTARGET=test_dwc2_chip_unknown
          "-DEXPECT=u2ps_dwc2_chip.h: none of U2PS_CHIP_STM32U5A5"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/test_build_fails.cmake)
add_test(NAME test_dwc2_chip_two
  COMMAND ${CMAKE_COMMAND} -DBUILD=${CMAKE_BINARY_DIR} -DTARGET=test_dwc2_chip_two
          "-DEXPECT=u2ps_dwc2_chip.h: more than one of"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/test_build_fails.cmake)
# The start-up's register code (port/dwc2/common/u2ps_dwc2_phy.c) against a
# register model (fake_regs.h), once per chip; the STM32H7S3 at the
# NUCLEO-H7S3L8's 24 MHz HSE and at 16 MHz
foreach(variant h7s3 h7s3_16 f723 u5a5)
  string(TOUPPER ${variant} test)
  string(REGEX REPLACE "_16$" "" chip ${test})
  u2ps_test(test_dwc2_phy_${variant} SOURCE test_dwc2_phy.c
            EXTRA ../port/dwc2/common/u2ps_dwc2_phy.c LIBS u2ps_dwc2_common)
  target_compile_options(test_dwc2_phy_${variant} PRIVATE
                         -include ${CMAKE_CURRENT_SOURCE_DIR}/fake_regs.h)
  target_compile_definitions(test_dwc2_phy_${variant} PRIVATE U2PS_CHIP_STM32${chip}=1
                             TEST_${test}=1)
endforeach()
target_compile_definitions(test_dwc2_phy_h7s3 PRIVATE U2PS_DWC2_HSE_HZ=24000000
                           U2PS_DWC2_CORE_HZ=600000000u)
target_compile_definitions(test_dwc2_phy_h7s3_16 PRIVATE U2PS_DWC2_HSE_HZ=16000000
                           U2PS_DWC2_CORE_HZ=400000000u)
target_compile_definitions(test_dwc2_phy_f723 PRIVATE U2PS_DWC2_HSE_HZ=25000000
                           U2PS_DWC2_CORE_HZ=216000000u)
target_compile_definitions(test_dwc2_phy_u5a5 PRIVATE U2PS_DWC2_HSE_HZ=16000000
                           U2PS_DWC2_CORE_HZ=160000000u)

if(U2PS_DEVICE)
# Device stack: each test links the fake port and the shared support, which
# supply the port interface and the app callbacks. The tests link libraries
# of fixed options (device/CMakeLists.txt), whatever the build's options;
# all but test_dev_vbus_bus run in VBUS mode app, with VBUS present
# throughout unless the test removes it. Chapter 9:
set(dev_support ${kit}/fake_dport.c dev_support.c fixture.c contract_log.c)
u2ps_test(test_dev_enum LIBS u2ps_device_app EXTRA ${dev_support})
u2ps_test(test_dev_ctrl LIBS u2ps_device_app EXTRA ${dev_support})
u2ps_test(test_dev_std LIBS u2ps_device_app EXTRA ${dev_support})
u2ps_test(test_dev_speed LIBS u2ps_device_app EXTRA ${dev_support})
# Start-up, re-init, attach and detach, configuration, suspend
u2ps_test(test_dev_session LIBS u2ps_device_app EXTRA ${dev_support})
# VBUS, once per VBUS mode
u2ps_test(test_dev_vbus_app SOURCE test_dev_vbus.c LIBS u2ps_device_app EXTRA ${dev_support})
u2ps_test(test_dev_vbus_bus SOURCE test_dev_vbus.c LIBS u2ps_device_bus EXTRA ${dev_support})
# The audio function: descriptor builder, clock requests, stream
u2ps_test(test_dev_desc LIBS u2ps_device_app EXTRA ${dev_support})
u2ps_test(test_dev_clock LIBS u2ps_device_app EXTRA ${dev_support})
u2ps_test(test_dev_stream LIBS u2ps_device_app EXTRA ${dev_support})
# The stream start: SET_INTERFACE's held status, deferred starts, the bound
u2ps_test(test_dev_start LIBS u2ps_device_app EXTRA ${dev_support})
# The stream limits against a library built with small ones
u2ps_test(test_dev_limits LIBS u2ps_device_limits EXTRA ${dev_support})
# The same speed test against the library that refuses full speed
u2ps_test(test_dev_speed_refuse_fs SOURCE test_dev_speed.c
  LIBS u2ps_device_refuse_fs EXTRA ${dev_support})
# The control hook, against the library built with it
u2ps_test(test_dev_hook LIBS u2ps_device_hook EXTRA ${dev_support})
# The contract checks: each violation the device stack reports
u2ps_test(test_dev_contract LIBS u2ps_device_app EXTRA ${dev_support})
# DMA memory at init: the port's check first, the control buffer zeroed
u2ps_test(test_dev_dma LIBS u2ps_device_app EXTRA ${dev_support})
# The stream once more against the library without the checks
u2ps_test(test_dev_stream_unchecked SOURCE test_dev_stream.c LIBS u2ps_device_unchecked
  EXTRA ${dev_support})
# The control pipe once more against the library without the checks: a
# callback's unlisted result is a request error there too
u2ps_test(test_dev_ctrl_unchecked SOURCE test_dev_ctrl.c LIBS u2ps_device_unchecked
  EXTRA ${dev_support})
endif()

if(U2PS_HOST)
# Host stack: each test links the fake host port and the shared support,
# which supply the port interface, the app callbacks and a device model.
# The tests link libraries of fixed options (host/CMakeLists.txt). The
# walk, against the default library and the one that accepts a function
# without an IAD:
set(host_support ${kit}/fake_hport.c ${kit}/hmodel.c host_support.c fixture.c contract_log.c)
u2ps_test(test_host_walk LIBS u2ps_host_plain EXTRA ${host_support})
u2ps_test(test_host_walk_no_iad SOURCE test_host_walk.c LIBS u2ps_host_no_iad EXTRA ${host_support})
# Skipped (77) with fewer than four alternate settings kept
set_tests_properties(test_host_walk test_host_walk_no_iad PROPERTIES SKIP_RETURN_CODE 77)
# Enumeration against the device model, and with strings off
u2ps_test(test_host_enum LIBS u2ps_host_plain EXTRA ${host_support})
u2ps_test(test_host_strings_off LIBS u2ps_host_no_strings EXTRA ${host_support})
# The stream: its start and stop, feedback, holes, the cap, queue errors
u2ps_test(test_host_stream LIBS u2ps_host_plain EXTRA ${host_support})
# The contract checks: each violation the host stack reports
u2ps_test(test_host_contract LIBS u2ps_host_plain EXTRA ${host_support})
# DMA memory at init: the port's checks first, the memory zeroed
u2ps_test(test_host_dma LIBS u2ps_host_plain EXTRA ${host_support})
# The stream once more against the library without the checks
u2ps_test(test_host_stream_unchecked SOURCE test_host_stream.c LIBS u2ps_host_unchecked
  EXTRA ${host_support})
# Compliance: the test device, against the library with a
# 100 mA budget; suspend and resume, in the DWC2 and the ChipIdea model of
# the port; hub, power and the time to an end, at 100 mA
u2ps_test(test_host_testdev LIBS u2ps_host_ma100 EXTRA ${host_support})
u2ps_test(test_host_suspend LIBS u2ps_host_plain EXTRA ${host_support})
# Each wait of the enumeration and the resume is a lower bound, with the
# millisecond counter read at any phase
u2ps_test(test_host_waits LIBS u2ps_host_plain EXTRA ${host_support})
u2ps_test(test_host_messages LIBS u2ps_host_ma100 EXTRA ${host_support})
# Full and low speed: identified (USB 2.0 7.1.2.3), and the same test
# against the library that refuses them (U2PS_HOST_REFUSE_FS)
u2ps_test(test_host_speed LIBS u2ps_host_plain EXTRA ${host_support})
u2ps_test(test_host_speed_refuse_fs SOURCE test_host_speed.c LIBS u2ps_host_refuse_fs
  EXTRA ${host_support})
# The power budget's range: a static assert keeps it within 8..5000 mA
add_test(NAME test_host_budget
  COMMAND ${CMAKE_COMMAND} -DU2PS_ROOT=${PROJECT_SOURCE_DIR} -DC_COMPILER=${CMAKE_C_COMPILER}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/test_host_budget.cmake)
# The control buffer's range: a static assert keeps it a multiple of 64,
# 256..65472
add_test(NAME test_host_ep0_buf
  COMMAND ${CMAKE_COMMAND} -DU2PS_ROOT=${PROJECT_SOURCE_DIR} -DC_COMPILER=${CMAKE_C_COMPILER}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/test_host_ep0_buf.cmake)
endif()

if(U2PS_DEVICE AND U2PS_HOST)
# The host stack enumerates the device stack: every description the
# device tests accept comes back
u2ps_test(test_host_roundtrip LIBS u2ps_host_plain u2ps_device_app
  EXTRA ${kit}/fake_hport.c ${kit}/hmodel.c host_support.c ${kit}/fake_dport.c dev_support.c
        fixture.c contract_log.c)
# The host stack streams to the device stack through a bus that
# steps microframes
u2ps_test(test_host_loopback LIBS u2ps_host_plain u2ps_device_app
  EXTRA ${kit}/fake_hport.c ${kit}/hmodel.c host_support.c ${kit}/fake_dport.c dev_support.c
        fixture.c contract_log.c)
endif()

if(U2PS_DEVICE)
# The DWC2 device port's decision logic and FIFO sizes, without registers
u2ps_test(test_dwc2_logic LIBS u2ps_dwc2_dcd)
# The same logic built with the re-arm rule before B-next, which injection
# builds may select for a comparison
add_library(u2ps_dwc2_iso_old_rule STATIC ../port/dwc2/dcd/u2ps_dwc2_iso.c)
target_include_directories(u2ps_dwc2_iso_old_rule PUBLIC ../port/dwc2/dcd)
target_compile_definitions(u2ps_dwc2_iso_old_rule PRIVATE U2PS_DWC2_OLD_RULE=1)
target_link_libraries(u2ps_dwc2_iso_old_rule PUBLIC u2ps_common)
u2ps_test(test_dwc2_old_rule LIBS u2ps_dwc2_iso_old_rule)
# The DWC2 device port's interrupt handler (u2ps_dwc2_dcd.c, an ARM source)
# against a register model (fake_regs.h), built for the STM32U5A5: the
# order of the data endpoint's XFRC, INCOMPISOOUT and EOPF
u2ps_test(test_dwc2_dcd EXTRA ../port/dwc2/dcd/u2ps_dwc2_dcd.c ../port/dwc2/dcd/u2ps_dwc2_iso.c
  LIBS u2ps_common)
target_include_directories(test_dwc2_dcd BEFORE PRIVATE ../port/dwc2 ../port/dwc2/common
                           ../port/dwc2/dcd ../device)
target_compile_options(test_dwc2_dcd PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/fake_regs.h)
target_compile_definitions(test_dwc2_dcd PRIVATE U2PS_DWC2_PC_TEST=1 U2PS_CHIP_STM32U5A5=1
                           U2PS_DWC2_HSE_HZ=16000000
                           U2PS_DWC2_CORE_HZ=160000000u U2PS_ISO_OUT_MPS_MAX=1024u
                           U2PS_ISO_OUT_COUNT_MAX=16u)
endif()

if(U2PS_HOST)
# The DWC2 host port's decision logic and FIFO sizes, without registers
u2ps_test(test_dwc2_host_logic LIBS u2ps_dwc2_hcd)
endif()

if(U2PS_DEVICE)
# The ChipIdea device port's decision logic, against a simulated controller
u2ps_test(test_chipidea_logic LIBS u2ps_chipidea_dcd)

# The example app's class memory stride against each port configuration
# header: the STM32U5, the STM32F723 and the STM32H7S3 (DWC2, with the
# port's check), the i.MX RT1062 (ChipIdea)
foreach(variant u5a5 f723 h7s3 rt1062)
  if(variant STREQUAL "rt1062")
    u2ps_test(test_example_stride_${variant} SOURCE test_example_stride.c)
    target_include_directories(test_example_stride_${variant} BEFORE PRIVATE ../port/chipidea)
  else()
    string(TOUPPER ${variant} chip)
    u2ps_test(test_example_stride_${variant} SOURCE test_example_stride.c LIBS u2ps_dwc2_dcd)
    target_include_directories(test_example_stride_${variant} BEFORE PRIVATE ../port/dwc2)
    target_compile_definitions(test_example_stride_${variant} PRIVATE TEST_DWC2=1)
  endif()
  target_include_directories(test_example_stride_${variant} BEFORE PRIVATE ../examples/nucleo_u5a5zj_q)
endforeach()
target_compile_definitions(test_example_stride_u5a5 PRIVATE U2PS_CHIP_STM32U5A5=1 TEST_ALIGN=4)
target_compile_definitions(test_example_stride_f723 PRIVATE U2PS_CHIP_STM32F723=1 TEST_ALIGN=32)
target_compile_definitions(test_example_stride_h7s3 PRIVATE U2PS_CHIP_STM32H7S3=1 TEST_ALIGN=32)
target_compile_definitions(test_example_stride_rt1062 PRIVATE TEST_ALIGN=32)

# The example app's stall mode (U2PS_EXAMPLE_STALL) with the STM32U5's
# reserve (DWC2)
u2ps_test(test_example_stall LIBS u2ps_dwc2_dcd)
target_include_directories(test_example_stall BEFORE PRIVATE ../port/dwc2
                           ../examples/nucleo_u5a5zj_q)
target_compile_definitions(test_example_stall PRIVATE U2PS_CHIP_STM32U5A5=1)
endif()

# AGENTS.md against the app headers, both ways (check_agents_sync.cmake);
# skipped while the tree has no AGENTS.md. The check itself against
# scratch guides (test_agents_sync.cmake).
add_test(NAME agents_sync
  COMMAND ${CMAKE_COMMAND} -DU2PS_ROOT=${PROJECT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_agents_sync.cmake)
set_tests_properties(agents_sync PROPERTIES SKIP_REGULAR_EXPRESSION "AGENTS.md is absent")
add_test(NAME test_agents_sync
  COMMAND ${CMAKE_COMMAND} -DU2PS_ROOT=${PROJECT_SOURCE_DIR}
          -DWORK=${CMAKE_CURRENT_BINARY_DIR}/agents_sync
          -P ${CMAKE_CURRENT_SOURCE_DIR}/test_agents_sync.cmake)
# Every path:line citation of AGENTS.md names a file of the tree and lies
# within it (check_agents_citations.cmake); skipped while the tree has no
# AGENTS.md. The check itself against scratch guides
# (test_agents_citations.cmake).
add_test(NAME agents_citations
  COMMAND ${CMAKE_COMMAND} -DU2PS_ROOT=${PROJECT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_agents_citations.cmake)
set_tests_properties(agents_citations PROPERTIES SKIP_REGULAR_EXPRESSION "AGENTS.md is absent")
add_test(NAME test_agents_citations
  COMMAND ${CMAKE_COMMAND} -DU2PS_ROOT=${PROJECT_SOURCE_DIR}
          -DWORK=${CMAKE_CURRENT_BINARY_DIR}/agents_citations
          -P ${CMAKE_CURRENT_SOURCE_DIR}/test_agents_citations.cmake)

# The stack selection of a configure (U2PS_DEVICE, U2PS_HOST): the
# configures that must stop and those that must pass
add_test(NAME test_stack_selection
  COMMAND ${CMAKE_COMMAND} -DU2PS_ROOT=${PROJECT_SOURCE_DIR}
          -DWORK=${CMAKE_CURRENT_BINARY_DIR}/stack_selection
          -DGENERATOR=${CMAKE_GENERATOR} -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
          -DC_COMPILER=${CMAKE_C_COMPILER}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/test_stack_selection.cmake)

# The build type u2ps sets: only as the top-level project, never in an
# integrator's project (test_build_type.cmake)
add_test(NAME test_build_type
  COMMAND ${CMAKE_COMMAND} -DU2PS_ROOT=${PROJECT_SOURCE_DIR}
          -DWORK=${CMAKE_CURRENT_BINARY_DIR}/build_type
          -DGENERATOR=${CMAKE_GENERATOR} -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
          -DC_COMPILER=${CMAKE_C_COMPILER}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/test_build_type.cmake)
# The default optimisation of each ARM toolchain file and the build types
# kept (test_build_type.cmake, ARM=ON): needs ATfE (U2PS_ATFE_DIR, here or
# in the environment) and arm-none-eabi-gcc, and is skipped without them
set(u2ps_test_atfe_dir "${U2PS_ATFE_DIR}")
if(NOT u2ps_test_atfe_dir AND DEFINED ENV{U2PS_ATFE_DIR})
  set(u2ps_test_atfe_dir "$ENV{U2PS_ATFE_DIR}")
endif()
add_test(NAME test_build_type_arm
  COMMAND ${CMAKE_COMMAND} -DU2PS_ROOT=${PROJECT_SOURCE_DIR}
          -DWORK=${CMAKE_CURRENT_BINARY_DIR}/build_type_arm
          -DGENERATOR=${CMAKE_GENERATOR} -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
          -DARM=ON -DATFE_DIR=${u2ps_test_atfe_dir}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/test_build_type.cmake)
set_tests_properties(test_build_type_arm PROPERTIES SKIP_REGULAR_EXPRESSION "ARM cases skipped")
# The self-test of the capture tool in tools/cyncap/, no hardware needed;
# it needs Python 3 and is skipped without it, so that the checks of the
# C code need nothing but a C compiler
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME cyncap_selftest
    COMMAND ${Python3_EXECUTABLE} -I -B ${PROJECT_SOURCE_DIR}/tools/cyncap/cyncap_test.py)
else()
  add_test(NAME cyncap_selftest
    COMMAND ${CMAKE_COMMAND} -E echo "cyncap_selftest: Python 3 not found, skipped")
  set_tests_properties(cyncap_selftest PROPERTIES SKIP_REGULAR_EXPRESSION "Python 3 not found, skipped")
endif()

# The PC test kit (testkit/), through its public API, each side with an
# app of its own: the device side, the host side, the loopback
if(U2PS_DEVICE)
  u2ps_test(test_testkit_dev LIBS u2ps_testkit_device)
endif()
if(U2PS_HOST)
  u2ps_test(test_testkit_host LIBS u2ps_testkit_host)
endif()
if(U2PS_DEVICE AND U2PS_HOST)
  u2ps_test(test_testkit_loopback LIBS u2ps_testkit_loopback)
endif()

# The PC tests of each board example that has them: its
# examples/<board>/tests.cmake, read here, registers them with u2ps_test()
file(GLOB example_tests LIST_DIRECTORIES false ${PROJECT_SOURCE_DIR}/examples/*/tests.cmake)
list(SORT example_tests)
foreach(f ${example_tests})
  include(${f})
endforeach()
