# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
# The PC test kit (u2ps_testkit.h), PC build only: an integrator links one
# of its libraries instead of the stack's to test its own app on the PC.
#   u2ps_testkit_device    (U2PS_DEVICE=ON)  a DAC app; the kit plays the
#                          device port and the host
#   u2ps_testkit_host      (U2PS_HOST=ON)    a player app; the kit plays the
#                          host port and a scripted device
#   u2ps_testkit_loopback  (both)            a player app; the kit plays the
#                          bus to the u2ps device stack and a DAC app
# Each holds its stack built with the configure's options and the contract
# checks on (u2ps_testkit_device_stack, u2ps_testkit_host_stack, object
# libraries of device/ and host/), the fake port, and the kit's code. The
# stack's calls of the app's callbacks are renamed, in the kit's copy of
# the stack only, to the kit's functions of the same arguments, which
# record each in the trace and then call the app's callback.
#
# The fake ports and the host's device model are also the project's own
# test doubles (tests/CMakeLists.txt), and this directory holds the fake
# ports' configuration headers, which the PC host library takes too.

# Sanitizers for the integrator's test executable too: compile and link
set(u2ps_testkit_sanitizers -fsanitize=address,undefined -fno-sanitize-recover=all
    -fno-omit-frame-pointer)

# The trace, the checks and the record of violations, for either side
add_library(u2ps_testkit_common STATIC u2ps_testkit.c)
target_include_directories(u2ps_testkit_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(u2ps_testkit_common PUBLIC u2ps_common)
target_compile_options(u2ps_testkit_common INTERFACE ${u2ps_testkit_sanitizers})
target_link_options(u2ps_testkit_common INTERFACE ${u2ps_testkit_sanitizers})

# The definitions that rename, in the kit's stack, each callback to the
# kit's function u2ps_testkit_traced_<callback without u2ps_app_>, and the
# contract hook, which both stacks call, to
# u2ps_testkit_traced_<side>_on_contract
function(u2ps_testkit_trace stack side)
  set(defs u2ps_app_on_contract=u2ps_testkit_traced_${side}_on_contract)
  foreach(cb IN LISTS ARGN)
    string(REGEX REPLACE "^u2ps_app_" "" short ${cb})
    list(APPEND defs ${cb}=u2ps_testkit_traced_${short})
  endforeach()
  target_compile_definitions(${stack} PRIVATE ${defs})
endfunction()

if(U2PS_DEVICE)
  u2ps_testkit_trace(u2ps_testkit_device_stack dev
    u2ps_app_on_request u2ps_app_on_sampling_frequency u2ps_app_on_configured
    u2ps_app_on_stream_start u2ps_app_on_packet u2ps_app_on_hole u2ps_app_on_pause
    u2ps_app_on_stream_stop u2ps_app_on_fault u2ps_app_on_reset u2ps_app_on_suspend
    u2ps_app_on_resume u2ps_app_on_vbus u2ps_app_on_control)
  add_library(u2ps_testkit_device STATIC u2ps_testkit_dev.c fake_dport.c)
  target_include_directories(u2ps_testkit_device PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(u2ps_testkit_device PUBLIC u2ps_testkit_device_stack u2ps_testkit_common)
endif()

if(U2PS_HOST)
  u2ps_testkit_trace(u2ps_testkit_host_stack host
    u2ps_app_host_on_device u2ps_app_host_on_end u2ps_app_host_on_stream_start
    u2ps_app_host_on_sent u2ps_app_host_on_hole u2ps_app_host_on_stream_stop
    u2ps_app_host_on_test_device u2ps_app_host_on_resumed)
  add_library(u2ps_testkit_host STATIC u2ps_testkit_host.c fake_hport.c hmodel.c)
  target_include_directories(u2ps_testkit_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(u2ps_testkit_host PUBLIC u2ps_testkit_host_stack u2ps_testkit_common)
endif()

if(U2PS_DEVICE AND U2PS_HOST)
  add_library(u2ps_testkit_loopback STATIC u2ps_testkit_loopback.c)
  target_link_libraries(u2ps_testkit_loopback PUBLIC u2ps_testkit_device u2ps_testkit_host)
endif()
