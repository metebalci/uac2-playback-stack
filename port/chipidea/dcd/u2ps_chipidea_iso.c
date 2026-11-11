/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_chipidea_iso.c: the ChipIdea device port's decision logic for its
 * stream and its close (u2ps_chipidea_iso.h). No register access.
 */
#include "u2ps_chipidea_iso.h"

static u2ps_uframe_t u14(uint32_t v)
{
  return (u2ps_uframe_t)(v & U2PS_UFRAME_MASK);
}

/* Forward distance from 'from' to 'to', 0..0x3FFF */
static uint16_t fwd(u2ps_uframe_t from, u2ps_uframe_t to)
{
  return u2ps_uframe_sub(to, from);
}

/* A forward distance that means "behind": 1..U2PS_CHIPIDEA_BEHIND
 * microframes before the reference */
static bool behind(uint16_t d)
{
  return d > U2PS_UFRAME_MASK - U2PS_CHIPIDEA_BEHIND;
}

static void holes_add(u2ps_chipidea_holes_t *to, const u2ps_chipidea_holes_t *from)
{
  to->no_packet += from->no_packet;
  to->late += from->late;
  to->no_buffer += from->no_buffer;
  to->corrupted += from->corrupted;
}

static uint32_t holes_sum(const u2ps_chipidea_holes_t *h)
{
  return h->no_packet + h->late + h->no_buffer + h->corrupted;
}

static const u2ps_chipidea_holes_t no_holes;

/* ---- Descriptors and registers --------------------------------------------- */

uint32_t u2ps_chipidea_dtd_token(uint16_t bytes, uint8_t multo)
{
  if (bytes > 0x4000u)
    bytes = 0x4000u;
  return (uint32_t)bytes << 16 | 1u << 15 | (uint32_t)(multo & 3u) << 10 | U2PS_CHIPIDEA_DTD_ACTIVE;
}

uint16_t u2ps_chipidea_dtd_left(uint32_t token)
{
  return (uint16_t)((token >> 16) & 0x7FFFu);
}

uint32_t u2ps_chipidea_dqh_iso_caps(uint16_t mps)
{
  if (mps > 1024u)
    mps = 1024u;
  return 1u << 30 | 1u << 29 | (uint32_t)mps << 16;
}

uint32_t u2ps_chipidea_ep_bit(uint8_t ep, bool in)
{
  if (ep > 7u)
    return 0;
  return 1u << (ep + (in ? 16u : 0u));
}

/* ---- Isochronous OUT stream ------------------------------------------------ */

static uint16_t q_at(const u2ps_chipidea_out_t *o, uint16_t pos)
{
  return o->q[(o->q_head + pos) % o->count];
}

static void q_push(u2ps_chipidea_out_t *o, uint16_t slot)
{
  o->q[(o->q_head + o->q_len) % o->count] = slot;
  o->q_len++;
}

static uint16_t q_pop(u2ps_chipidea_out_t *o)
{
  uint16_t slot = o->q[o->q_head];
  o->q_head = (uint16_t)((o->q_head + 1u) % o->count);
  o->q_len--;
  return slot;
}

/* The slots released since the last retirement, then 'slot' unless it is
 * U2PS_CHIPIDEA_NO_SLOT, are queued at the tail; *a tells the register
 * layer */
static void append(u2ps_chipidea_out_t *o, uint16_t slot, u2ps_chipidea_append_t *a)
{
  a->tail = o->q_len != 0u ? q_at(o, (uint16_t)(o->q_len - 1u)) : U2PS_CHIPIDEA_NO_SLOT;
  a->first = o->q_len;
  a->n = 0;
  for (; o->h_free != 0u; o->h_free--) {
    q_push(o, o->h[o->h_head]);
    o->h_head = (uint16_t)((o->h_head + 1u) % o->count);
    o->h_len--;
    a->n++;
  }
  if (slot != U2PS_CHIPIDEA_NO_SLOT) {
    q_push(o, slot);
    a->n++;
  }
}

/* The surplus fell below what was reported: each microframe of the
 * difference counts once in fallback_over, the fallback count's expected
 * over-report, not in errors. Before the base is confirmed it moves the
 * base instead. */
