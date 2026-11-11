# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
#
# The PC test kit's worked example: a minimal DAC app (dac_app.c) and a
# minimal player app (player_app.c), each tested on the PC against u2ps
# with the kit (testkit/u2ps_testkit.h). It is built the way an
# integration builds its own tests: as a project that adds u2ps with
# add_subdirectory() and links a kit library per test. From the root of
# the u2ps tree:
#
#   cmake -S examples/pc_testkit -B build/pc_testkit -G Ninja \
#     -DU2PS_DEVICE=ON -DU2PS_HOST=ON -DU2PS_VBUS=app -DU2PS_HOST_IA_VBUS_RATED_MA=500
#   cmake --build build/pc_testkit
#   ctest --test-dir build/pc_testkit --output-on-failure
#
# u2ps's own PC build adds this directory too, so its ctest runs these
# tests. Copy the directory into your project, set u2ps_tree below to your
# copy of u2ps, and replace the apps with yours.

cmake_minimum_required(VERSION 3.22)
project(u2ps_pc_testkit_example LANGUAGES C)

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_C_STANDARD 11)
  set(CMAKE_C_STANDARD_REQUIRED ON)
  # Where u2ps lies: here, two levels up
  set(u2ps_tree ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  add_subdirectory(${u2ps_tree} u2ps)
  enable_testing()
endif()

# One executable per test: the app's sources, the test, and the kit
# library of its side, which brings the stack, the fake port and the
# sanitizers. The test's exit code is its verdict.
function(example_test name kit)
  add_executable(${name} ${ARGN})
  target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(${name} PRIVATE ${kit})
  add_test(NAME pc_testkit_${name} COMMAND ${name})
endfunction()

example_test(test_dac u2ps_testkit_device test_dac.c dac_app.c)
example_test(test_player u2ps_testkit_host test_player.c player_app.c)
example_test(test_player_loopback u2ps_testkit_loopback test_player_loopback.c player_app.c)
