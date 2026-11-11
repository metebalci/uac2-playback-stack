# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
# Shared by the device and host stacks: USB and UAC2 constants, microframe
# arithmetic (static inline in u2ps_uframe.h). No controller code, and no
# source file: an interface library of headers.

add_library(u2ps_common INTERFACE)
target_include_directories(u2ps_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