static void under(u2ps_chipidea_out_t *o)
{
  if (o->surplus >= o->low)
    return;
  if (!o->based) {
    o->surplus = 0;
    return;
  }
  o->fallback_over += (uint32_t)(o->low - o->surplus);
  o->low = o->surplus;
}

/* x microframes without a packet found after the last report: NO_PACKET,
 * except that the first one after a dry prime counts with the dry run's
 * reason, LATE (a prime late in its microframe, RM p. 2416) */
static void report_no_packet(u2ps_chipidea_out_t *o, uint32_t x)
{
  if (x != 0u && o->grace) {
    x--;
    o->next.late++;
    o->late_total++;
  }
  o->grace = false;
  o->next.no_packet += x;
  o->no_packet_total += x;
}

/* An exact point: the surplus is the holes not yet accounted, and those
 * above 0 are reported before the next packet */
static void settle(u2ps_chipidea_out_t *o)
{
  if (o->surplus > 0) {
    report_no_packet(o, (uint32_t)o->surplus);
    o->surplus = 0;
    o->low = 0;
  } else {
    o->grace = false;
    under(o);
  }
  o->based = true;
}

bool u2ps_chipidea_out_open(u2ps_chipidea_out_t *o, uint16_t count, uint16_t mps, uint8_t unit)
{
  o->state = U2PS_CHIPIDEA_OUT_CLOSED;
  /* At 65535 slots the uint16_t count cannot exceed them, and the test
   * would be always false (-Wtype-limits) */
#if U2PS_CHIPIDEA_SLOT_MAX < 0xFFFFu
  bool too_many = count > U2PS_CHIPIDEA_SLOT_MAX;
#else
  bool too_many = false;
#endif
  if (count < U2PS_CHIPIDEA_ISO_OUT_MIN || too_many || mps == 0u ||
      mps > 1024u || unit > U2PS_CHIPIDEA_FN_FRAMES)
    return false;
  o->unit = unit;
  o->count = count;
  o->mps = mps;
  o->started = o->based = false;
  o->q_head = o->h_head = o->h_len = o->h_free = 0;
  o->q_len = 0;
  for (uint16_t i = 0; i < count; i++)
    q_push(o, i);
  o->delivered = o->grace = false;
  o->counted = o->n_since = o->pass_left = 0;
  o->pend_len = 0;
  o->ref = o->now = o->uframe = 0;
  o->surplus = o->surplus_prev = o->low = 0;
  o->next = no_holes;
  o->state = U2PS_CHIPIDEA_OUT_OPEN;
  return true;
}

uint16_t u2ps_chipidea_out_at(const u2ps_chipidea_out_t *o, uint16_t pos)
{
  if (o->state == U2PS_CHIPIDEA_OUT_CLOSED || pos >= o->q_len)
    return U2PS_CHIPIDEA_NO_SLOT;
  return q_at(o, pos);
}

/* Moves ref forward to 'to', counting 'retired' more dTDs at or before it */
static void advance(u2ps_chipidea_out_t *o, u2ps_uframe_t to, uint16_t retired)
{
  uint16_t d = fwd(o->ref, to);

  if (behind(d)) {
    o->errors++; /* read after everything counted: cannot lie before ref */
    d = 0;
    to = o->ref;
  }
  o->surplus += (int32_t)d - (int32_t)o->n_since - (int32_t)retired;
  o->ref = to;
  o->n_since = 0;
}

/* A dry run up to FRINDEX t: ref moves to t, counting 'retired' more dTDs,
 * and the surplus there, exact as no packet can be in flight, is the run,
 * LATE: the stack never holds the reserve (U2PS_CHIPIDEA_ISO_OUT_RESERVE),
 * so a dry endpoint is the port's own loss. */
static u2ps_chipidea_holes_t dry_run(u2ps_chipidea_out_t *o, u2ps_uframe_t t,
                                       uint16_t retired)
{
  u2ps_chipidea_holes_t h = no_holes;

  advance(o, t, retired);
  uint32_t dry = o->surplus > 0 ? (uint32_t)o->surplus : 0u;
  if (o->surplus > 0) {
    o->surplus = 0;
    o->low = 0;
  } else {
    under(o);
  }
  o->based = true;
  o->surplus_prev = o->surplus;
  h.late = dry;
  o->late_total += h.late;
  return h;
}

