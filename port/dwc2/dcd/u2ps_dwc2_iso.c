/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_iso.c: the DWC2 device port's decision logic for its stream
 * (u2ps_dwc2_iso.h). No register access. The functions of the packet path
 * are in section .text.u2ps_fast (U2PS_FAST_TEXT).
 */
#include "u2ps_dwc2_iso.h"

#ifndef U2PS_DWC2_OLD_RULE
#define U2PS_DWC2_OLD_RULE 0
#endif

static bool parity(u2ps_uframe_t u)
{
  return (u & 1u) != 0u;
}

static u2ps_uframe_t fn14(uint32_t fnsof)
{
  return (u2ps_uframe_t)(fnsof & U2PS_UFRAME_MASK);
}

/* Forward distance from 'from' to 'to', 0..0x3FFF */
static uint16_t fwd(u2ps_uframe_t from, u2ps_uframe_t to)
{
  return u2ps_uframe_sub(to, from);
}

/* A forward distance that means "behind": the microframe lies one or two
 * before the reference (u2ps_dwc2_iso.h) */
static bool behind(uint16_t d)
{
  return d >= U2PS_UFRAME_MASK - 1u;
}

/* ---- Packet sizes ---------------------------------------------------------- */

uint16_t u2ps_dwc2_out_xfrsiz(uint16_t mps)
{
  return (uint16_t)(((uint32_t)mps + 3u) & ~3u);
}

/* ---- Class memory ---------------------------------------------------------- */

u2ps_dwc2_open_err_t u2ps_dwc2_mem_check(uintptr_t mem, uint16_t stride, uint16_t count,
                                         uint16_t mps, uint16_t count_min, uint16_t count_max,
                                         uint16_t align)
{
  if (count < count_min || count > count_max)
    return U2PS_DWC2_OPEN_COUNT;
  if (mem == 0u)
    return U2PS_DWC2_OPEN_MEM_NULL;
  if (stride < u2ps_dwc2_out_xfrsiz(mps))
    return U2PS_DWC2_OPEN_STRIDE_SHORT;
  if (stride % align != 0u)
    return U2PS_DWC2_OPEN_STRIDE_ALIGN;
  if (mem % align != 0u)
    return U2PS_DWC2_OPEN_MEM_ALIGN;
  return U2PS_DWC2_OPEN_OK;
}

/* ---- Microframes ----------------------------------------------------------- */

U2PS_FAST_TEXT u2ps_uframe_t u2ps_dwc2_uframe(uint32_t fnsof, bool odd)
{
  u2ps_uframe_t fn = fn14(fnsof);

  return u2ps_uframe_sub(fn, (uint16_t)((fn ^ (odd ? 1u : 0u)) & 1u));
}

U2PS_FAST_TEXT u2ps_dwc2_bnext_t u2ps_dwc2_bnext(u2ps_uframe_t p, u2ps_uframe_t now)
{
  u2ps_dwc2_bnext_t a;
  uint16_t d = fwd(p, now);

#if U2PS_DWC2_OLD_RULE
  /* Injection builds only: the rule before B-next, for the comparison */
  a.odd = parity(u2ps_uframe_add(now, 1u));
  a.late = d;
  return a;
#endif

  if (d <= 1u) {
    /* In time for P+1, possibly inside it */
    a.odd = parity(u2ps_uframe_add(p, 1u));
    a.late = 0;
  } else {
    a.odd = parity(u2ps_uframe_add(now, 1u));
    a.late = d;
  }
  return a;
}

/* ---- Isochronous OUT stream ------------------------------------------------ */

/* The buffer after the held ones: head + held is below 2 * count
 * (u2ps_dwc2_out_release), which passes 65,535 above 32,768 buffers, so
 * the sum stays in 32 bits until the subtraction */
U2PS_FAST_TEXT static uint16_t next_buf(const u2ps_dwc2_out_t *o)
{
  uint32_t b = (uint32_t)o->head + o->held;
  return (uint16_t)(b >= o->count ? b - o->count : b);
}

U2PS_FAST_TEXT static void no_packet_record(u2ps_dwc2_out_packet_t *r)
{
  r->buf = r->arm_buf = U2PS_DWC2_NO_BUF;
  r->late = r->no_buffer = r->no_packet = 0;
}

