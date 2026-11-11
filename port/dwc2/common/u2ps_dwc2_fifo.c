/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_fifo.c: FIFO RAM sizes of the DWC2 device port
 * (u2ps_dwc2_fifo.h). No register access.
 */
#include "u2ps_dwc2_fifo.h"

#define CTRL_EPS 1u /* EP0 */
#define OUT_EPS  2u /* EP0 and the data endpoint, as the manual's example counts */

static uint32_t words(uint32_t bytes)
{
  return (bytes + 3u) / 4u;
}

static uint32_t tx_depth(uint32_t mps)
{
  uint32_t w = words(mps);
  return w < U2PS_DWC2_TXFIFO_MIN ? U2PS_DWC2_TXFIFO_MIN : w;
}

bool u2ps_dwc2_fifo_plan(uint16_t out_mps, uint16_t fb_mps, u2ps_dwc2_fifo_t *f)
{
  uint32_t largest = out_mps > U2PS_DWC2_EP0_PACKET ? out_mps : U2PS_DWC2_EP0_PACKET;
  /* Two packets with their status words, so one is read out while the next
   * comes in */
  uint32_t rx = (5u * CTRL_EPS + 8u) + 2u * (words(largest) + 1u) + 2u * OUT_EPS + 1u;
  uint32_t tx0 = tx_depth(U2PS_DWC2_EP0_PACKET);
  uint32_t tx_fb = tx_depth(fb_mps);
  uint32_t total = rx + tx0 + tx_fb;

  f->rx = (uint16_t)rx;
  f->tx0 = (uint16_t)tx0;
  f->tx_fb = (uint16_t)tx_fb;
  f->tx0_start = (uint16_t)rx;
  f->tx_fb_start = (uint16_t)(rx + tx0);
  f->total = (uint16_t)total;
  return out_mps <= 1024u && rx <= U2PS_DWC2_RXFIFO_MAX && tx_fb <= U2PS_DWC2_TXFIFO_MAX &&
         total <= U2PS_DWC2_FIFO_WORDS;
}