void u2ps_chipidea_out_pass(u2ps_chipidea_out_t *o, uint16_t n, uint32_t frindex)
{
  if (o->state != U2PS_CHIPIDEA_OUT_OPEN || n == 0u)
    return;
  if (n > o->q_len) {
    o->errors++;
    n = o->q_len;
  }
  o->now = u14(frindex);
  o->pass_left = n;
  if (o->unit != U2PS_CHIPIDEA_FN_NONE)
    return; /* the dTDs are counted one by one, from their Frame Numbers */

  /* The fallback count: microframes up to FRINDEX less the dTDs retired */
  uint16_t newly = n > o->counted ? (uint16_t)(n - o->counted) : 0u;
  if (n > o->counted)
    o->counted = n;
  if (!o->started) {
    /* The base: the pass's dTDs taken as the last n microframes */
    o->started = true;
    o->ref = o->now;
    o->surplus = o->surplus_prev = o->low = 0;
    o->uframe = u2ps_uframe_sub(o->now, n);
    return;
  }
  advance(o, o->now, newly);
  /* The duplicate guard: report only what two passes in a row show */
  int32_t certain = o->surplus < o->surplus_prev ? o->surplus : o->surplus_prev;
  if (certain > 0) {
    report_no_packet(o, (uint32_t)certain);
    o->surplus -= certain;
    o->low = 0;
    o->based = true;
  } else {
    if (o->surplus <= 0)
      o->grace = false; /* no microframe without a packet since the prime */
    under(o);
  }
  o->surplus_prev = o->surplus;
}

/* A dTD at microframe boundary b retired (FN_UFRAMES: b = P - 1, the
 * microframe before its packet; FN_FRAMES: b = 8F - 1, the microframe
 * before its frame): returns false if b is out of order */
static bool position(u2ps_chipidea_out_t *o, u2ps_uframe_t b)
{
  uint16_t d = fwd(o->ref, b);

  if (o->unit == U2PS_CHIPIDEA_FN_FRAMES) {
    /* The same frame as ref, or as the last boundary: counted only */
    if (d == 0u || behind(d))
      return true;
    if (d > U2PS_UFRAME_MASK - 2u * U2PS_CHIPIDEA_BEHIND)
      return false; /* an earlier frame */
  } else if (behind(d) || (d == 0u && o->n_since != 0u)) {
    return false; /* not after the last packet */
  }
  advance(o, b, 0);
  settle(o);
  return true;
}

