/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_host_stream.c: the stream's packet path: the size of each packet
 * (the accumulator), the queue of the app's packets, the device's feedback,
 * holes and drained microframes, and the packet path's counters. The
 * control sequence of a start and a stop is in u2ps_host.c.
 *
 * The accumulator (Formats 2.3.1.1; USB 2.0 5.12.4.2, p. 74) counts audio
 * slots in units of 1/(8000 * 2^16) slot, in 64 bits, so that both the
 * nominal rate fs/8000 (fs * 2^16 units) and a 16.16 feedback value Ff
 * (Ff * 8000 units) are exact. From the stream's first packet on, every
 * bus microframe adds the current rate: each packet's own microframe when
 * its size is latched, and each LATE or NO_BUFFER microframe when the port
 * reports it; a drained microframe adds nothing. The latch takes n =
 * min(INT(A), INT(rate) + 1) slots and subtracts them, so a packet is large
 * as soon as the accumulated fraction reaches 1, and a hole's debt is
 * repaid by packets of INT(rate) + 1 slots. The debt is capped at
 * U2PS_HOST_DEBT_MAX microframes' worth: the whole slots beyond it are
 * dropped and counted in slots_forgiven. A packet of more slots than
 * max_packet_size carries is cut to fit; the cut slots are dropped and
 * counted in slots_clamped. So, over a stream, the slots sent plus the
 * slots forgiven and clamped differ from the rate summed over its bus
 * microframes, drained ones excepted, by less than one slot.
 *
 * References: USB 2.0 Specification, Revision 2.0; Formats is the Audio
 * Data Formats of USB Audio Class 2.0, Release 2.0.
 */
#include <stddef.h>
#include <string.h>

#include "u2ps_host_priv.h"
#include "u2ps_hport.h"
#include "u2ps_hport_cfg.h"

_Static_assert(U2PS_HOST_DEBT_MAX >= 1u && U2PS_HOST_DEBT_MAX <= 8000u,
               "U2PS_HOST_DEBT_MAX must be 1..8000 microframes: set -DU2PS_HOST_DEBT_MAX "
               "(default 8)");

/* One audio slot in the accumulator's units */
#define SLOT ((uint64_t)8000u << 16)

/* The feedback value: 4 bytes at high speed, 16.16, its top 4 bits zero
 * (USB 2.0 5.12.4.2) */
#define FB_LEN 4u

bool u2ps_host_in_upcall;

static struct {
  bool open;    /* the port's stream is open: its reports go to the app */
  bool running; /* packets may be queued */
  uint32_t stream;
  bool explicit_feedback;
  uint16_t slot_bytes; /* nr_channels * subslot_size */
  uint16_t max_slots;  /* the most slots a packet carries within max_packet_size */
  uint64_t nominal;    /* the nominal rate, units per microframe */
  uint64_t rate;       /* the current rate: Ff, or the nominal one */
  uint64_t acc;        /* the accumulator A */
  bool latched;        /* the next packet's size is latched */
  uint16_t latch;
  uint32_t queued, sent; /* packets queued, and reported sent */
  bool drain_asked;      /* u2ps_host_drain(), and no queue since */
  bool draining;         /* the NO_BUFFER microframes before packet drain_packet are drained */
  uint32_t drain_packet;
  bool last_known;
  u2ps_uframe_t last;    /* the microframe of the last packet sent */
  /* The stack's counters of u2ps_host_stats_t */
  uint32_t drained, fb_zero_length, fb_rejected, slots_clamped, slots_forgiven, queue_errors,
    uframe_back;
} s;

/* A call of any context: from thread it masks the port's vectors */
static bool enter(void)
{
  if (u2ps_host_in_upcall)
    return false;
  U2PS_HOST_MASK(true);
  u2ps_host_in_upcall = true;
  return true;
}

static void leave(bool masked)
{
  if (!masked)
    return;
  u2ps_host_in_upcall = false;
  U2PS_HOST_MASK(false);
}

#if U2PS_CONTRACT_CHECKS
/* Where the port's DMA reaches a packet, as it reported at the last
 * successful init (no stream runs without one) */
