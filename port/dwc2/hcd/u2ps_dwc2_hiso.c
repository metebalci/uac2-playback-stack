/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_hiso.c: the DWC2 host port's decision logic (u2ps_dwc2_hiso.h).
 * The functions of the packet path, which the port's handler calls at each
 * channel halt and arm, are in section .text.u2ps_fast (U2PS_FAST_TEXT).
 *
 * Manual: RM0456 Rev 7, chapter 73.
 */
#include "u2ps_dwc2_hiso.h"
#include "u2ps_types.h"

/* The parity of microframe u */
U2PS_FAST_TEXT static bool parity(u2ps_uframe_t u)
{
  return (u & 1u) != 0u;
}

/* FRNUM, bits 13:0 */
U2PS_FAST_TEXT static u2ps_uframe_t fn14(uint32_t frnum)
{
  return (u2ps_uframe_t)(frnum & U2PS_UFRAME_MASK);
}

U2PS_FAST_TEXT u2ps_uframe_t u2ps_dwc2_hframe(uint32_t frnum, bool odd)
{
  u2ps_uframe_t fn = fn14(frnum);

  return u2ps_uframe_sub(fn, (uint16_t)((fn ^ (odd ? 1u : 0u)) & 1u));
}

/* Whether FRNUM, read at the CHH of a transfer armed for 'target' with
 * parity 'odd', lies before the target: a read taken before the transfer */
U2PS_FAST_TEXT static bool stale(u2ps_uframe_t target, uint32_t frnum, bool odd)
{
  return u2ps_uframe_dist(target, u2ps_dwc2_hframe(frnum, odd)) < 0;
}

/* Microframes from..to inclusive, saturated to 16 bits */
U2PS_FAST_TEXT static uint16_t span(u2ps_uframe_t from, u2ps_uframe_t to)
{
  int16_t d = u2ps_uframe_dist(from, to);
  return d < 0 ? 0u : (uint16_t)(d + 1);
}

/* ---- The stream ---------------------------------------------------------------- */

bool u2ps_dwc2_hstream_fits(uint8_t ep_out, uint16_t mps, uint8_t ep_fb, uint8_t fb_interval,
                            uint16_t fb_mps)
{
  bool data = ep_out >= 1u && ep_out <= 15u && mps >= 1u && mps <= 1024u;
  bool fb = ep_fb == 0u || (ep_fb <= 15u && fb_interval >= 1u &&
                            fb_interval <= U2PS_DWC2_HFB_INTERVAL_MAX && fb_mps >= 4u &&
                            fb_mps <= U2PS_DWC2_HFB_MPS_MAX);
  return data && fb;
}

/* ---- Isochronous OUT data channel ------------------------------------------- */

void u2ps_dwc2_hout_open(u2ps_dwc2_hout_t *o)
{
  o->state = U2PS_DWC2_HOUT_OPEN;
  o->started = false;
  o->no_buffer = o->late = 0;
}

void u2ps_dwc2_hout_close(u2ps_dwc2_hout_t *o)
{
  o->state = U2PS_DWC2_HOUT_CLOSED;
}