bool u2ps_dwc2_out_open(u2ps_dwc2_out_t *o, uint16_t count, uint32_t fnsof, bool *odd)
{
  o->state = U2PS_DWC2_OUT_CLOSED;
  if (count < U2PS_DWC2_OUT_RESERVE + 1u)
    return false;
  o->started = false;
  o->count = count;
  o->head = 0;
  o->held = 0;
  o->last = o->ref = o->seen = 0;
  o->late = o->no_buffer = o->no_packet = 0;
  o->armed_odd = parity(u2ps_uframe_add(fn14(fnsof), 1u));
  *odd = o->armed_odd;
  o->following = true;
  o->follow_fn = fn14(fnsof);
  o->state = U2PS_DWC2_OUT_ARMED;
  return true;
}

U2PS_FAST_TEXT bool u2ps_dwc2_out_eopf(u2ps_dwc2_out_t *o, uint32_t fnsof, bool *odd)
{
  u2ps_uframe_t fn = fn14(fnsof);

  if (!o->following || o->state != U2PS_DWC2_OUT_ARMED)
    return false;
  o->armed_odd = parity(u2ps_uframe_add(fn, 1u));
  o->follow_fn = fn;
  o->follows++;
  *odd = o->armed_odd;
  return true;
}

U2PS_FAST_TEXT void u2ps_dwc2_out_eopf_after(u2ps_dwc2_out_t *o, uint32_t fnsof)
{
  if (fn14(fnsof) != o->follow_fn)
    o->follow_late++;
}

void u2ps_dwc2_out_closing(u2ps_dwc2_out_t *o)
{
  if (o->state != U2PS_DWC2_OUT_CLOSED)
    o->state = U2PS_DWC2_OUT_CLOSING;
}

void u2ps_dwc2_out_close(u2ps_dwc2_out_t *o)
{
  o->state = U2PS_DWC2_OUT_CLOSED;
}

U2PS_FAST_TEXT bool u2ps_dwc2_out_complete(u2ps_dwc2_out_t *o, uint32_t fnsof, bool reread,
                              u2ps_dwc2_out_packet_t *r)
{
  u2ps_uframe_t p = u2ps_dwc2_uframe(fnsof, o->armed_odd);
  bool resync = false;

  o->following = false; /* the first XFRC: B-next takes over */
  r->uframe = p;
  if (o->state != U2PS_DWC2_OUT_ARMED) {
    if (o->state == U2PS_DWC2_OUT_CLOSING)
      o->close_drops++;
    else
      o->errors++; /* nothing was armed: the core cannot complete */
    no_packet_record(r);
    return true;
  }
  /* The guard: P lies after the last packet's microframe */
  if (o->started) {
    uint16_t d = fwd(o->last, p);
    if (d == 0u || behind(d)) {
      if (!reread)
        return false;
      /* Still behind: the hardware's microframe stands, and no miss is
       * reported for it */
      o->errors++;
      resync = true;
    }
  }
  uint32_t no_packet = 0;
  if (o->started && !resync) {
    /* The hole since the last packet, less what is reported or recorded */
    uint16_t gap = (uint16_t)(fwd(o->last, p) - 1u);
    uint16_t known = fwd(o->last, o->seen);
    if (gap >= known)
      no_packet = (uint16_t)(gap - known);
    else
      o->errors++; /* a packet inside microframes found lost */
  }
  if (o->started)
    no_packet += o->no_packet;
  /* Every completion is the base of the next arm and of the next hole */
  o->last = o->ref = o->seen = p;
  r->buf = next_buf(o);
  if (o->held + U2PS_DWC2_OUT_RESERVE >= o->count) {
    /* A reserve packet: delivered, it would leave fewer than the reserve
     * free. NO_BUFFER in its own microframe, recorded with the hole before
     * it for the next packet record; its buffer is armed again. Only after
     * the first packet: count > U2PS_DWC2_OUT_RESERVE. */
    o->no_packet = no_packet;
    o->no_buffer++;
    o->no_buffer_total++;
    no_packet_record(r);
    r->arm_buf = next_buf(o);
    o->state = U2PS_DWC2_OUT_DROPPED;
    return true;
  }
  r->late = o->late;
  r->no_buffer = o->no_buffer;
  r->no_packet = no_packet;
  o->late = o->no_buffer = o->no_packet = 0;
  o->started = true;
  o->held++;
  o->state = U2PS_DWC2_OUT_COMPLETED;
  r->arm_buf = next_buf(o);
  return true;
}