void u2ps_chipidea_out_retire(u2ps_chipidea_out_t *o, uint32_t token, uint32_t fn,
                                u2ps_chipidea_out_packet_t *r)
{
  uint8_t status = (uint8_t)(token & 0xFFu);

  r->holes = no_holes;
  r->buf = U2PS_CHIPIDEA_NO_SLOT;
  r->len = 0;
  r->uframe = U2PS_UFRAME_UNKNOWN;
  r->append.first = r->append.n = 0;
  r->append.tail = U2PS_CHIPIDEA_NO_SLOT;
  bool overflow = (status & (U2PS_CHIPIDEA_DTD_BUF_ERROR | U2PS_CHIPIDEA_DTD_HALTED)) != 0u;
  if (o->state == U2PS_CHIPIDEA_OUT_CLOSED || o->q_len == 0u ||
      ((status & U2PS_CHIPIDEA_DTD_ACTIVE) != 0u && !overflow)) {
    o->errors++;
    return;
  }
  uint16_t slot = q_pop(o);
  uint16_t pass_left = o->pass_left;
  if (o->pass_left != 0u)
    o->pass_left--;
  bool counted = o->counted != 0u;
  if (counted)
    o->counted--;
  if (o->state == U2PS_CHIPIDEA_OUT_CLOSING) {
    o->close_drops++;
    return;
  }

  bool corrupt = !overflow && (status & U2PS_CHIPIDEA_DTD_XACT_ERROR) != 0u;
  bool exact = false; /* uframe holds the packet's own microframe */
  u2ps_uframe_t p = 0;

  if (overflow) {
    /* Not a packet: its microframe is a hole, LATE with the halt */
    o->errors++;
    if (counted)
      o->surplus++;
  } else if (!counted && o->unit != U2PS_CHIPIDEA_FN_NONE) {
    /* Count it at its Frame Number, which lies at or before FRINDEX */
    u2ps_uframe_t b;
    if (o->unit == U2PS_CHIPIDEA_FN_UFRAMES) {
      p = u2ps_uframe_sub(o->now, (uint16_t)((o->now - fn) & U2PS_FRAME_MASK));
      b = u2ps_uframe_sub(p, 1u);
      exact = true;
    } else {
      b = u2ps_uframe_sub(u2ps_uframe_make((uint16_t)fn, 0), 1u);
    }
    if (!o->started) {
      /* The base. FN_FRAMES: the first packet lies in its frame, at or
       * before FRINDEX less the pass's dTDs after it */
      o->started = true;
      o->surplus = o->surplus_prev = o->low = 0;
      if (o->unit == U2PS_CHIPIDEA_FN_UFRAMES) {
        o->based = true;
        o->ref = b;
      } else {
        u2ps_uframe_t lo = u2ps_uframe_add(b, 1u);
        u2ps_uframe_t est = u2ps_uframe_sub(o->now, pass_left != 0u ? (uint16_t)(pass_left - 1u) : 0u);
        int16_t d = u2ps_uframe_dist(lo, est);
        if (d < 0)
          est = lo;
        else if (d > 7)
          est = u2ps_uframe_add(lo, 7u);
        o->ref = u2ps_uframe_sub(est, 1u);
        o->uframe = o->ref;
      }
      o->n_since = 0;
    } else if (!position(o, b)) {
      /* Out of order: no hole from it; the count restarts here */
      o->errors++;
      o->ref = b;
      o->n_since = 0;
    }
    o->n_since++;
  }

  uint16_t again = slot; /* the slot to queue again, or none */
  if (overflow || corrupt) {
    if (corrupt) {
      o->next.corrupted++;
      o->corrupted_total++;
    }
  } else if (o->q_len + o->h_free < U2PS_CHIPIDEA_ISO_OUT_RESERVE) {
    /* A reserve packet: delivered, it would leave fewer than the reserve
     * free. NO_BUFFER in its own microframe, before the next packet or in
     * the tail; the slot is queued again, its data overwritten unread. Not
     * before the first packet: count > U2PS_CHIPIDEA_ISO_OUT_RESERVE. */
    o->next.no_buffer++;
    o->no_buffer_total++;
  } else {
    again = U2PS_CHIPIDEA_NO_SLOT;
    uint16_t left = u2ps_chipidea_dtd_left(token);
    if (left > o->mps) {
      o->errors++;
      left = o->mps;
    }
    if (o->delivered)
      r->holes = o->next; /* none is reported before the first packet */
    o->next = no_holes;
    o->delivered = true;
    r->buf = slot;
    r->len = (uint16_t)(o->mps - left);
    if (exact)
      r->uframe = p;
    else
      r->uframe = u2ps_uframe_add(o->uframe, (uint16_t)(1u + holes_sum(&r->holes)));
    o->uframe = r->uframe;
    o->h[(o->h_head + o->h_len) % o->count] = slot;
    o->h_len++;
  }
  append(o, again, &r->append);
  /* Dry primes whose retired dTDs have all been taken now */
  for (uint8_t i = 0; i < o->pend_len; i++)
    o->pend[i].k--;
  while (o->pend_len != 0u && o->pend[0].k == 0u) {
    u2ps_chipidea_dry_t e = o->pend[0];
    for (uint8_t i = 1; i < o->pend_len; i++)
      o->pend[i - 1u] = o->pend[i];
    o->pend_len--;
    if (o->unit == U2PS_CHIPIDEA_FN_NONE) {
      holes_add(&o->next, &e.holes);
      o->grace = true;
    } else if (o->started) {
      u2ps_chipidea_holes_t h = dry_run(o, e.t, 0);
      holes_add(&o->next, &h);
      o->grace = true;
    }
  }
}