static u2ps_hport_range_t reach[U2PS_HPORT_RANGES_MAX];
static uint32_t reach_count;
#endif

void u2ps_host_stream_reset(void)
{
  memset(&s, 0, sizeof s);
}

void u2ps_host_stream_begin(uint32_t stream, const u2ps_host_alternate_setting_t *a,
                            uint32_t sampling_frequency)
{
  s.open = s.running = true;
  s.stream = stream;
  s.explicit_feedback = a->explicit_feedback;
  s.slot_bytes = (uint16_t)(a->nr_channels * a->format.subslot_size);
  s.max_slots = (uint16_t)(a->max_packet_size / s.slot_bytes);
  s.nominal = s.rate = (uint64_t)sampling_frequency << 16;
  s.acc = 0;
  s.latched = s.drain_asked = s.draining = s.last_known = false;
  s.queued = s.sent = 0;
}

void u2ps_host_stream_closing(void)
{
  s.running = false;
}

void u2ps_host_stream_end(void)
{
  s.open = s.running = false;
}

void u2ps_host_stream_stats(u2ps_host_stats_t *st)
{
  st->no_buffer = st->no_buffer > s.drained ? st->no_buffer - s.drained : 0u;
  st->drained = s.drained;
  st->fb_zero_length = s.fb_zero_length;
  st->fb_rejected = s.fb_rejected;
  st->slots_clamped = s.slots_clamped;
  st->slots_forgiven = s.slots_forgiven;
  st->queue_errors = s.queue_errors;
  st->uframe_back = s.uframe_back;
}

/* The next packet's size, latched until it is queued: its microframe adds
 * the rate */
static uint16_t latch(void)
{
  if (!s.latched) {
    s.acc += s.rate;
    uint64_t n = s.acc / SLOT;
    uint64_t top = s.rate / SLOT + 1u;
    if (n > top)
      n = top;
    s.acc -= n * SLOT;
    if (n > s.max_slots) {
      s.slots_clamped += (uint32_t)(n - s.max_slots);
      n = s.max_slots;
    }
    s.latch = (uint16_t)n;
    s.latched = true;
  }
  return s.latch;
}

/* 'uframes' LATE or NO_BUFFER microframes add their rate; the debt beyond
 * U2PS_HOST_DEBT_MAX microframes' worth is forgiven in whole slots */
static void owe(uint16_t uframes)
{
  s.acc += (uint64_t)uframes * s.rate;
  uint64_t cap = (uint64_t)U2PS_HOST_DEBT_MAX * s.rate;
  if (s.acc > cap) {
    uint64_t whole = (s.acc - cap) / SLOT;
    s.acc -= whole * SLOT;
    s.slots_forgiven += (uint32_t)whole;
  }
}

/* ---- Calls --------------------------------------------------------------------- */

uint16_t u2ps_host_slots_next(uint32_t stream)
{
  uint16_t n = 0;
  bool m = enter();
  if (s.running && stream == s.stream)
    n = latch();
  leave(m);
  return n;
}

#if U2PS_CONTRACT_CHECKS
void u2ps_host_stream_ranges(void)
{
  reach_count = u2ps_hport_dma_ranges(reach); /* at most U2PS_HPORT_RANGES_MAX */
}

/* Every byte of [data, data + len) in one range, without a wrap */
static bool reached(const uint8_t *data, uint32_t len)
{
  uintptr_t a = (uintptr_t)data, end = a + len;
  for (uint32_t i = 0; i < reach_count; i++)
    if (a >= reach[i].start && end >= a && end <= reach[i].end)
      return true;
  return false;
}

/* Why a queue was refused, in the order of the header's causes. A stream
 * that does not run is reported from thread only for a number never
 * returned: the interrupt may have stopped it just before. */
static void check_refused(uint32_t stream, const uint8_t *data, bool callback)
{
  if (!s.running || stream != s.stream) {
    if (stream == 0u || stream > u2ps_host_last_stream || callback)
      U2PS_HOST_CONTRACT(U2PS_CONTRACT_HOST_QUEUE_STREAM);
  } else if (data == NULL || (uintptr_t)data % U2PS_HPORT_ALIGN != 0u) {
    U2PS_HOST_CONTRACT(U2PS_CONTRACT_HOST_QUEUE_DATA);
  } else {
    U2PS_HOST_CONTRACT(U2PS_CONTRACT_HOST_QUEUE_FULL);
  }
}
#endif