U2PS_FAST_TEXT uint16_t u2ps_dwc2_out_drop(u2ps_dwc2_out_t *o, uint32_t fnsof)
{
  u2ps_uframe_t p = u2ps_dwc2_uframe(fnsof, o->armed_odd);

  o->following = false;
  if (o->state != U2PS_DWC2_OUT_ARMED) {
    if (o->state == U2PS_DWC2_OUT_CLOSING)
      o->close_drops++;
    else
      o->errors++;
    return U2PS_DWC2_NO_BUF;
  }
  o->errors++;
  if (o->started) {
    uint16_t d = fwd(o->last, p);
    if (d == 0u || behind(d))
      p = o->last; /* cannot lie behind the last packet: no hole from it */
  }
  o->ref = p;
  o->state = U2PS_DWC2_OUT_DROPPED;
  return next_buf(o);
}

U2PS_FAST_TEXT bool u2ps_dwc2_out_arm(u2ps_dwc2_out_t *o, uint32_t fnsof, u2ps_dwc2_bnext_t *a)
{
  u2ps_uframe_t now = fn14(fnsof);
  uint8_t state = o->state;

  a->odd = false;
  a->late = 0;
  if (state != U2PS_DWC2_OUT_COMPLETED && state != U2PS_DWC2_OUT_DROPPED)
    return false;
  if (behind(fwd(o->ref, now))) {
    /* Read after the completion, now cannot lie before it */
    o->errors++;
    now = o->ref;
  }
  if (now != o->ref)
    o->late_completions++;
  if (state == U2PS_DWC2_OUT_COMPLETED) {
    *a = u2ps_dwc2_bnext(o->last, now);
    if (a->late != 0u) {
      o->late_total += a->late;
      o->seen = now;
    }
  } else {
    u2ps_dwc2_bnext_t b = u2ps_dwc2_bnext(o->ref, now);
    if (b.late != 0u && o->started) {
      /* A dropped packet's microframe and the hole before it, up to what
       * is already accounted for, are NO_PACKET (none after a reserve
       * packet, the base itself); the late arm's are LATE */
      uint16_t upto = fwd(o->last, o->ref);
      uint16_t known = fwd(o->last, o->seen);
      if (upto > known)
        o->no_packet += (uint16_t)(upto - known);
      o->late += b.late;
      o->late_total += b.late;
      o->seen = now;
    }
    a->odd = b.odd;
  }
  o->armed_odd = a->odd;
  o->state = U2PS_DWC2_OUT_ARMED;
  return true;
}

void u2ps_dwc2_out_tail(u2ps_dwc2_out_t *o, u2ps_dwc2_out_packet_t *r)
{
  no_packet_record(r);
  r->uframe = o->last;
  r->late = o->late;
  r->no_buffer = o->no_buffer;
  r->no_packet = o->no_packet;
  o->late = o->no_buffer = o->no_packet = 0;
}

/* ---- Feedback IN endpoint -------------------------------------------------- */

/* The window of u2ps_dwc2_fb_watch() for the state f is in now, after
 * every change of following, recovering, expect_known, expect or period:
 * empty while the parity follows the bus or a recovery runs; every
 * microframe while no poll is known; else from E - 1, the microframes
 * x = (fn - (E - 1)) mod 2^14 up to half the wrap (x <= 8192, which is
 * u2ps_uframe_dist(E, fn) >= -1) whose offset in the period,
 * x mod period, is 0 or 1 (E - 1 + k * period and E + k * period) */
U2PS_FAST_TEXT static void window(u2ps_dwc2_fb_t *f)
{
  if (f->following || f->recovering) {
    f->win_len = 0;
  } else if (!f->expect_known) {
    f->win_start = 0;
    f->win_len = U2PS_UFRAME_COUNT;
    f->win_mask = 0;
  } else {
    f->win_start = u2ps_uframe_sub(f->expect, 1u);
    f->win_len = U2PS_UFRAME_COUNT / 2u + 1u;
    f->win_mask = (uint16_t)(f->period - 1u);
  }
}

