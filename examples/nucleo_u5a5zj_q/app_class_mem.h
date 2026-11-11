/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * app_class_mem.h: the class memory layout of the example app. Each buffer
 * of a stream holds a packet of up to max_packet_size bytes, then a canary
 * word that no packet may reach.
 */
#ifndef APP_CLASS_MEM_H
#define APP_CLASS_MEM_H

#include <stdint.h>

#include "u2ps_dport_cfg.h"

/* The canary word after each buffer's packet area */
#define APP_CANARY_BYTES 4u

/* The stride of a buffer for packets of up to mps bytes: the port's least
 * stride, U2PS_DPORT_STRIDE(mps), which holds the transfer the port
 * programs, then the canary word, rounded up to U2PS_DPORT_ALIGN, since
 * every stride must be a multiple of it (u2ps_stream_cfg_t, u2ps_dev.h):
 * the least stride plus 4 on the STM32U5 (ALIGN 4), plus 32 on the
 * STM32F723 and the i.MX RT1062 (ALIGN 32). The canary is the last word. */
#define APP_STRIDE(mps)                                                          \
  ((uint16_t)((U2PS_DPORT_STRIDE(mps) + APP_CANARY_BYTES + U2PS_DPORT_ALIGN - 1u) & \
              ~(U2PS_DPORT_ALIGN - 1u)))

#endif /* APP_CLASS_MEM_H */
