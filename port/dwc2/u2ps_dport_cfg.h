/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dport_cfg.h: the DWC2 device port's configuration header, with the
 * constants every port's header defines (u2ps_dport.h), for the
 * STM32F723, the STM32U5 and the STM32H7S3 (whose device port build stops
 * for now: u2ps_dwc2_chip.h). The ARM build defines the chip's macro,
 * U2PS_CHIP_STM32F723, U2PS_CHIP_STM32U5A5 or U2PS_CHIP_STM32H7S3.
 */
#ifndef U2PS_DPORT_CFG_H
#define U2PS_DPORT_CFG_H

#include <stdint.h>

/* Alignment of class memory, in bytes. Neither manual states a DOEPDMAx
 * alignment; the DMA makes 32-bit accesses only (OTG_GAHBCFG.HBSTLEN,
 * RM0456 73.14.3, p. 3323), so 4 suffices (inferred).
 * - STM32F723: 32, the Cortex-M7 data cache line (an Arm fact that RM0431
 *   does not state), so class memory may also lie in cached SRAM1 or
 *   SRAM2, where the port cleans and invalidates whole lines
 *   (u2ps_dport.h, stream_open); 4 would do in DTCM, the default DMA
 *   memory, which is not cached.
 * - STM32U5: 4; no data cache lies on the SRAM path (RM0456 9.3, p. 383).
 * - STM32H7S3: 32, the Cortex-M7 data cache line, as the F723: the RAM the
 *   OTG_HS DMA reaches is cached by default (RM0477 Table 6). */
#if defined(U2PS_CHIP_STM32F723)
#define U2PS_DPORT_ALIGN 32u
#elif defined(U2PS_CHIP_STM32U5A5)
#define U2PS_DPORT_ALIGN 4u
#elif defined(U2PS_CHIP_STM32H7S3)
#define U2PS_DPORT_ALIGN 32u
#else
#error "u2ps_dport_cfg.h (DWC2): none of U2PS_CHIP_STM32F723, U2PS_CHIP_STM32U5A5 and U2PS_CHIP_STM32H7S3 is defined: link u2ps_device or u2ps_dwc2_dcd from an ARM build with -DU2PS_CHIP=stm32f723, stm32u5a5 or stm32h7s3, which defines it"
#endif

/* The least stride for buffers of mps bytes: the transfer size the port
 * programs, mps rounded up to a whole word (u2ps_dwc2_out_xfrsiz), rounded
 * up to U2PS_DPORT_ALIGN */
#define U2PS_DPORT_STRIDE(mps) \
  ((uint16_t)(((((mps) + 3u) & ~3u) + U2PS_DPORT_ALIGN - 1u) & ~(U2PS_DPORT_ALIGN - 1u)))

/* The buffers of a stream the port keeps armed, so that the device stack
 * holds at most count - 1: after a completion the core ignores
 * isochronous OUT packets until a buffer is armed again (RM0456 73.15.6,
 * p. 3444; RM0431 32.16.6). A packet that would take the last free buffer
 * is counted NO_BUFFER and its buffer armed again (u2ps_dwc2_iso.h). */
#define U2PS_DPORT_ISO_OUT_RESERVE 1u

/* The least number of buffers of a stream: the reserve and one the device
 * stack may hold */
#define U2PS_DPORT_ISO_OUT_MIN 2u

/* on_packet's microframe numbers are exact: the packet's microframe P comes
 * from EONUM and FNSOF read at its completion. Exceptions, each rare: a
 * completion handled 2k or 2k + 1 microframes late (k of 1 or more) gives
 * P + 2k, two or more microframes off, and places the hole one packet
 * early, as in 0.1.0; and in a gap, a parity that the follow writes while
 * the host's packet is on the wire (NAKSTS reads 0 then; whether a packet
 * is on the wire at the EOPF point is not measured), or in a pass served
 * after the next SOF that the port does not find as such (a known limit,
 * README), stamps that packet a microframe off (u2ps_dwc2_iso.h,
 * u2ps_dwc2_out_incomplete). */
#define U2PS_DPORT_UFRAME_EXACT 1

/* EP0 data stages have no limit below a 16-bit length: the port splits each
 * one into U2PS_EP0_MPS packets */
#define U2PS_DPORT_EP0_MAX 0xFFFFu

/* Nine bidirectional endpoints including EP0 (RM0431 32.3, Table 225;
 * RM0456 73.3, Table 762, p. 3292; RM0477 62.3, Table 668, p. 3049) */
#define U2PS_DPORT_EP_MAX 8u

/* The feedback endpoint's bInterval: at least 2, since the core sends in
 * every microframe of the armed parity and has no interval setting
 * (RM0431 32.15.51; RM0456 73.14.48, p. 3378); at most 13, the reach of the
 * port's microframe arithmetic (u2ps_dwc2_iso.h) */
#define U2PS_DPORT_FB_INTERVAL_MIN 2u
#define U2PS_DPORT_FB_INTERVAL_MAX 13u

/* The port does not sense VBUS yet: VBUS modes app and bus only
 * (u2ps_dev.h) */
#define U2PS_DPORT_VBUS_SENSE 0

/* u2ps_dport_release() costs O(1) and writes no register: one buffer is
 * armed at a time, always (U2PS_DPORT_ISO_OUT_RESERVE). The
 * reach rule of u2ps_dport_dma_check() and of class memory at a stream's
 * open is u2ps_dwc2_dma_ok() (u2ps_dwc2.h): on the STM32U5 SRAM1, SRAM2,
 * SRAM3, SRAM5 and SRAM6 from 0x2000_0000, each clocked and not powered
 * down; on the STM32F723 DTCM (with its clock on), SRAM1 and SRAM2; on the
 * STM32H7S3 the AXI SRAMs as the option bytes map them and the AHB SRAMs
 * with their clocks on, never DTCM. The port's other choices are in
 * u2ps_dwc2_dcd.h. */

#endif /* U2PS_DPORT_CFG_H */
