/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_fifo.h: FIFO RAM sizes of the DWC2 device port, for EP0, one
 * isochronous OUT data endpoint and one isochronous IN feedback endpoint.
 * A pure function with no register access; the port writes OTG_GRXFSIZ,
 * OTG_DIEPTXF0 and OTG_DIEPTXFx from it at every bus reset.
 *
 * Manuals: RM0431 (probably Rev 4, cited by section) and RM0456 Rev 7.
 * - The OTG_HS FIFO RAM holds 4 Kbytes, 1024 words (RM0431 32.11; RM0456
 *   73.11, p. 3307).
 * - Device Rx FIFO = (5 * control endpoints + 8) + (largest packet / 4 + 1,
 *   for the status word) + 2 * OUT endpoints + 1 for Global NAK, where the
 *   manual's example counts EP0 among the OUT endpoints. "Typically, two
 *   (largest packet size / 4) + 1 spaces are recommended so that when the
 *   previous packet is being transferred to the CPU, the USB can receive
 *   the subsequent packet" (RM0431 32.11.3; RM0456 73.11.3, p. 3310-3311).
 *   The port takes two. The quotient is rounded up, so a packet whose size
 *   is not a multiple of 4 still fits (inferred; equal to the manual's value
 *   for multiples of 4).
 * - A Tx FIFO holds at least one maximum-size packet of its endpoint
 *   (RM0431 32.11.3; RM0456 p. 3311), and every Tx FIFO depth has a
 *   minimum value of 16 words (RM0431 32.15.13 and 32.15.19; RM0456
 *   73.14.16 and 73.14.22, p. 3345 and p. 3353).
 * - The Rx FIFO starts at word 0 ("Rx start address fixed to 0", the
 *   device-mode FIFO figure of RM0456 73.11.1, p. 3308). The Tx FIFOs
 *   follow it here. Neither manual states the unit of the start address
 *   fields. The reset values of OTG_DIEPTXFx place each Tx FIFO x at the
 *   end of the one before it when the depths are counted in words (0x400,
 *   0x600, ... with depths 0x200, RM0456 73.14.22, p. 3353; likewise the FS
 *   core's, RM0431 32.15.12, 32.15.13 and 32.15.19), so the start offsets
 *   here are words (inferred).
 * - Limits: RXFD at most 1024 and at most its power-on value, 0x400 at HS;
 *   a Tx FIFO depth at most its power-on value, 0x200 (RM0431 32.15.12,
 *   32.15.13 and 32.15.19; RM0456 73.14.14 and 73.14.16, p. 3344-3345).
 * For 776-byte packets and a 4-byte feedback value: 408, 16 and 16 words,
 * 440 of 1024.
 */
#ifndef U2PS_DWC2_FIFO_H
#define U2PS_DWC2_FIFO_H

#include <stdbool.h>
#include <stdint.h>

#define U2PS_DWC2_FIFO_WORDS  1024u /* FIFO RAM of OTG_HS, 32-bit words */
#define U2PS_DWC2_RXFIFO_MAX  1024u /* power-on RXFD at HS */
#define U2PS_DWC2_TXFIFO_MIN  16u   /* least depth of every Tx FIFO */
#define U2PS_DWC2_TXFIFO_MAX  512u  /* power-on depth of every Tx FIFO */
#define U2PS_DWC2_EP0_PACKET  64u   /* EP0 maximum packet size (USB 2.0 9.6.1) */

/* FIFO RAM sizes and start offsets, in 32-bit words */
typedef struct {
  uint16_t rx;          /* Rx FIFO depth (OTG_GRXFSIZ.RXFD), starting at word 0 */
  uint16_t tx0;         /* EP0 Tx FIFO depth (OTG_DIEPTXF0.TX0FD) */
  uint16_t tx_fb;       /* feedback endpoint Tx FIFO depth (OTG_DIEPTXFx.INEPTXFD) */
  uint16_t tx0_start;   /* word offset of the EP0 Tx FIFO */
  uint16_t tx_fb_start; /* word offset of the feedback Tx FIFO */
  uint16_t total;       /* words used */
} u2ps_dwc2_fifo_t;

/* The sizes for EP0, an isochronous OUT endpoint of out_mps bytes and a
 * feedback endpoint of fb_mps bytes. Returns false if they exceed the
 * limits above (out_mps above 1024 cannot be an isochronous HS packet,
 * USB 2.0 5.6.3); *f is filled in either way. */
bool u2ps_dwc2_fifo_plan(uint16_t out_mps, uint16_t fb_mps, u2ps_dwc2_fifo_t *f);

#endif /* U2PS_DWC2_FIFO_H */