bool u2ps_host_queue(uint32_t stream, const uint8_t *data)
{
#if U2PS_CONTRACT_CHECKS
  bool callback = u2ps_host_callback();
  if (u2ps_host_masked && !callback)
    U2PS_HOST_CONTRACT(U2PS_CONTRACT_HOST_CALL_NESTED);
#endif
  bool m = enter();
  bool ok = s.running && stream == s.stream && data != NULL &&
            s.queued - s.sent < U2PS_HOST_QUEUE_MAX &&
            (uintptr_t)data % U2PS_HPORT_ALIGN == 0u;
  if (ok) {
#if U2PS_CONTRACT_CHECKS
    if (!s.latched)
      U2PS_HOST_CONTRACT(U2PS_CONTRACT_HOST_QUEUE_UNSIZED);
#endif
    uint16_t n = latch();
    s.latched = false;
    if (s.drain_asked) {
      /* The empty microframes before this packet are drained */
      s.drain_asked = false;
      s.draining = true;
      s.drain_packet = s.queued;
    }
    s.queued++;
#if U2PS_CONTRACT_CHECKS
    if (!reached(data, (uint32_t)n * s.slot_bytes))
      U2PS_HOST_CONTRACT(U2PS_CONTRACT_HOST_QUEUE_DATA);
#endif
    u2ps_hport_queue(data, (uint16_t)(n * s.slot_bytes));
  } else {
    s.queue_errors++;
#if U2PS_CONTRACT_CHECKS
    check_refused(stream, data, callback);
#endif
  }
  leave(m);
  return ok;
}

void u2ps_host_drain(uint32_t stream)
{
  bool m = enter();
  if (s.running && stream == s.stream)
    s.drain_asked = true;
  leave(m);
}

/* ---- Upcalls ------------------------------------------------------------------- */

void u2ps_host_on_sent(u2ps_uframe_t uframe)
{
  if (!s.open)
    return;
  u2ps_host_in_upcall = true;
  if (uframe != U2PS_UFRAME_UNKNOWN) {
    if (s.last_known && u2ps_uframe_dist(s.last, uframe) <= 0)
      s.uframe_back++;
    s.last = uframe;
    s.last_known = true;
  }
  if (s.draining && s.sent == s.drain_packet)
    s.draining = false;
  s.sent++;
  u2ps_app_host_on_sent(uframe);
  u2ps_host_in_upcall = false;
}

void u2ps_host_on_missed(uint16_t uframes, u2ps_miss_t why)
{
  if (!s.open || uframes == 0u)
    return;
  /* The empty microframes before the first packet after a drain */
  if (why == U2PS_MISS_NO_BUFFER && s.draining && s.sent == s.drain_packet) {
    s.drained += uframes;
    return;
  }
  /* A CORRUPTED packet counts as sent: no rate to add */
  if (why == U2PS_MISS_LATE || why == U2PS_MISS_NO_BUFFER)
    owe(uframes);
  u2ps_host_in_upcall = true;
  u2ps_app_host_on_hole(uframes, why);
  u2ps_host_in_upcall = false;
}

void u2ps_host_on_fb(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  (void)uframe;
  if (!s.open || !s.explicit_feedback)
    return;
  if (len == 0u) {
    s.fb_zero_length++;
    return;
  }
  uint32_t ff = len == FB_LEN ? u2ps_le32(data) : 0u;
  uint64_t rate = (uint64_t)ff * 8000u;
  /* Within +-12.5 % of the nominal rate */
  uint64_t off = rate > s.nominal ? rate - s.nominal : s.nominal - rate;
  if (len != FB_LEN || (ff >> 28) != 0u || 8u * off > s.nominal) {
    s.fb_rejected++;
    return;
  }
  s.rate = rate;
}

void u2ps_host_on_fb_missed(u2ps_uframe_t uframe, u2ps_miss_t why)
{
  /* The port counts it; the rate stays */
  (void)uframe;
  (void)why;
}