void u2ps_chipidea_out_release(u2ps_chipidea_out_t *o, uint16_t n)
{
  if (n == 0u)
    return;
  if (o->state == U2PS_CHIPIDEA_OUT_CLOSED || n > o->h_len - o->h_free) {
    o->errors++;
    return;
  }
  if (o->state == U2PS_CHIPIDEA_OUT_CLOSING) {
    /* Freed, with those not yet queued: nothing is queued in a close */
    uint16_t k = (uint16_t)(o->h_free + n);
    o->h_head = (uint16_t)((o->h_head + k) % o->count);
    o->h_len = (uint16_t)(o->h_len - k);
    o->h_free = 0;
    return;
  }
  o->h_free = (uint16_t)(o->h_free + n);
}

uint16_t u2ps_chipidea_out_primed(u2ps_chipidea_out_t *o, uint16_t k, uint32_t frindex)
{
  if (o->state != U2PS_CHIPIDEA_OUT_OPEN || k > o->q_len) {
    o->errors++;
    return U2PS_CHIPIDEA_NO_SLOT;
  }
  uint16_t slot = k < o->q_len ? q_at(o, k) : U2PS_CHIPIDEA_NO_SLOT;
  u2ps_uframe_t t = u14(frindex);

  if (k == 0u) {
    /* The run follows the last dTD taken */
    if (o->started) {
      u2ps_chipidea_holes_t h = dry_run(o, t, 0);
      holes_add(&o->next, &h);
      o->grace = true;
    }
    return slot;
  }
  /* The run lies after the k retired dTDs still queued */
  u2ps_chipidea_dry_t e = {.k = k, .t = t, .holes = no_holes};
  if (o->unit == U2PS_CHIPIDEA_FN_NONE) {
    if (!o->started)
      return slot;
    /* No Frame Numbers to place them by: counted at the prime */
    uint16_t newly = k > o->counted ? (uint16_t)(k - o->counted) : 0u;
    if (k > o->counted)
      o->counted = k;
    e.holes = dry_run(o, t, newly);
  }
  if (o->pend_len == U2PS_CHIPIDEA_PEND_MAX) {
    o->errors++;
    holes_add(&e.holes, &o->pend[o->pend_len - 1u].holes);
    o->pend_len--;
  }
  o->pend[o->pend_len++] = e;
  return slot;
}

void u2ps_chipidea_out_tail(u2ps_chipidea_out_t *o, u2ps_chipidea_holes_t *h)
{
  *h = o->delivered ? o->next : no_holes;
  o->next = no_holes;
  /* The next packet's estimated microframe counts the holes taken too */
  o->uframe = u2ps_uframe_add(o->uframe, (uint16_t)holes_sum(h));
}

void u2ps_chipidea_out_closing(u2ps_chipidea_out_t *o)
{
  if (o->state == U2PS_CHIPIDEA_OUT_OPEN)
    o->state = U2PS_CHIPIDEA_OUT_CLOSING;
}

void u2ps_chipidea_out_close(u2ps_chipidea_out_t *o)
{
  o->state = U2PS_CHIPIDEA_OUT_CLOSED;
}

/* ---- Feedback IN endpoint -------------------------------------------------- */

bool u2ps_chipidea_fb_open(u2ps_chipidea_fb_t *f, uint8_t interval)
{
  f->state = U2PS_CHIPIDEA_OUT_CLOSED;
  if (interval < U2PS_CHIPIDEA_FB_INTERVAL_MIN || interval > U2PS_CHIPIDEA_FB_INTERVAL_MAX)
    return false;
  f->head = 0;
  f->len = 2;
  f->q[0] = 0;
  f->q[1] = 1;
  f->state = U2PS_CHIPIDEA_OUT_OPEN;
  return true;
}

uint16_t u2ps_chipidea_fb_retire(u2ps_chipidea_fb_t *f, uint32_t token, uint16_t *tail)
{
  *tail = U2PS_CHIPIDEA_NO_SLOT;
  if (f->state == U2PS_CHIPIDEA_OUT_CLOSED || f->len == 0u ||
      (token & U2PS_CHIPIDEA_DTD_ACTIVE) != 0u) {
    f->errors++;
    return U2PS_CHIPIDEA_NO_SLOT;
  }
  uint8_t d = f->q[f->head];
  f->head ^= 1u;
  f->len--;
  if (f->state == U2PS_CHIPIDEA_OUT_CLOSING)
    return U2PS_CHIPIDEA_NO_SLOT;
  f->polls++;
  if (f->len != 0u)
    *tail = f->q[f->head];
  f->q[(f->head + f->len) & 1u] = d;
  f->len++;
  return d;
}

