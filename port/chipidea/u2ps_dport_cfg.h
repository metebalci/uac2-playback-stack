/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dport_cfg.h: the ChipIdea device port's configuration header, with
 * the constants every port's header defines (u2ps_dport.h), for the
 * i.MX RT1062. RM is the i.MX RT1060 Processor Reference Manual,
 * IMXRT1060RM Rev. 3, 07/2021.
 *
 * Not usable yet: the ChipIdea device port has no register code
 * (u2ps_chipidea_dcd.c defines none of u2ps_dport.h's functions), so an
 * app for the i.MX RT1062 builds but does not link. Its decision logic is
 * tested on the PC against a simulated controller.
 */
#ifndef U2PS_DPORT_CFG_H
#define U2PS_DPORT_CFG_H

#include <stdint.h>

/* The unit of the dTD's Frame Number field (RM Table 42-64, p. 2398), which
 * the manual does not state: 0, the field is not used and holes come from
 * the fallback count; 1, microframes; 2, frames (U2PS_CHIPIDEA_FN_* in
 * u2ps_chipidea_iso.h). A board measurement decides it; until then the
 * port uses the fallback count. Set by the CMake option
 * U2PS_CHIPIDEA_FN_UNIT. */
#ifndef U2PS_CHIPIDEA_FN_UNIT
#define U2PS_CHIPIDEA_FN_UNIT 0
#endif

/* Alignment of class memory, in bytes. The controller states no buffer
 * alignment: a dTD's page 0 pointer carries a 12-bit byte offset (RM Table
 * 42-64, p. 2398). 32 is the Cortex-M7 data cache line (an Arm fact that
 * RM does not state), so class memory may also lie in cached OCRAM, where
 * the port invalidates whole lines (u2ps_dport.h, stream_open); 4 would
 * do in DTCM, which is not cached. */
#define U2PS_DPORT_ALIGN 32u

/* The least stride for buffers of mps bytes: each dTD receives at most
 * Total Bytes = mps (a longer packet is an overflow, RM Table 42-74,
 * p. 2422), rounded up to U2PS_DPORT_ALIGN */
#define U2PS_DPORT_STRIDE(mps) \
  ((uint16_t)(((mps) + U2PS_DPORT_ALIGN - 1u) & ~(U2PS_DPORT_ALIGN - 1u)))

/* The buffers of a stream the port keeps queued, so that the device stack
 * holds at most count - 2: an unprimed OUT endpoint ignores the host's
 * packets (RM 42.5.6.4.3, p. 2414; Table 42-72, p. 2416), and the list
 * must stay "out ahead of the device controller by at least two
 * (micro)frames" (RM p. 2415). A packet that would leave fewer queued is
 * counted NO_BUFFER and its buffer queued again (u2ps_chipidea_iso.h). */
#define U2PS_DPORT_ISO_OUT_RESERVE 2u

/* The least number of buffers of a stream: the reserve and one the device
 * stack may hold */
#define U2PS_DPORT_ISO_OUT_MIN 3u

/* on_packet's microframe numbers are exact only if the Frame Number counts
 * microframes */
#if U2PS_CHIPIDEA_FN_UNIT == 1
#define U2PS_DPORT_UFRAME_EXACT 1
#else
#define U2PS_DPORT_UFRAME_EXACT 0
#endif

/* One dTD carries an EP0 data stage; RM recommends at most 16 KB per dTD
 * (Table 42-63, p. 2397) */
#define U2PS_DPORT_EP0_MAX 16384u

/* Endpoints 0 to 7 (RM 42.7.4, HWDEVICE.DEVEP = 8, p. 2440) */
#define U2PS_DPORT_EP_MAX 7u

/* The feedback endpoint's bInterval: from 2, so that the second feedback
 * dTD keeps the list two microframes ahead while the port appends the
 * first again (RM p. 2415), to 16, the most USB 2.0 allows (9.6.6,
 * Table 9-13, p. 271) */
#define U2PS_DPORT_FB_INTERVAL_MIN 2u
#define U2PS_DPORT_FB_INTERVAL_MAX 16u

/* The port does not sense VBUS yet: VBUS modes app and bus only
 * (u2ps_dev.h) */
#define U2PS_DPORT_VBUS_SENSE 0

/* What the port interface leaves to each port (u2ps_dport.h):
 * - u2ps_dport_release() costs O(1) and writes no register: the released
 *   buffers are queued at the next retirement, while the reserve keeps
 *   the list from running dry (u2ps_chipidea_iso.h).
 * - fb_recoveries counts primes after the feedback list ran dry. The list
 *   runs dry only after polls retired both dTDs, so once the host stops
 *   polling at a stream's end no recovery can come: no data rule is
 *   needed (u2ps_chipidea_iso.h).
 * - fallback_over counts the microframes the fallback count reported too
 *   many and found later; errors counts real faults only.
 * - DMA memory: IMXRT1060RM Rev. 3 does not state which memories the USB
 *   controller's DMA reaches (it names the USB controllers as masters of
 *   the NIC-301 interconnect, chapter 29, and lists no targets), so the
 *   port has no reach rule: u2ps_dport_dma_check() is not defined, as no
 *   other port call is, and nothing checks DMA memory or class memory. */

#endif /* U2PS_DPORT_CFG_H */