U2PS_FAST_TEXT bool u2ps_dwc2_hout_arm(u2ps_dwc2_hout_t *o, uint32_t frnum)
{
  u2ps_uframe_t now = fn14(frnum);
  u2ps_uframe_t next = u2ps_uframe_add(now, 1u);

  switch (o->state) {
  case U2PS_DWC2_HOUT_OPEN:
    /* The first packet: microframes before it are no hole */
    o->target = next;
    break;
  case U2PS_DWC2_HOUT_COMPLETED: {
    /* In time only while the last packet's microframe lasts: the periodic
     * transactions of a microframe go first (RM0456 73.7.3), so an arm in
     * the target itself is late */
    int16_t d = u2ps_uframe_dist(o->last, now);
    if (d < 0) {
      o->errors++;
      d = 0;
    }
    if (d == 0) {
      o->target = u2ps_uframe_add(o->last, 1u);
    } else {
      o->late_completions++;
      o->late = (uint16_t)d;
      o->target = next;
    }
    break;
  }
  case U2PS_DWC2_HOUT_AGAIN:
    /* FRMOR: the packet missed its target; a stream's first packet leaves
     * no hole */
    if (o->started)
      o->late = (uint16_t)(o->late + span(o->target, now));
    o->target = next;
    break;
  case U2PS_DWC2_HOUT_DRY: {
    /* The queue was empty from the last packet's next microframe to now */
    int16_t d = u2ps_uframe_dist(o->last, now);
    o->no_buffer = d < 0 ? 0u : (uint16_t)d;
    o->target = next;
    break;
  }
  default:
    o->errors++;
    o->target = next;
    break;
  }
  o->odd = parity(o->target);
  o->state = U2PS_DWC2_HOUT_ARMED;
  return o->odd;
}

U2PS_FAST_TEXT void u2ps_dwc2_hout_dry(u2ps_dwc2_hout_t *o)
{
  if (o->state == U2PS_DWC2_HOUT_COMPLETED)
    o->state = U2PS_DWC2_HOUT_DRY;
}

U2PS_FAST_TEXT bool u2ps_dwc2_hout_chh(u2ps_dwc2_hout_t *o, uint32_t hcint, uint32_t frnum,
                                       bool reread, u2ps_dwc2_hout_report_t *r)
{
  *r = (u2ps_dwc2_hout_report_t){.sent = false};
  if (o->state != U2PS_DWC2_HOUT_ARMED) {
    o->errors++;
    return true;
  }
  /* A read before the target was taken before the transfer: read again */
  if (stale(o->target, frnum, o->odd)) {
    if (!reread)
      return false;
    o->errors++;
  }
  r->uframe = o->target;
  if ((hcint & (U2PS_DWC2_HCINT_XFRC | U2PS_DWC2_HCINT_AHBERR)) == 0u &&
      (hcint & U2PS_DWC2_HCINT_FRMOR) != 0u) {
    /* Not sent in its microframe: the same packet goes again */
    o->state = U2PS_DWC2_HOUT_AGAIN;
    r->again = true;
    return true;
  }
  if ((hcint & U2PS_DWC2_HCINT_AHBERR) != 0u) {
    /* The DMA could not fetch it: it did not cross the bus intact */
    r->corrupted = true;
    o->corrupted++;
    o->errors++;
  }
  r->sent = true;
  r->no_buffer = o->no_buffer;
  r->late = o->late;
  o->no_buffer_total += o->no_buffer;
  o->late_total += o->late;
  o->no_buffer = o->late = 0;
  o->last = o->target;
  o->started = true;
  o->sent++;
  o->state = U2PS_DWC2_HOUT_COMPLETED;
  return true;
}

/* ---- Isochronous IN feedback channel ----------------------------------------- */

bool u2ps_dwc2_hfb_open(u2ps_dwc2_hfb_t *f, uint8_t interval, uint32_t frnum)
{
  if (interval < 1u || interval > U2PS_DWC2_HFB_INTERVAL_MAX)
    return false;
  f->state = U2PS_DWC2_HFB_IDLE;
  f->period = (uint16_t)(1u << (interval - 1u));
  f->poll = u2ps_uframe_add(fn14(frnum), 1u);
  return true;
}

void u2ps_dwc2_hfb_close(u2ps_dwc2_hfb_t *f)
{
  f->state = U2PS_DWC2_HFB_CLOSED;
}