uint16_t u2ps_chipidea_fb_primed(u2ps_chipidea_fb_t *f, uint16_t k)
{
  if (f->state != U2PS_CHIPIDEA_OUT_OPEN || k >= f->len) {
    if (f->state != U2PS_CHIPIDEA_OUT_OPEN || k > f->len)
      f->errors++;
    return U2PS_CHIPIDEA_NO_SLOT;
  }
  f->recoveries++;
  return f->q[(f->head + k) & 1u];
}

void u2ps_chipidea_fb_closing(u2ps_chipidea_fb_t *f)
{
  if (f->state == U2PS_CHIPIDEA_OUT_OPEN)
    f->state = U2PS_CHIPIDEA_OUT_CLOSING;
}

void u2ps_chipidea_fb_close(u2ps_chipidea_fb_t *f)
{
  f->state = U2PS_CHIPIDEA_OUT_CLOSED;
}

/* ---- Close ----------------------------------------------------------------- */

uint32_t u2ps_chipidea_ep_ctrl(uint8_t ep, uint8_t ep_out, uint8_t ep_fb, bool enable)
{
  uint32_t v = 0;

  if (ep == 0u || ep > 7u)
    return 0;
  if (ep == ep_out)
    v |= 1u << 2 | (enable ? 1u << 7 | 1u << 6 : 0u); /* RXT iso, RXE, RXR */
  else
    v |= 2u << 2; /* RXT bulk */
  if (ep_fb != 0u && ep == ep_fb)
    v |= 1u << 18 | (enable ? 1u << 23 | 1u << 22 : 0u); /* TXT iso, TXE, TXR */
  else
    v |= 2u << 18; /* TXT bulk */
  return v;
}

static uint32_t stream_mask(uint8_t ep_out, uint8_t ep_fb)
{
  uint32_t m = u2ps_chipidea_ep_bit(ep_out, false);
  if (ep_fb != 0u)
    m |= u2ps_chipidea_ep_bit(ep_fb, true);
  return m;
}

uint32_t u2ps_chipidea_close_begin(u2ps_chipidea_close_t *c, uint8_t ep_out, uint8_t ep_fb)
{
  c->mask = stream_mask(ep_out, ep_fb);
  c->active = true;
  c->quiet = 0;
  c->sofs = 0;
  c->flushes = 1;
  return c->mask;
}

u2ps_chipidea_close_action_t u2ps_chipidea_close_sof(u2ps_chipidea_close_t *c,
                                                         uint32_t endptflush, uint32_t endptprime,
                                                         uint32_t endptstat, uint32_t *flush)
{
  *flush = 0;
  if (!c->active)
    return U2PS_CHIPIDEA_CLOSE_DONE;
  c->sofs++;
  if ((endptflush & c->mask) != 0u) {
    c->quiet = 0;
    return U2PS_CHIPIDEA_CLOSE_WAIT;
  }
  if ((endptstat & c->mask) != 0u) {
    /* A packet was in progress: flush the endpoints still ready again */
    *flush = endptstat & c->mask;
    c->flushes++;
    c->quiet = 0;
    return U2PS_CHIPIDEA_CLOSE_FLUSH;
  }
  if ((endptprime & c->mask) != 0u || ++c->quiet < 2u) {
    if ((endptprime & c->mask) != 0u)
      c->quiet = 0;
    return U2PS_CHIPIDEA_CLOSE_WAIT;
  }
  c->active = false;
  return U2PS_CHIPIDEA_CLOSE_DONE;
}

bool u2ps_chipidea_close_idle(u2ps_chipidea_close_t *c, uint32_t endptprime,
                                uint32_t endptstat, uint8_t ep_out, uint8_t ep_fb)
{
  if (((endptprime | endptstat) & stream_mask(ep_out, ep_fb)) != 0u) {
    c->open_busy++;
    return false;
  }
  return true;
}
