/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dport_cfg.h: the fake port's configuration header, with the
 * constants every port's header defines (u2ps_dport.h). The fake checks
 * stream_open against them, and the tests and the apps tested with the PC
 * test kit size class memory with them.
 */
#ifndef U2PS_DPORT_CFG_H
#define U2PS_DPORT_CFG_H

#include <stdint.h>

#define U2PS_DPORT_STRIDE(mps)  ((uint16_t)(((mps) + 3u) & ~3u))
#define U2PS_DPORT_ISO_OUT_MIN  2u
/* The fake keeps one buffer armed, as DWC2 does: the device stack holds at
 * most count - 1 (fake_dport.h) */
#define U2PS_DPORT_ISO_OUT_RESERVE 1u
#define U2PS_DPORT_ALIGN        4u
#define U2PS_DPORT_UFRAME_EXACT 1
#define U2PS_DPORT_EP_MAX       7u

#define U2PS_DPORT_VBUS_SENSE   0

#endif /* U2PS_DPORT_CFG_H */