bool u2ps_dwc2_fb_open(u2ps_dwc2_fb_t *f, uint8_t interval, uint32_t fnsof)
{
  if (interval < U2PS_DWC2_FB_INTERVAL_MIN || interval > U2PS_DWC2_FB_INTERVAL_MAX)
    return false;
  u2ps_dwc2_fb_close(f);
  f->period = (uint16_t)(1u << (interval - 1u));
  f->armed_at = fn14(fnsof);
  f->odd = f->armed_odd = parity(u2ps_uframe_add(f->armed_at, 1u));
  f->following = true;
  f->confirmed = false;
  f->expect_known = false;
  f->recovering = false;
  f->expect = 0;
  f->data_seen = false;
  f->data_at = 0;
  window(f);
  return true;
}

U2PS_FAST_TEXT bool u2ps_dwc2_fb_eopf(u2ps_dwc2_fb_t *f, uint32_t fnsof)
{
  u2ps_uframe_t fn = fn14(fnsof);

  if (!f->following)
    return false;
  f->odd = f->armed_odd = parity(u2ps_uframe_add(fn, 1u));
  return true;
}

U2PS_FAST_TEXT bool u2ps_dwc2_fb_xfrc(u2ps_dwc2_fb_t *f, uint32_t fnsof)
{
  /* Q from the parity that was armed, which stays */
  u2ps_uframe_t q = u2ps_dwc2_uframe(fnsof, f->armed_odd);

  f->following = false;
  f->expect = u2ps_uframe_add(q, f->period);
  f->expect_known = true;
  f->confirmed = true;
  f->odd = f->armed_odd; /* cancels a switch the recovery would make */
  f->recoveries += f->pending; /* the polls go on: those were real misses */
  f->pending = 0;
  window(f);
  return !f->recovering;
}

void u2ps_dwc2_fb_close(u2ps_dwc2_fb_t *f)
{
  f->idle_recoveries += f->pending;
  f->pending = 0;
}

void u2ps_dwc2_fb_follow_off(u2ps_dwc2_fb_t *f)
{
  f->following = false;
  window(f);
}

/* A recovery or switch at microframe fn: pending for recoveries if a data
 * packet came in the period before fn (the window takes P up to two
 * microframes after fn, which a read order cannot give but costs nothing),
 * until a poll follows; otherwise idle */
U2PS_FAST_TEXT static void count_recovery(u2ps_dwc2_fb_t *f, u2ps_uframe_t fn)
{
  int16_t since = u2ps_uframe_dist(f->data_at, fn);
  if (f->data_seen && since >= -2 && since <= (int16_t)f->period)
    f->pending++;
  else
    f->idle_recoveries++;
}

U2PS_FAST_TEXT u2ps_dwc2_fb_action_t u2ps_dwc2_fb_iisoixfr(u2ps_dwc2_fb_t *f, uint32_t fnsof)
{
  u2ps_uframe_t fn = fn14(fnsof);

  if (f->following)
    return U2PS_DWC2_FB_NONE; /* the parity follows the bus; no poll yet */
  if (f->recovering)
    return U2PS_DWC2_FB_NONE;
  if (f->expect_known) {
    /* Only in an expected poll microframe, expect + k * period, or one
     * later if handled late */
    int16_t d = u2ps_uframe_dist(f->expect, fn);
    uint16_t off = d >= 0 ? (uint16_t)((uint16_t)d % f->period) : 0u;
    if (d < 0 || off > 1u)
      return U2PS_DWC2_FB_NONE;
    f->recovering = true;
    window(f);
    count_recovery(f, fn);
    if (f->confirmed) {
      /* The first missed poll after an XFRC: same parity */
      f->confirmed = false;
      f->expect = u2ps_uframe_add(u2ps_uframe_sub(fn, off), f->period);
      return U2PS_DWC2_FB_RECOVER;
    }
    /* No XFRC for a period after a recovery's arm: switch */
    f->expect_known = false;
    f->odd = !f->armed_odd;
    return U2PS_DWC2_FB_SWITCH;
  }
  /* No poll known: switch once a period has passed since the arm */
  if (u2ps_uframe_dist(f->armed_at, fn) < (int16_t)f->period)
    return U2PS_DWC2_FB_NONE;
  f->recovering = true;
  window(f);
  count_recovery(f, fn);
  f->odd = !f->armed_odd;
  return U2PS_DWC2_FB_SWITCH;
}

U2PS_FAST_TEXT void u2ps_dwc2_fb_rearm(u2ps_dwc2_fb_t *f, uint32_t fnsof)
{
  f->armed_at = fn14(fnsof);
  f->armed_odd = f->odd;
  f->recovering = false;
  window(f);
}