U2PS_FAST_TEXT uint16_t u2ps_dwc2_hfb_arm(u2ps_dwc2_hfb_t *f, uint32_t frnum, bool *odd,
                           u2ps_uframe_t *skipped)
{
  if (f->state != U2PS_DWC2_HFB_IDLE)
    return 0;
  u2ps_uframe_t now = fn14(frnum);
  uint16_t n = 0;
  /* A poll whose microframe has begun is skipped, not shifted: the next
   * stays one period later */
  int16_t d = u2ps_uframe_dist(now, f->poll);
  if (d < 1) {
    *skipped = f->poll;
    n = (uint16_t)((uint16_t)(-d) / f->period + 1u);
    f->poll = u2ps_uframe_add(f->poll, (uint16_t)(n * f->period));
    f->missed += n;
  }
  /* Armed only from the microframe right before the poll: the channel
   * polls in the next microframe of its parity (RM0456 p. 3360) */
  if (u2ps_uframe_dist(now, f->poll) > 1)
    return n;
  f->odd = *odd = parity(f->poll);
  f->state = U2PS_DWC2_HFB_ARMED;
  return n;
}

U2PS_FAST_TEXT bool u2ps_dwc2_hfb_chh(u2ps_dwc2_hfb_t *f, uint32_t hcint, uint32_t frnum,
                                      bool reread, u2ps_dwc2_hfb_result_t *result,
                                      u2ps_uframe_t *uframe)
{
  *result = U2PS_DWC2_HFB_NONE;
  if (f->state != U2PS_DWC2_HFB_ARMED) {
    f->errors++;
    return true;
  }
  if (stale(f->poll, frnum, f->odd)) {
    if (!reread)
      return false;
    f->errors++;
  }
  *uframe = f->poll;
  f->polls++;
  if ((hcint & U2PS_DWC2_HCINT_XFRC) != 0u) {
    *result = U2PS_DWC2_HFB_DATA;
  } else if ((hcint & U2PS_DWC2_HCINT_FRMOR) != 0u) {
    *result = U2PS_DWC2_HFB_LATE;
    f->missed++;
  } else {
    /* TXERR, babble, or a halt without either */
    if ((hcint & U2PS_DWC2_HCINT_BBERR) != 0u)
      f->errors++;
    *result = U2PS_DWC2_HFB_NO_PACKET;
    f->missed++;
  }
  f->poll = u2ps_uframe_add(f->poll, f->period);
  f->state = U2PS_DWC2_HFB_IDLE;
  return true;
}

/* ---- FIFO RAM in host mode ----------------------------------------------------- */

#define FIFO_WORDS 1024u /* OTG_HS FIFO RAM, 4 Kbytes (RM0456 73.11, p. 3307) */
#define TXFIFO_MIN 16u   /* least Tx FIFO depth (73.14.15, 73.14.21) */
#define EP0_PACKET 64u   /* EP0's packets at high speed (USB 2.0 9.6.1) */

static uint16_t words(uint16_t bytes)
{
  return (uint16_t)((bytes + 3u) / 4u);
}

bool u2ps_dwc2_hfifo_plan(uint16_t out_mps, uint16_t fb_mps, u2ps_dwc2_hfifo_t *f)
{
  uint16_t in_max = fb_mps > EP0_PACKET ? fb_mps : EP0_PACKET;
  /* Two of the largest IN packet, each with its status word, and the
   * transfer-complete word */
  f->rx = (uint16_t)(2u * (words(in_max) + 1u) + 1u);
  f->nptx = words(EP0_PACKET) > TXFIFO_MIN ? words(EP0_PACKET) : TXFIFO_MIN;
  /* Two packets of the isochronous OUT channel */
  f->ptx = (uint16_t)(2u * words(out_mps));
  if (f->ptx < TXFIFO_MIN)
    f->ptx = TXFIFO_MIN;
  f->nptx_start = f->rx;
  f->ptx_start = (uint16_t)(f->rx + f->nptx);
  f->total = (uint16_t)(f->rx + f->nptx + f->ptx);
  return out_mps >= 1u && out_mps <= 1024u && (fb_mps == 0u || (fb_mps >= 4u && fb_mps <= 1024u)) &&
         f->total <= FIFO_WORDS;
}
