/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dwc2_logic.c: the DWC2 device port's decision logic and FIFO sizes,
 * without registers:
 * - transfer size ceil4(mps) and the received length;
 * - the class memory checks behind U2PS_FAULT_OPEN, at the alignment of
 *   each chip (4 and 32);
 * - the packet's microframe P from FNSOF and the armed parity, read after
 *   XFRC, also for an interrupt that entered in P-1, and the guard with
 *   its window (behind only by 0, 1 or 2 microframes);
 * - B-next over every P, now in P..P+3, both parities, across the wrap;
 * - the stream: late completions, LATE, holes, gaps of more than 8192
 *   microframes; the reserve: a packet that would take the last free
 *   buffer is NO_BUFFER in its own microframe and its buffer armed again,
 *   with host gaps inside the stall, a late arm after it, stalls of 20,000
 *   and 70,000 packets, and the tail at the stream's end; releases that
 *   only free; dropped packets and the close; completions and releases in
 *   states the hardware cannot produce;
 * - the data endpoint's parity: following the bus at EOPF from the open
 *   until the first packet, without a time limit;
 * - the feedback parity: following the bus at EOPF from the open until the
 *   first poll, without a time limit, so the first poll is answered
 *   whatever its microframe; kept across XFRCs handled in the next
 *   microframe, switched after one period without XFRC with the follow
 *   off, IISOIXFR
 *   ignored outside the expected poll, an XFRC during a recovery; the
 *   microframes in which IISOIXFR needs to be unmasked, and the window the
 *   logic keeps for them against the rule over every microframe, after
 *   every step of random sequences;
 * - the ring of buffers: the buffer of each packet and each arm, reserve
 *   packets, and releases of every size, for counts of 2 to 16, and at
 *   40,000 and 65,535 buffers, where head + held passes 65,535;
 * - FIFO RAM 408/16/16 words for 776-byte packets.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "u2ps_dwc2_fifo.h"
#include "u2ps_dwc2_iso.h"

/* The port's configuration header, for one of its chips */
#define U2PS_CHIP_STM32U5A5 1
#include "../port/dwc2/u2ps_dport_cfg.h"

#define MASK U2PS_UFRAME_MASK

static u2ps_uframe_t uf(long v)
{
  return (u2ps_uframe_t)((unsigned long)v & MASK);
}

static bool odd_of(long v)
{
  return (uf(v) & 1u) != 0u;
}

/* ---- Packet sizes ---------------------------------------------------------- */

static void test_xfrsiz_len(void)
{
  CHECK_EQ(u2ps_dwc2_out_xfrsiz(1), 4);
  CHECK_EQ(u2ps_dwc2_out_xfrsiz(4), 4);
  CHECK_EQ(u2ps_dwc2_out_xfrsiz(5), 8);
  CHECK_EQ(u2ps_dwc2_out_xfrsiz(776), 776);
  CHECK_EQ(u2ps_dwc2_out_xfrsiz(777), 780);
  CHECK_EQ(u2ps_dwc2_out_xfrsiz(1023), 1024);
  CHECK_EQ(u2ps_dwc2_out_xfrsiz(1024), 1024);
  unsigned bad = 0, stride_bad = 0;
  for (unsigned mps = 1; mps <= 1024u; mps++) {
    unsigned x = u2ps_dwc2_out_xfrsiz((uint16_t)mps);
    if (x % 4u != 0u || x < mps || x - mps > 3u)
      bad++;
    /* Class memory holds what the port programs */
    if (U2PS_DPORT_STRIDE(mps) < x || U2PS_DPORT_STRIDE(mps) % U2PS_DPORT_ALIGN != 0u)
      stride_bad++;
  }
  CHECK_EQ(bad, 0);
  CHECK_EQ(stride_bad, 0);

  /* Length = programmed - remaining */
  u2ps_dwc2_out_t o;
  memset(&o, 0, sizeof o);
  CHECK_EQ(u2ps_dwc2_out_len(&o, 776, 0), 776);
  CHECK_EQ(u2ps_dwc2_out_len(&o, 780, 3), 777);
  CHECK_EQ(u2ps_dwc2_out_len(&o, 780, 4), 776);
  CHECK_EQ(u2ps_dwc2_out_len(&o, 776, 776), 0); /* zero-length packet */
  CHECK_EQ(u2ps_dwc2_out_len(&o, 388, 200), 188);
  CHECK_EQ(o.errors, 0);
  CHECK_EQ(u2ps_dwc2_out_len(&o, 4, 8), 0); /* cannot come from the core */
  CHECK_EQ(o.errors, 1);
}

/* ---- Class memory ---------------------------------------------------------- */

/* The causes behind U2PS_FAULT_OPEN that the app can meet, for both
 * alignments of the port (4: STM32U5, 32: STM32F723) */
static void test_mem_check(void)
{
  static const uint16_t aligns[] = {4, 32};
  for (unsigned a = 0; a < 2u; a++) {
    uint16_t al = aligns[a];
    uintptr_t mem = 0x20000000u;
    /* The least stride, U2PS_DPORT_STRIDE with this alignment, for every
     * packet size; and below it */
    unsigned bad = 0;
    for (unsigned mps = 1; mps <= 1024u; mps++) {
      uint16_t least = (uint16_t)((u2ps_dwc2_out_xfrsiz((uint16_t)mps) + al - 1u) & ~(al - 1u));
      if (u2ps_dwc2_mem_check(mem, least, 8, (uint16_t)mps, 2, 16, al) != U2PS_DWC2_OPEN_OK)
        bad++;
      u2ps_dwc2_open_err_t e = u2ps_dwc2_mem_check(mem, (uint16_t)(least - al), 8, (uint16_t)mps, 2, 16, al);
      if (least - al < u2ps_dwc2_out_xfrsiz((uint16_t)mps) && e != U2PS_DWC2_OPEN_STRIDE_SHORT)
        bad++;
    }
    CHECK_EQ(bad, 0);
    /* 776-byte packets: stride 776 holds them on both */
    uint16_t s = al == 4u ? 776u : 800u;
    CHECK_EQ(u2ps_dwc2_mem_check(mem, s, 8, 776, 2, 16, al), U2PS_DWC2_OPEN_OK);
    CHECK_EQ(u2ps_dwc2_mem_check(mem, s, 2, 776, 2, 16, al), U2PS_DWC2_OPEN_OK);
    CHECK_EQ(u2ps_dwc2_mem_check(mem, s, 16, 776, 2, 16, al), U2PS_DWC2_OPEN_OK);
    CHECK_EQ(u2ps_dwc2_mem_check(mem, s, 1, 776, 2, 16, al), U2PS_DWC2_OPEN_COUNT);
    CHECK_EQ(u2ps_dwc2_mem_check(mem, s, 17, 776, 2, 16, al), U2PS_DWC2_OPEN_COUNT);
    CHECK_EQ(u2ps_dwc2_mem_check(0, s, 8, 776, 2, 16, al), U2PS_DWC2_OPEN_MEM_NULL);
    CHECK_EQ(u2ps_dwc2_mem_check(mem, 772, 8, 776, 2, 16, al), U2PS_DWC2_OPEN_STRIDE_SHORT);
    CHECK_EQ(u2ps_dwc2_mem_check(mem, 777, 8, 777, 2, 16, al), U2PS_DWC2_OPEN_STRIDE_SHORT);
    /* A canary word after the least stride: fine on the U5, off ALIGN on
     * the F723 */
    CHECK_EQ(u2ps_dwc2_mem_check(mem, (uint16_t)(s + 4u), 8, 776, 2, 16, al),
             al == 4u ? U2PS_DWC2_OPEN_OK : U2PS_DWC2_OPEN_STRIDE_ALIGN);
    CHECK_EQ(u2ps_dwc2_mem_check(mem, (uint16_t)(s + 2u), 8, 776, 2, 16, al),
             U2PS_DWC2_OPEN_STRIDE_ALIGN);
    CHECK_EQ(u2ps_dwc2_mem_check(mem + 4u, s, 8, 776, 2, 16, al),
             al == 4u ? U2PS_DWC2_OPEN_OK : U2PS_DWC2_OPEN_MEM_ALIGN);
    CHECK_EQ(u2ps_dwc2_mem_check(mem + 2u, s, 8, 776, 2, 16, al), U2PS_DWC2_OPEN_MEM_ALIGN);
    CHECK_EQ(u2ps_dwc2_mem_check(mem + al, s, 8, 776, 2, 16, al), U2PS_DWC2_OPEN_OK);
  }
  /* This build's constants agree: U2PS_DPORT_STRIDE is the least stride */
  CHECK_EQ(u2ps_dwc2_mem_check(0x20000000u, U2PS_DPORT_STRIDE(777u), 2, 777, 2, 16,
                               U2PS_DPORT_ALIGN), U2PS_DWC2_OPEN_OK);
  CHECK_EQ(u2ps_dwc2_mem_check(0x20000000u, (uint16_t)(U2PS_DPORT_STRIDE(777u) - 4u), 2, 777, 2,
                               16, U2PS_DPORT_ALIGN), U2PS_DWC2_OPEN_STRIDE_SHORT);
}

/* ---- Microframes ----------------------------------------------------------- */

static void test_packet_uframe(void)
{
  unsigned bad = 0, early = 0;
  for (long p = 0; p <= (long)MASK; p++) {
    bool odd = odd_of(p);
    /* Read after XFRC: fn is P, or P+1 when handled in the next microframe */
    if (u2ps_dwc2_uframe(uf(p), odd) != uf(p))
      bad++;
    if (u2ps_dwc2_uframe(uf(p + 1), odd) != uf(p))
      bad++;
    /* FNSOF bits above 13 do not count */
    if (u2ps_dwc2_uframe((uint32_t)uf(p + 1) | 0x4000u, odd) != uf(p))
      bad++;
    /* A read in P-1, at an entry before the packet: two too early */
    if (u2ps_dwc2_uframe(uf(p - 1), odd) != uf(p - 2))
      early++;
  }
  CHECK_EQ(bad, 0);
  CHECK_EQ(early, 0);
  /* Across the wrap */
  CHECK_EQ(u2ps_dwc2_uframe(0x0000, true), 0x3FFF);
  CHECK_EQ(u2ps_dwc2_uframe(0x3FFF, true), 0x3FFF);
  CHECK_EQ(u2ps_dwc2_uframe(0x0000, false), 0x0000);
}

/* B-next over every P, now in P..P+3 and both parities (P's parity is
 * EONUM), across the 0x3FFF wrap */
static void test_bnext(void)
{
  unsigned bad[4] = {0, 0, 0, 0};
  for (long p = 0; p <= (long)MASK; p++) {
    for (long d = 0; d <= 3; d++) {
      u2ps_dwc2_bnext_t a = u2ps_dwc2_bnext(uf(p), uf(p + d));
      bool want_odd = d <= 1 ? odd_of(p + 1) : odd_of(p + d + 1);
      uint16_t want_late = d <= 1 ? 0u : (uint16_t)d;
      if (a.odd != want_odd || a.late != want_late)
        bad[d]++;
    }
  }
  CHECK_EQ(bad[0], 0);
  CHECK_EQ(bad[1], 0);
  CHECK_EQ(bad[2], 0);
  CHECK_EQ(bad[3], 0);

  /* now = P+1: the arm is for P+1 itself, not for parity(now+1) */
  u2ps_dwc2_bnext_t a = u2ps_dwc2_bnext(100, 101);
  CHECK_EQ(a.odd, true);
  CHECK_EQ(a.late, 0);
  a = u2ps_dwc2_bnext(0x3FFF, 0x0000);
  CHECK_EQ(a.odd, false); /* P+1 = 0x0000 */
  CHECK_EQ(a.late, 0);
  /* now = P+2 across the wrap: LATE 0x0000 and 0x0001, armed for 0x0002 */
  a = u2ps_dwc2_bnext(0x3FFF, 0x0001);
  CHECK_EQ(a.odd, false);
  CHECK_EQ(a.late, 2);
}

/* ---- Isochronous OUT stream ------------------------------------------------ */

static u2ps_dwc2_out_t out;

/* A packet in microframe p, completed with FNSOF read as fn; then the arm
 * decision with FNSOF read as now. The core receives only in microframes
 * of the armed parity, so p must have it. Returns the record; *a the
 * decision. */
static u2ps_dwc2_out_packet_t packet(long p, long fn, long now, bool *armed, u2ps_dwc2_bnext_t *a)
{
  u2ps_dwc2_out_packet_t r;
  memset(&r, 0xA5, sizeof r);
  CHECK_EQ(out.armed_odd, odd_of(p));
  bool ok = u2ps_dwc2_out_complete(&out, uf(fn), false, &r);
  CHECK(ok);
  CHECK_EQ(r.uframe, uf(p));
  *armed = u2ps_dwc2_out_arm(&out, uf(now), a);
  return r;
}

static void open_out(uint16_t count, long now, bool *odd)
{
  memset(&out, 0, sizeof out);
  CHECK(u2ps_dwc2_out_open(&out, count, uf(now), odd));
}

static void test_out_rearm(void)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  CHECK(!u2ps_dwc2_out_open(&out, 1, 0, &odd));
  open_out(16, 99, &odd);
  CHECK_EQ(odd, false); /* parity(100) */
  CHECK_EQ(out.armed_odd, false);

  /* In time: completion handled in P, re-armed in P for P+1 */
  r = packet(100, 100, 100, &armed, &a);
  CHECK_EQ(r.buf, 0);
  CHECK_EQ(r.arm_buf, 1);
  CHECK_EQ(r.no_packet, 0);
  CHECK(armed);
  CHECK_EQ(a.odd, true);
  CHECK_EQ(a.late, 0);
  CHECK_EQ(out.armed_odd, true);
  CHECK_EQ(out.late_completions, 0);

  /* Handled in P+1 (late completion): still armed for P+1 */
  r = packet(101, 102, 102, &armed, &a);
  CHECK_EQ(r.no_packet, 0);
  CHECK(armed);
  CHECK_EQ(a.odd, false); /* parity(102) */
  CHECK_EQ(a.late, 0);
  CHECK_EQ(out.late_completions, 1);

  /* The re-arm read P+2: P+1 and P+2 are LATE, armed for P+3 */
  r = packet(102, 102, 104, &armed, &a);
  CHECK(armed);
  CHECK_EQ(a.odd, true); /* parity(105) */
  CHECK_EQ(a.late, 2);
  CHECK_EQ(out.late_completions, 2);
  CHECK_EQ(out.late_total, 2);
  /* The next packet in P+3: the LATE microframes are no NO_PACKET */
  r = packet(105, 105, 105, &armed, &a);
  CHECK_EQ(r.no_packet, 0);
  CHECK_EQ(r.late, 0);

  /* Re-armed for P+1 inside P+1, after its token: the next packet comes in
   * P+3, and the two microframes between are NO_PACKET */
  r = packet(106, 107, 107, &armed, &a);
  CHECK_EQ(a.odd, true); /* parity(107) */
  r = packet(109, 109, 109, &armed, &a);
  CHECK_EQ(r.no_packet, 2);

  /* Armed for 110; the host skipped 110 to 113, and the next microframe of
   * the armed parity is 114 */
  r = packet(114, 114, 114, &armed, &a);
  CHECK_EQ(r.no_packet, 4);
  CHECK_EQ(out.errors, 0);
}

static void test_out_wrap(void)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  open_out(3, 0x3FFC, &odd);
  r = packet(0x3FFD, 0x3FFD, 0x3FFD, &armed, &a);
  /* Late across the wrap: completion of 0x3FFE handled in 0x0000 */
  r = packet(0x3FFE, 0x3FFE, 0x0000, &armed, &a);
  CHECK_EQ(r.no_packet, 0);
  CHECK_EQ(a.late, 2);
  CHECK_EQ(a.odd, true); /* parity(0x0001) */
  r = packet(0x0001, 0x0001, 0x0001, &armed, &a);
  CHECK_EQ(r.no_packet, 0);
  /* A hole across the wrap */
  open_out(3, 0x3FFD, &odd);
  r = packet(0x3FFE, 0x3FFE, 0x3FFE, &armed, &a);
  r = packet(0x0003, 0x0003, 0x0003, &armed, &a);
  CHECK_EQ(r.no_packet, 4); /* 0x3FFF, 0x0000, 0x0001, 0x0002 */
  CHECK_EQ(out.errors, 0);
}

/* The guard: P must lie after the last packet's microframe; behind means
 * 0, 1 or 2 microframes before or on it */
static void test_out_guard(void)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  open_out(4, 399, &odd);
  r = packet(400, 400, 400, &armed, &a); /* armed for 401 */
  /* The packet of 401 with FNSOF read in 400, before it: the formula gives
   * 399, behind the last packet; the guard asks for a re-read */
  CHECK(!u2ps_dwc2_out_complete(&out, 400, false, &r));
  CHECK_EQ(out.state, U2PS_DWC2_OUT_ARMED); /* nothing changed */
  CHECK_EQ(out.held, 1);
  CHECK(u2ps_dwc2_out_complete(&out, 401, true, &r));
  CHECK_EQ(r.uframe, 401);
  CHECK_EQ(r.no_packet, 0);
  CHECK_EQ(out.errors, 0);
  CHECK(u2ps_dwc2_out_arm(&out, 401, &a)); /* armed for 402 */

  /* Both reads behind: counted, the hardware's microframe stands, no miss */
  CHECK(!u2ps_dwc2_out_complete(&out, 401, false, &r));
  CHECK(u2ps_dwc2_out_complete(&out, 400, true, &r));
  CHECK_EQ(out.errors, 1);
  CHECK_EQ(r.uframe, 400);
  CHECK_EQ(r.no_packet, 0);
  CHECK_EQ(out.last, 400);
  CHECK(u2ps_dwc2_out_arm(&out, 400, &a));
  CHECK_EQ(a.odd, true); /* parity(401) */
  /* The next packet's hole counts from the resynchronised microframe (one
   * released, so that it is delivered) */
  u2ps_dwc2_out_release(&out, 1);
  r = packet(403, 403, 403, &armed, &a);
  CHECK_EQ(r.no_packet, 2); /* 401, 402 */
  CHECK_EQ(out.errors, 1);

  /* Equal counts as behind: armed for P's own parity after a late arm */
  open_out(4, 499, &odd);
  r = packet(500, 500, 503, &armed, &a);
  CHECK_EQ(a.odd, false); /* parity(504) */
  CHECK(!u2ps_dwc2_out_complete(&out, 501, false, &r)); /* gives 500 */
  CHECK_EQ(out.state, U2PS_DWC2_OUT_ARMED);

  /* Two behind is behind; four behind is a forward gap of 0x3FFC */
  open_out(4, 599, &odd);
  r = packet(600, 600, 603, &armed, &a); /* LATE 601..603, armed for 604 */
  CHECK(!u2ps_dwc2_out_complete(&out, 598, false, &r));
  CHECK(u2ps_dwc2_out_complete(&out, 596, false, &r));
  CHECK_EQ(r.uframe, 596);
  CHECK_EQ(r.no_packet, 0x3FFB - 3); /* the gap less the LATE microframes */
  CHECK_EQ(out.errors, 0);

  /* Across the wrap, a packet after the last is not behind */
  open_out(4, 0x3FFE, &odd);
  r = packet(0x3FFF, 0x3FFF, 0x3FFF, &armed, &a);
  CHECK(u2ps_dwc2_out_complete(&out, 0x0001, false, &r));
  CHECK_EQ(r.uframe, 0x0000);
  CHECK_EQ(r.no_packet, 0);
}

/* Gaps of more than 8192 microframes are gaps, not steps back */
static void test_out_long_gaps(void)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  /* A host pause of 9000 microframes */
  open_out(4, 99, &odd);
  r = packet(100, 100, 100, &armed, &a);
  u2ps_dwc2_out_release(&out, 1);
  r = packet(9101, 9101, 9101, &armed, &a);
  CHECK_EQ(r.no_packet, 9000);
  u2ps_dwc2_out_release(&out, 1);
  unsigned holes = 0;
  for (long p = 9102; p <= 10101; p++) {
    r = packet(p, p, p, &armed, &a);
    holes += r.no_packet + r.late + r.no_buffer;
    u2ps_dwc2_out_release(&out, 1);
  }
  CHECK_EQ(holes, 0);
  CHECK_EQ(out.errors, 0);
  /* A short hole afterwards */
  r = packet(10104, 10104, 10104, &armed, &a);
  CHECK_EQ(r.no_packet, 2);
  CHECK_EQ(out.errors, 0);

  /* The stack holds all it may for 9000 microframes while the host sends
   * in every one: 9000 reserve packets, NO_BUFFER 9000 */
  open_out(2, 199, &odd);
  r = packet(200, 200, 200, &armed, &a);
  unsigned bad = 0;
  for (long p = 201; p <= 9200; p++) {
    r = packet(p, p, p, &armed, &a);
    bad += r.buf != U2PS_DWC2_NO_BUF || !armed;
  }
  CHECK_EQ(bad, 0);
  CHECK_EQ(out.no_buffer, 9000);
  CHECK_EQ(out.late, 0);
  u2ps_dwc2_out_release(&out, 1);
  r = packet(9201, 9201, 9201, &armed, &a);
  CHECK_EQ(r.buf, 1);
  CHECK_EQ(r.no_buffer, 9000);
  CHECK_EQ(r.no_packet, 0);
  CHECK_EQ(r.late, 0);
  CHECK_EQ(out.errors, 0);
}

/* A 'now' behind the completion cannot come from the hardware */
static void test_out_now_clamp(void)
{
  bool odd;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  open_out(4, 999, &odd);
  CHECK(u2ps_dwc2_out_complete(&out, 1000, false, &r));
  CHECK(u2ps_dwc2_out_arm(&out, 999, &a));
  CHECK_EQ(out.errors, 1);
  CHECK_EQ(a.odd, true); /* parity(1001), as for now = P */
  CHECK_EQ(a.late, 0);
  CHECK_EQ(out.late_completions, 0);
  CHECK(u2ps_dwc2_out_complete(&out, 1001, false, &r));
  CHECK(u2ps_dwc2_out_arm(&out, 999, &a)); /* two behind */
  CHECK_EQ(out.errors, 2);
  CHECK_EQ(a.odd, false); /* parity(1002) */
  CHECK_EQ(a.late, 0);
}

/* The reserve: the port keeps U2PS_DWC2_OUT_RESERVE buffer armed, so the
 * stack holds at most count - 1. A packet that would take the last free
 * buffer is a reserve packet: not delivered, NO_BUFFER in its own
 * microframe, its buffer armed again. A release only frees; the
 * NO_BUFFER microframes come in the next packet's record. */
static void test_out_release(void)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  CHECK_EQ(U2PS_DWC2_OUT_RESERVE, 1);
  CHECK_EQ(U2PS_DPORT_ISO_OUT_RESERVE, U2PS_DWC2_OUT_RESERVE);
  CHECK_EQ(U2PS_DPORT_ISO_OUT_MIN, U2PS_DPORT_ISO_OUT_RESERVE + 1u);
  CHECK(!u2ps_dwc2_out_open(&out, U2PS_DWC2_OUT_RESERVE, 0, &odd));

  /* Two buffers, each released inside its upcall: nothing lost */
  open_out(2, 9, &odd);
  r = packet(10, 10, 10, &armed, &a);
  CHECK_EQ(r.buf, 0);
  CHECK_EQ(r.arm_buf, 1);
  CHECK(armed);
  u2ps_dwc2_out_release(&out, 1);
  r = packet(11, 11, 11, &armed, &a);
  CHECK_EQ(r.buf, 1);
  CHECK_EQ(r.arm_buf, 0);
  CHECK(armed);

  /* Held: the packet of 12 is a reserve packet, and so are 13 and 14 */
  r = packet(12, 12, 12, &armed, &a);
  CHECK_EQ(r.buf, U2PS_DWC2_NO_BUF);
  CHECK_EQ(r.arm_buf, 0); /* the same buffer again */
  CHECK(armed);
  CHECK_EQ(a.odd, true); /* parity(13) */
  CHECK_EQ(a.late, 0);
  CHECK_EQ(r.late + r.no_buffer + r.no_packet, 0); /* nothing reported now */
  CHECK_EQ(out.no_buffer, 1);
  CHECK_EQ(out.held, 1);
  CHECK_EQ(out.state, U2PS_DWC2_OUT_ARMED);
  r = packet(13, 13, 13, &armed, &a);
  CHECK_EQ(r.buf, U2PS_DWC2_NO_BUF);
  CHECK_EQ(r.arm_buf, 0);
  r = packet(14, 14, 14, &armed, &a);
  CHECK(armed);
  CHECK_EQ(out.no_buffer, 3);
  CHECK_EQ(out.no_buffer_total, 3);
  /* A release in thread: bookkeeping only, nothing to arm */
  u2ps_dwc2_out_release(&out, 1);
  CHECK_EQ(out.held, 0);
  CHECK_EQ(out.state, U2PS_DWC2_OUT_ARMED);
  CHECK_EQ(out.no_buffer, 3);
  r = packet(15, 15, 15, &armed, &a);
  CHECK_EQ(r.buf, 0);
  CHECK_EQ(r.arm_buf, 1);
  CHECK_EQ(r.no_buffer, 3);
  CHECK_EQ(r.no_packet, 0);
  CHECK_EQ(r.late, 0);
  /* Reported once */
  u2ps_dwc2_out_release(&out, 1);
  r = packet(16, 16, 16, &armed, &a);
  CHECK_EQ(r.no_buffer + r.late + r.no_packet, 0);
  CHECK_EQ(out.late_completions, 0);
  CHECK_EQ(out.late_total, 0);
  CHECK_EQ(out.no_buffer_total, 3);
  CHECK_EQ(out.errors, 0);

  /* Releases that do nothing */
  u2ps_dwc2_out_release(&out, 0);
  CHECK_EQ(out.errors, 0);
  u2ps_dwc2_out_release(&out, 3); /* only 1 held */
  CHECK_EQ(out.held, 1);
  CHECK_EQ(out.errors, 1);
  u2ps_dwc2_out_close(&out);
  u2ps_dwc2_out_release(&out, 1);
  CHECK_EQ(out.errors, 2);
}

/* A host gap of 500 microframes inside the stall: the reserve packets
 * before and after it are NO_BUFFER, the gap stays NO_PACKET */
static void test_out_reserve_gap(void)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  open_out(3, 999, &odd);
  r = packet(1000, 1000, 1000, &armed, &a);
  CHECK_EQ(r.buf, 0);
  r = packet(1001, 1001, 1001, &armed, &a);
  CHECK_EQ(r.buf, 1);
  CHECK_EQ(r.arm_buf, 2);
  unsigned bad = 0;
  for (long p = 1002; p <= 1003; p++) {
    r = packet(p, p, p, &armed, &a);
    bad += r.buf != U2PS_DWC2_NO_BUF || r.arm_buf != 2u || !armed;
  }
  /* The host sends nothing in 1004..1503 */
  for (long p = 1504; p <= 1506; p++) {
    r = packet(p, p, p, &armed, &a);
    bad += r.buf != U2PS_DWC2_NO_BUF || r.arm_buf != 2u || !armed;
  }
  CHECK_EQ(bad, 0);
  CHECK_EQ(out.no_buffer, 5);
  CHECK_EQ(out.no_packet, 500);
  CHECK_EQ(out.late, 0);
  u2ps_dwc2_out_release(&out, 2);
  r = packet(1507, 1507, 1507, &armed, &a);
  CHECK_EQ(r.buf, 2);
  CHECK_EQ(r.no_buffer, 5);
  CHECK_EQ(r.no_packet, 500);
  CHECK_EQ(r.late, 0);
  CHECK_EQ(out.no_buffer_total, 5);
  CHECK_EQ(out.late_completions, 0);
  CHECK_EQ(out.errors, 0);
}

/* The arm after a reserve packet read two microframes late: B-next from
 * the reserve packet's own microframe, and its LATE microframes recorded
 * for the next packet's record */
static void test_out_reserve_late(void)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  open_out(2, 9, &odd);
  r = packet(10, 10, 10, &armed, &a);
  r = packet(11, 11, 13, &armed, &a);
  CHECK_EQ(r.buf, U2PS_DWC2_NO_BUF);
  CHECK(armed);
  CHECK_EQ(a.odd, false); /* parity(14) */
  CHECK_EQ(a.late, 0);    /* recorded, not reported after a packet */
  CHECK_EQ(out.late, 2);  /* 12, 13 */
  CHECK_EQ(out.no_buffer, 1);
  CHECK_EQ(out.late_completions, 1);
  CHECK_EQ(out.late_total, 2);
  u2ps_dwc2_out_release(&out, 1);
  r = packet(14, 14, 14, &armed, &a);
  CHECK_EQ(r.buf, 1);
  CHECK_EQ(r.late, 2);
  CHECK_EQ(r.no_buffer, 1);
  CHECK_EQ(r.no_packet, 0);
  /* In time after a reserve packet: armed for P + 1 */
  r = packet(15, 15, 15, &armed, &a);
  CHECK_EQ(r.buf, U2PS_DWC2_NO_BUF);
  CHECK_EQ(a.odd, false); /* parity(16) */
  CHECK_EQ(out.late, 0);
  r = packet(16, 16, 16, &armed, &a);
  CHECK_EQ(out.no_buffer, 2);
  CHECK_EQ(out.late_total, 2);
  CHECK_EQ(out.errors, 0);
}

/* Stalls longer than the microframe numbers' wrap: each reserve packet
 * moves the base, so nothing aliases, and the counts do not overflow 16
 * bits */
static void test_out_reserve_long(void)
{
  static const long ns[] = {20000, 70000};
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  for (unsigned i = 0; i < 2u; i++) {
    long n = ns[i];
    /* Every microframe */
    open_out(2, 99, &odd);
    r = packet(100, 100, 100, &armed, &a);
    unsigned bad = 0;
    for (long p = 101; p <= 100 + n; p++) {
      r = packet(p, p, p, &armed, &a);
      bad += r.buf != U2PS_DWC2_NO_BUF || !armed;
    }
    CHECK_EQ(bad, 0);
    u2ps_dwc2_out_release(&out, 1);
    r = packet(101 + n, 101 + n, 101 + n, &armed, &a);
    CHECK_EQ(r.buf, 1);
    CHECK_EQ(r.no_buffer, n);
    CHECK_EQ(r.no_packet, 0);
    CHECK_EQ(r.late, 0);
    CHECK_EQ(out.no_buffer_total, n);
    CHECK_EQ(out.errors, 0);
    /* Every third microframe: two NO_PACKET before each */
    open_out(2, 99, &odd);
    r = packet(100, 100, 100, &armed, &a);
    bad = 0;
    for (long k = 1; k <= n; k++) {
      long p = 100 + 3 * k;
      r = packet(p, p, p, &armed, &a);
      bad += r.buf != U2PS_DWC2_NO_BUF || !armed;
    }
    CHECK_EQ(bad, 0);
    u2ps_dwc2_out_release(&out, 1);
    long p = 100 + 3 * (n + 1);
    r = packet(p, p, p, &armed, &a);
    CHECK_EQ(r.buf, 1);
    CHECK_EQ(r.no_buffer, n);
    CHECK_EQ(r.no_packet, 2 * n + 2);
    CHECK_EQ(r.late, 0);
    CHECK_EQ(out.errors, 0);
  }
}

/* The stream's end: what is recorded after the last delivered packet, up
 * to the last packet received, is the tail, taken once */
static void test_out_reserve_tail(void)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r, t;

  /* Three delivered; reserve packets in 2003 and, after two microframes
   * without a packet, in 2006, armed late in 2009 */
  open_out(4, 1999, &odd);
  for (long p = 2000; p <= 2002; p++)
    r = packet(p, p, p, &armed, &a);
  CHECK_EQ(out.held, 3);
  r = packet(2003, 2003, 2003, &armed, &a);
  CHECK_EQ(r.buf, U2PS_DWC2_NO_BUF);
  r = packet(2006, 2006, 2009, &armed, &a);
  CHECK_EQ(r.buf, U2PS_DWC2_NO_BUF);
  /* A close begins; the tail before on_closed */
  u2ps_dwc2_out_closing(&out);
  memset(&t, 0xA5, sizeof t);
  u2ps_dwc2_out_tail(&out, &t);
  CHECK_EQ(t.no_buffer, 2);
  CHECK_EQ(t.no_packet, 2);
  CHECK_EQ(t.late, 3);
  CHECK_EQ(t.buf, U2PS_DWC2_NO_BUF);
  CHECK_EQ(t.arm_buf, U2PS_DWC2_NO_BUF);
  /* Taken once */
  u2ps_dwc2_out_tail(&out, &t);
  CHECK_EQ(t.no_buffer + t.no_packet + t.late, 0);
  u2ps_dwc2_out_close(&out);

  /* Without a close (a bus reset, a detach): the same */
  open_out(2, 2999, &odd);
  r = packet(3000, 3000, 3000, &armed, &a);
  r = packet(3001, 3001, 3001, &armed, &a);
  r = packet(3002, 3002, 3002, &armed, &a);
  u2ps_dwc2_out_tail(&out, &t);
  CHECK_EQ(t.no_buffer, 2);
  CHECK_EQ(t.no_packet + t.late, 0);
  CHECK_EQ(out.no_buffer_total, 2);

  /* A dropped packet after the last delivered one, re-armed late: its
   * microframe is NO_PACKET, the late ones LATE */
  open_out(4, 3999, &odd);
  r = packet(4000, 4000, 4000, &armed, &a);
  CHECK_EQ(u2ps_dwc2_out_drop(&out, 4001), 1);
  CHECK(u2ps_dwc2_out_arm(&out, 4004, &a));
  u2ps_dwc2_out_tail(&out, &t);
  CHECK_EQ(t.no_packet, 1);
  CHECK_EQ(t.late, 3);
  CHECK_EQ(t.no_buffer, 0);

  /* Nothing after the last delivered packet, and nothing before the
   * first: no tail */
  open_out(4, 4999, &odd);
  u2ps_dwc2_out_tail(&out, &t);
  CHECK_EQ(t.no_buffer + t.no_packet + t.late, 0);
  CHECK_EQ(u2ps_dwc2_out_drop(&out, 5000), 0);
  CHECK(u2ps_dwc2_out_arm(&out, 5003, &a));
  u2ps_dwc2_out_tail(&out, &t);
  CHECK_EQ(t.no_buffer + t.no_packet + t.late, 0);
  r = packet(5004, 5004, 5004, &armed, &a);
  r = packet(5005, 5005, 5007, &armed, &a); /* LATE reported after it */
  CHECK_EQ(a.late, 2);
  u2ps_dwc2_out_tail(&out, &t);
  CHECK_EQ(t.no_buffer + t.no_packet + t.late, 0);
  CHECK_EQ(out.errors, 1); /* the drop */
}

/* The pending holes taken in the middle of a stream, as at a long gap
 * (u2ps_dport_report_holes): u2ps_dwc2_out_tail() takes what is recorded
 * after the last delivered packet, and the next packet's record holds only
 * what came after; with both, every microframe since that packet counts
 * once */
static void test_out_pending(void)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r, t;

  open_out(3, 999, &odd);
  r = packet(1000, 1000, 1000, &armed, &a);
  r = packet(1001, 1001, 1001, &armed, &a);
  CHECK_EQ(out.held, 2);
  /* Reserve packets in 1002, 1003 and, after six microframes without a
   * packet, in 1010, armed late in 1012 */
  r = packet(1002, 1002, 1002, &armed, &a);
  r = packet(1003, 1003, 1003, &armed, &a);
  r = packet(1010, 1010, 1012, &armed, &a);
  CHECK_EQ(r.buf, U2PS_DWC2_NO_BUF);
  memset(&t, 0xA5, sizeof t);
  u2ps_dwc2_out_tail(&out, &t);
  CHECK_EQ(t.no_buffer, 3);
  CHECK_EQ(t.no_packet, 6);
  CHECK_EQ(t.late, 2);
  CHECK_EQ(t.uframe, uf(1010));
  u2ps_dwc2_out_tail(&out, &t);
  CHECK_EQ(t.no_buffer + t.no_packet + t.late, 0);
  /* The stream goes on: nothing in 1013..1100, a reserve packet in 1101,
   * then a release and a packet in 1102 */
  r = packet(1101, 1101, 1101, &armed, &a);
  CHECK_EQ(r.buf, U2PS_DWC2_NO_BUF);
  u2ps_dwc2_out_release(&out, 2);
  r = packet(1102, 1102, 1102, &armed, &a);
  CHECK_EQ(r.buf, 2);
  CHECK_EQ(r.no_buffer, 1);
  CHECK_EQ(r.no_packet, 88);
  CHECK_EQ(r.late, 0);
  CHECK_EQ(3u + 6u + 2u + r.no_buffer + r.no_packet + r.late, 1102u - 1001u - 1u);
  u2ps_dwc2_out_tail(&out, &t);
  CHECK_EQ(t.no_buffer + t.no_packet + t.late, 0);
  CHECK_EQ(out.no_buffer_total, 4);
  CHECK_EQ(out.errors, 0);
}

/* Calls in states the hardware or the register layer cannot produce */
static void test_out_bad_states(void)
{
  bool odd;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  open_out(2, 699, &odd);
  CHECK(u2ps_dwc2_out_complete(&out, 700, false, &r));
  CHECK_EQ(out.state, U2PS_DWC2_OUT_COMPLETED);
  /* A release while the arm decision is pending */
  u2ps_dwc2_out_release(&out, 1);
  CHECK_EQ(out.errors, 1);
  CHECK_EQ(out.held, 1);
  /* A second completion with nothing armed */
  CHECK(u2ps_dwc2_out_complete(&out, 701, false, &r));
  CHECK_EQ(r.buf, U2PS_DWC2_NO_BUF);
  CHECK_EQ(r.arm_buf, U2PS_DWC2_NO_BUF);
  CHECK_EQ(out.errors, 2);
  CHECK_EQ(out.held, 1);
  CHECK(u2ps_dwc2_out_arm(&out, 700, &a));
  /* The stack holds all it may: a reserve packet, whose arm decision is
   * pending as after a drop; a buffer is always armed after it */
  CHECK(u2ps_dwc2_out_complete(&out, 701, false, &r));
  CHECK_EQ(r.buf, U2PS_DWC2_NO_BUF);
  CHECK_EQ(r.arm_buf, 1);
  u2ps_dwc2_out_release(&out, 1);
  CHECK_EQ(out.errors, 3);
  CHECK_EQ(out.held, 1);
  CHECK(u2ps_dwc2_out_arm(&out, 701, &a));
  CHECK_EQ(out.state, U2PS_DWC2_OUT_ARMED);
  /* An arm decision with nothing completed arms nothing */
  CHECK(!u2ps_dwc2_out_arm(&out, 702, &a));
  CHECK_EQ(out.errors, 3);
  u2ps_dwc2_out_close(&out);
  CHECK(u2ps_dwc2_out_complete(&out, 703, false, &r));
  CHECK_EQ(r.buf, U2PS_DWC2_NO_BUF);
  CHECK_EQ(r.arm_buf, U2PS_DWC2_NO_BUF);
  CHECK_EQ(out.errors, 4);
  CHECK(!u2ps_dwc2_out_arm(&out, 703, &a));
  CHECK_EQ(u2ps_dwc2_out_drop(&out, 704), U2PS_DWC2_NO_BUF);
  CHECK_EQ(out.errors, 5);
}

/* A packet that must not be delivered: the same buffer is armed again,
 * and its microframe joins the hole */
static void test_out_drop(void)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  open_out(4, 799, &odd);
  r = packet(800, 800, 800, &armed, &a); /* armed buffer 1 for 801 */
  CHECK_EQ(u2ps_dwc2_out_drop(&out, 801), 1);
  CHECK_EQ(out.errors, 1);
  CHECK_EQ(out.held, 1);
  CHECK(u2ps_dwc2_out_arm(&out, 801, &a));
  CHECK_EQ(a.odd, false); /* parity(802) */
  CHECK_EQ(a.late, 0);
  r = packet(802, 802, 802, &armed, &a);
  CHECK_EQ(r.buf, 1);
  CHECK_EQ(r.no_packet, 1); /* 801 */

  /* Dropped in 803, re-armed late in 806: 803 NO_PACKET, 804..806 LATE */
  CHECK_EQ(u2ps_dwc2_out_drop(&out, 803), 2);
  CHECK(u2ps_dwc2_out_arm(&out, 806, &a));
  CHECK_EQ(a.odd, true); /* parity(807) */
  CHECK_EQ(a.late, 0);   /* recorded */
  CHECK_EQ(out.late_completions, 1);
  r = packet(807, 807, 807, &armed, &a);
  CHECK_EQ(r.buf, 2);
  CHECK_EQ(r.late, 3);
  CHECK_EQ(r.no_packet, 1);
  CHECK_EQ(out.late_total, 3);
  CHECK_EQ(out.errors, 2);

  /* Before the first packet: nothing is reported */
  open_out(4, 899, &odd);
  CHECK_EQ(u2ps_dwc2_out_drop(&out, 900), 0);
  CHECK(u2ps_dwc2_out_arm(&out, 905, &a));
  CHECK_EQ(a.odd, false); /* parity(906) */
  r = packet(906, 906, 906, &armed, &a);
  CHECK_EQ(r.buf, 0);
  CHECK_EQ(r.late + r.no_packet + r.no_buffer, 0);
}

/* A close: completions drop, releases free but never arm */
static void test_out_closing(void)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  open_out(4, 899, &odd);
  r = packet(900, 900, 900, &armed, &a);
  u2ps_dwc2_out_closing(&out);
  CHECK(u2ps_dwc2_out_complete(&out, 901, false, &r));
  CHECK_EQ(r.buf, U2PS_DWC2_NO_BUF);
  CHECK_EQ(r.arm_buf, U2PS_DWC2_NO_BUF);
  CHECK_EQ(out.close_drops, 1);
  CHECK_EQ(u2ps_dwc2_out_drop(&out, 902), U2PS_DWC2_NO_BUF);
  CHECK_EQ(out.close_drops, 2);
  CHECK(!u2ps_dwc2_out_arm(&out, 903, &a));
  u2ps_dwc2_out_release(&out, 1);
  CHECK_EQ(out.held, 0);
  CHECK_EQ(out.errors, 0);
  /* Packets dropped in the close are no part of the tail */
  u2ps_dwc2_out_tail(&out, &r);
  CHECK_EQ(r.no_buffer + r.no_packet + r.late, 0);
  u2ps_dwc2_out_close(&out);
  CHECK_EQ(out.state, U2PS_DWC2_OUT_CLOSED);

  /* The stack holds all it may when the close begins: a release frees and
   * arms nothing; the reserve packet is the tail */
  open_out(2, 999, &odd);
  r = packet(1000, 1000, 1000, &armed, &a);
  r = packet(1001, 1001, 1001, &armed, &a);
  CHECK_EQ(r.buf, U2PS_DWC2_NO_BUF);
  CHECK(armed);
  u2ps_dwc2_out_closing(&out);
  u2ps_dwc2_out_release(&out, 1);
  CHECK_EQ(out.held, 0);
  CHECK_EQ(out.state, U2PS_DWC2_OUT_CLOSING);
  CHECK(!u2ps_dwc2_out_arm(&out, 1003, &a));
  u2ps_dwc2_out_tail(&out, &r);
  CHECK_EQ(r.no_buffer, 1);
  CHECK_EQ(r.no_packet + r.late, 0);
  CHECK_EQ(out.errors, 0);
}

/* The data endpoint's parity follows the bus from the open to the first
 * XFRC: each EOPF writes parity(now + 1), the packet's P is taken with it,
 * and a write that lands in the microframe it was meant for counts in
 * follow_late. A completion, a drop, a close or the follow turned off end
 * it; no time limit does (test_out_follow_late_packet). */
static void test_out_follow(void)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  open_out(4, 99, &odd); /* armed for 100: even */
  CHECK(out.following);
  CHECK(u2ps_dwc2_out_eopf(&out, uf(100), &odd));
  CHECK_EQ(odd, true);
  CHECK_EQ(out.armed_odd, true);
  u2ps_dwc2_out_eopf_after(&out, uf(100));
  CHECK_EQ(out.follow_late, 0);
  CHECK(u2ps_dwc2_out_eopf(&out, uf(101), &odd));
  CHECK_EQ(odd, false);
  u2ps_dwc2_out_eopf_after(&out, uf(102)); /* landed in 102 */
  CHECK_EQ(out.follow_late, 1);
  CHECK_EQ(out.follows, 2);
  /* The first packet, in 102: P from the followed parity */
  r = packet(102, 102, 102, &armed, &a);
  CHECK_EQ(r.no_packet, 0);
  CHECK(!out.following);
  CHECK(!u2ps_dwc2_out_eopf(&out, uf(103), &odd));
  CHECK_EQ(out.follows, 2);

  /* A drop ends it too */
  open_out(4, 200, &odd);
  CHECK_EQ(u2ps_dwc2_out_drop(&out, uf(201)), 0);
  CHECK(u2ps_dwc2_out_arm(&out, uf(201), &a));
  CHECK(!u2ps_dwc2_out_eopf(&out, uf(202), &odd));
  /* So does a close, and the follow turned off */
  open_out(4, 300, &odd);
  u2ps_dwc2_out_closing(&out);
  CHECK(!u2ps_dwc2_out_eopf(&out, uf(301), &odd));
  open_out(4, 300, &odd);
  out.following = false;
  CHECK(!u2ps_dwc2_out_eopf(&out, uf(301), &odd));
  CHECK_EQ(out.armed_odd, true); /* parity(301), as armed */

  /* Across the wrap of the microframe numbers */
  open_out(4, 0x3FFD, &odd);
  CHECK(u2ps_dwc2_out_eopf(&out, uf(0x3FFE), &odd));
  CHECK_EQ(odd, true); /* parity(0x3FFF) */
  CHECK(u2ps_dwc2_out_eopf(&out, uf(0x3FFF), &odd));
  CHECK_EQ(odd, false); /* parity(0x0000) */
  u2ps_dwc2_out_eopf_after(&out, uf(0x3FFF));
  r = packet(0x4000, 0x4000, 0x4000, &armed, &a);
  CHECK_EQ(out.errors, 0);
}

/* The follow has no time limit: a first packet 240 ms (1,920 microframes)
 * after the open, as Linux may send it at high rates, and one 20,000
 * microframes after it (past the wrap of the microframe numbers), finds
 * its parity armed, and that packet still ends the follow */
static void test_out_follow_late_packet(void)
{
  static const long after[] = {1920, 1921, 20000};
  for (unsigned i = 0; i < sizeof after / sizeof after[0]; i++) {
    for (long open_at = 1000; open_at <= 1001; open_at++) {
      bool odd, armed;
      u2ps_dwc2_bnext_t a;
      u2ps_dwc2_out_packet_t r;
      long first = open_at + after[i];
      unsigned stopped = 0, wrong = 0;
      open_out(4, open_at, &odd);
      for (long m = open_at + 1; m < first; m++) {
        if (!u2ps_dwc2_out_eopf(&out, uf(m), &odd)) {
          stopped++;
          continue;
        }
        if (odd != odd_of(m + 1))
          wrong++;
        u2ps_dwc2_out_eopf_after(&out, uf(m));
      }
      CHECK_EQ(stopped, 0);
      CHECK_EQ(wrong, 0);
      CHECK(out.following);
      CHECK_EQ(out.follows, (uint32_t)after[i] - 1u);
      CHECK_EQ(out.follow_late, 0);
      /* The first packet, in 'first': P from the followed parity */
      r = packet(first, first, first, &armed, &a);
      CHECK_EQ(r.buf, 0);
      CHECK_EQ(r.no_packet, 0);
      CHECK(armed);
      CHECK_EQ(out.errors, 0);
      /* ... and the follow is over */
      CHECK(!out.following);
      CHECK(!u2ps_dwc2_out_eopf(&out, uf(first + 1), &odd));
      CHECK_EQ(out.follows, (uint32_t)after[i] - 1u);
    }
  }
}

/* ---- Gaps of the host: the parity at INCOMPISOOUT -------------------------- */

/* A model of the core and of the register layer (u2ps_dwc2_dcd.c) around
 * the logic, one microframe per step.
 * The core:
 * - receives the host's packet in microframe m only if the endpoint is
 *   enabled, armed for parity(m) (EONUM = FNSOF[0]) and its NAK is clear;
 *   a received packet sets NAK as it enters the Rx FIFO (PKTCNT reaches
 *   0), and only the re-arm's CNAK clears it (RM0456 73.15.6, p. 3444,
 *   internal data flow 2 and 3, and p. 3445, isochronous flow 1); a
 *   corrupted packet (crc_next) is flushed unseen, leaves NAK clear and
 *   the endpoint enabled (p. 3444, flow 2; p. 3446);
 * - completes a received packet in its microframe (EPENA cleared, XFRC
 *   set), or with late_next only after the EOPF point: then INCOMPISOOUT is
 *   raised for it at that EOPF, with EPENA still set, and its XFRC comes at
 *   the start of m + 1 (measured at 705.6 and 768 kHz: 0.01 to 0.03 % of
 *   the packets complete 117 to 121 us into their microframe, against
 *   PFIVL 95 %, 118.75 us); with same_pass the handler serves that
 *   INCOMPISOOUT only in the pass of the XFRC, after it;
 * - raises INCOMPISOOUT at an EOPF at which the endpoint is enabled, armed
 *   for parity(m) and has not completed a packet, or still drains one; with
 *   every_eopf, at every EOPF at which the endpoint is enabled;
 * - latches XFRC, INCOMPISOOUT and EOPF while the handler cannot run (from
 *   the EOPF of block_from to the start of block_until, before its token):
 *   then one pass serves them all, reading FNSOF as that microframe.
 * The pass, as irq_pass(): the data endpoint's XFRC first (complete with
 * FNSOF read as now, re-arm with now), then INCOMPISOOUT (idle: EPENA set,
 * NAKSTS clear, no XFRC served in this pass), then EOPF for a follow (no
 * data write while NAKSTS is set, nor in the pass that started the
 * follow). The app releases each packet
 * at once unless it holds them all; in a gap of more than 400 microframes
 * (U2PS_GAP_PAUSE) the device stack's tick takes the holes held back
 * (u2ps_dport_report_holes) every 8 microframes. */
typedef struct {
  long m;                /* the current microframe */
  /* The core */
  bool enabled, odd;     /* EPENA and EONUM of the data endpoint */
  bool xfrc;             /* OTG_DOEPINTx.XFRC pending */
  bool nak;              /* OTG_DOEPCTLx.NAKSTS */
  bool draining;         /* a received packet completes after this EOPF */
  long rx_m;             /* the received packet's microframe */
  bool incomp_bit, eopf_bit; /* OTG_GINTSTS, latched */
  /* Switches */
  bool late_next;        /* the next received packet completes after its EOPF */
  bool same_pass;        /* ... and its INCOMPISOOUT is served with its XFRC */
  bool every_eopf;       /* INCOMPISOOUT at every EOPF while enabled */
  bool crc_next;         /* the next packet the host sends is corrupted */
  long block_from, block_until;
  bool late_arm;         /* the next re-arm comes in P + 1, after its token */
  bool pending_arm;
  bool hold;             /* the app holds every packet */
  /* The register layer */
  bool eopf_out;         /* EOPF unmasked for a follow */
  bool incomp_on;        /* INCOMPISOOUT unmasked */
  bool started;
  long last_rx;
  unsigned long sent, received, delivered, lost, corrupted, incomp, holes, stamps, passes;
} bus_t;

static bus_t bus;

static void bus_open(long at, bool follow)
{
  bool odd;
  open_out(16, at, &odd);
  memset(&bus, 0, sizeof bus);
  out.following = follow; /* as u2ps_dport_stream_open() */
  bus.m = at + 1;
  bus.enabled = true;
  bus.odd = odd;
  bus.eopf_out = follow;
  bus.incomp_on = follow;
  bus.block_from = bus.block_until = -1;
}

static void bus_arm(long now)
{
  u2ps_dwc2_bnext_t a;
  bus.enabled = u2ps_dwc2_out_arm(&out, uf(now), &a);
  if (bus.enabled)
    bus.nak = false; /* EPENA with CNAK */
  bus.odd = a.odd;
  bus.holes += a.late;
  if (!bus.hold && out.held > 0u)
    u2ps_dwc2_out_release(&out, out.held);
}

static void bus_tail(void)
{
  u2ps_dwc2_out_packet_t t;
  u2ps_dwc2_out_tail(&out, &t);
  bus.holes += t.late + t.no_buffer + t.no_packet;
}

/* The register layer at INCOMPISOOUT (out_incomplete() of
 * u2ps_dwc2_dcd.c) */
static bool reg_incomplete(long fn, bool served)
{
  bool idle = !served && bus.enabled && !bus.nak;
  bool odd;
  if (u2ps_dwc2_out_incomplete(&out, uf(fn), idle, &odd)) {
    bus.odd = odd;
    u2ps_dwc2_out_eopf_after(&out, uf(fn));
    bus.eopf_bit = false; /* eopf_set(true) clears it, then unmasks */
    bus.eopf_out = true;
    return true;
  }
  return false;
}

/* One pass of the handler, FNSOF read as fn */
static void bus_pass(long fn)
{
  bool served = false, started = false;
  bool odd;

  bus.passes++;
  if (bus.xfrc) {
    u2ps_dwc2_out_packet_t r;
    bus.xfrc = false;
    served = true;
    if (!u2ps_dwc2_out_complete(&out, uf(fn), false, &r))
      CHECK(u2ps_dwc2_out_complete(&out, uf(fn), true, &r));
    if (r.uframe != uf(bus.rx_m))
      bus.stamps++;
    if (r.buf != U2PS_DWC2_NO_BUF)
      bus.delivered++;
    bus.holes += r.late + r.no_buffer + r.no_packet;
    if (bus.late_arm) {
      bus.late_arm = false;
      bus.pending_arm = true;
    } else {
      bus_arm(fn);
    }
  }
  if (bus.incomp_bit && bus.incomp_on) {
    bus.incomp_bit = false;
    started = reg_incomplete(fn, served);
  }
  if (bus.eopf_bit && bus.eopf_out) {
    bus.eopf_bit = false;
    if (!out.following) {
      bus.eopf_out = false; /* the follow ended at an XFRC */
    } else if (bus.enabled && !bus.nak && !started && u2ps_dwc2_out_eopf(&out, uf(fn), &odd)) {
      bus.odd = odd;
      u2ps_dwc2_out_eopf_after(&out, uf(fn));
    }
  }
}

static bool bus_blocked(bool at_eopf)
{
  long m = bus.m;
  return (m == bus.block_from && at_eopf) || (m > bus.block_from && m < bus.block_until);
}

static bool bus_irq(void)
{
  return bus.xfrc || (bus.incomp_bit && bus.incomp_on) || (bus.eopf_bit && bus.eopf_out);
}

/* One microframe; the host sends a packet in it if 'send' */
static void bus_step(bool send)
{
  long m = bus.m;

  /* After the SOF: a packet completing late, a blocked handler's pass */
  if (bus.draining) {
    bus.draining = false;
    bus.enabled = false;
    bus.xfrc = true;
  }
  if (!bus_blocked(false) && bus_irq())
    bus_pass(m);
  /* The token */
  if (send) {
    bus.sent++;
    if (bus.crc_next) {
      bus.crc_next = false;
      bus.corrupted++;
    } else if (bus.enabled && bus.odd == odd_of(m) && !bus.nak) {
      bus.nak = true; /* PKTCNT 0: NAK set as the packet enters the Rx FIFO */
      bus.received++;
      bus.rx_m = m;
      bus.started = true;
      bus.last_rx = m;
      if (bus.late_next) {
        bus.late_next = false;
        bus.draining = true;
        if (bus.same_pass) {
          bus.same_pass = false;
          bus.block_from = m;
          bus.block_until = m + 1;
        }
      } else {
        bus.enabled = false;
        bus.xfrc = true;
      }
    } else {
      bus.lost++;
    }
  }
  if (bus.pending_arm) {
    bus.pending_arm = false;
    bus_arm(m);
  }
  if (!bus_blocked(false) && bus_irq())
    bus_pass(m);
  /* The EOPF of m */
  if (bus.draining || (bus.enabled && (bus.every_eopf || bus.odd == odd_of(m)))) {
    bus.incomp++;
    bus.incomp_bit = true;
  }
  bus.eopf_bit = true;
  if (!bus_blocked(true) && bus_irq())
    bus_pass(m);
  /* The tick in a long gap */
  if (bus.started && m - bus.last_rx > 400 && m % 8 == 0)
    bus_tail();
  bus.m = m + 1;
}

static void bus_send(long n)
{
  for (long i = 0; i < n; i++)
    bus_step(true);
}

static void bus_quiet(long n)
{
  for (long i = 0; i < n; i++)
    bus_step(false);
}

/* The host's first packet after a gap of g microframes, both parities of g,
 * under and over the pause (400 microframes, 50 ms), at the lengths
 * measured on the boards (20 to 200 ms) and past half the wrap: received
 * in its own microframe, the hole the gap, once INCOMPISOOUT of the armed
 * parity has come twice without a packet between (g of 3 or more). After
 * a single skipped microframe (g = 1) the next packet is lost, as in
 * release 0.1.0: the one INCOMPISOOUT of the gap cannot be told from that
 * of a packet still completing. */
static void test_out_gap_resume(void)
{
  static const long gaps[] = {1, 2, 3, 4, 5, 159, 160, 399, 400, 401, 402, 1599, 1600, 9000, 9001};
  for (unsigned i = 0; i < sizeof gaps / sizeof gaps[0]; i++) {
    for (long open_at = 1000; open_at <= 1001; open_at++) {
      long g = gaps[i];
      unsigned long lost = g == 1 ? 1u : 0u;
      bus_open(open_at, true);
      bus_send(50);
      long last = bus.m - 1;
      bus_quiet(g);
      bus_send(1);
      CHECK_EQ(bus.lost, lost);
      if (g > 1)
        CHECK_EQ(bus.last_rx, last + g + 1);
      bus_send(49);
      bus_tail();
      CHECK_EQ(bus.lost, lost);
      CHECK_EQ(bus.delivered, 100 - lost);
      CHECK_EQ(bus.holes, (unsigned long)g + lost);
      CHECK_EQ(bus.stamps, 0);
      /* INCOMPISOOUT in the armed parity's microframes until the follow
       * starts, then in each */
      CHECK_EQ(bus.incomp, g >= 3 ? (unsigned long)g - 1u : 1u);
      CHECK_EQ(out.gap_follows, g >= 3 ? 1u : 0u);
      CHECK_EQ(out.follows, g >= 3 ? (uint32_t)g - 3u : 0u);
      CHECK_EQ(out.follow_late, 0);
      CHECK_EQ(out.errors, 0);
    }
  }
}

/* The host streams without a gap: nothing changes the parity, nothing is
 * lost or stamped wrong, across the wrap of the microframe numbers; also
 * with packets that complete after the EOPF point, served in their own
 * pass or in the pass of their XFRC, with INCOMPISOOUT raised at every
 * EOPF (every_eopf), and with a pass one microframe late, with and without
 * every_eopf */
static void test_out_gap_streaming(void)
{
  for (int v = 0; v < 7; v++) {
    bus_open(0x3C00, true); /* 2,001 microframes: across 0x3FFF to 0x0000 */
    bus.every_eopf = v >= 3 && v <= 5;
    bus_send(1);
    for (long i = 0; i < 2000; i++) {
      if (i % 100 == 50) {
        if (v == 1 || v == 4) {
          bus.late_next = true;
        } else if (v == 2) {
          bus.late_next = true;
          bus.same_pass = true;
        } else if (v >= 5) {
          /* the passes from this microframe's EOPF on, that of the next
           * one's XFRC and, with every_eopf, of the INCOMPISOOUT raised at this
           * EOPF, come at the start of the one after: one microframe late */
          bus.block_from = bus.m;
          bus.block_until = bus.m + 2;
        }
      }
      bus_step(true);
    }
    bus_tail();
    CHECK_EQ(bus.lost, 0);
    CHECK_EQ(bus.delivered, 2001);
    CHECK_EQ(bus.holes, 0);
    CHECK_EQ(bus.stamps, 0);
    CHECK_EQ(out.gap_follows, 0);
    CHECK_EQ(out.follows, 0);
    CHECK_EQ(out.errors, 0);
    if (v == 0)
      CHECK_EQ(bus.incomp, 0);
    if (v == 1 || v == 2)
      CHECK_EQ(bus.incomp, 20);
  }

  /* A re-arm in P + 1 after its token (the injection build) loses that
   * packet and, as in 0.1.0 with B-next, the next one: the hole is 2 */
  bus_open(0x3000, true);
  bus_send(10);
  bus.late_arm = true;
  bus_send(10);
  bus_tail();
  CHECK_EQ(bus.lost, 2);
  CHECK_EQ(bus.delivered, 18);
  CHECK_EQ(bus.holes, 2);
  CHECK_EQ(bus.stamps, 0);
  CHECK_EQ(out.gap_follows, 0);
  CHECK_EQ(out.errors, 0);

  /* A corrupted packet: the core drops it and stays armed for its
   * microframe's parity, so the next packet is lost too, as in 0.1.0 */
  bus_open(0x3000, true);
  bus_send(10);
  bus.crc_next = true;
  bus_send(10);
  bus_tail();
  CHECK_EQ(bus.corrupted, 1);
  CHECK_EQ(bus.lost, 1);
  CHECK_EQ(bus.delivered, 18);
  CHECK_EQ(bus.holes, 2);
  CHECK_EQ(bus.stamps, 0);
  CHECK_EQ(out.gap_follows, 0);
  CHECK_EQ(out.errors, 0);
}

/* One miss, then the next packet of the armed parity completes past its
 * EOPF: its INCOMPISOOUT finds NAKSTS set, so it is no evidence of a gap,
 * and the follow writes nothing under it. v 0: a gap of 2; v 1: a single
 * skip and a packet lost to the other parity (lost as in 0.1.0); v 2: a
 * corrupted packet and the next one lost (as in 0.1.0); v 3: a re-arm
 * after the token (two lost, as in 0.1.0 with B-next); v 4: a gap of 30,
 * whose first packet completes late while the follow runs */
static void test_out_gap_false(void)
{
  static const unsigned long want_lost[] = {0, 1, 1, 2, 0};
  static const unsigned long want_holes[] = {2, 2, 2, 2, 30};
  for (int v = 0; v < 5; v++) {
    for (int every_eopf = 0; every_eopf <= 1; every_eopf++) {
      bus_open(6000, true);
      bus.every_eopf = every_eopf != 0;
      bus_send(20);
      if (v == 0) {
        bus_quiet(2);
      } else if (v == 1) {
        bus_quiet(1);
        bus_send(1);
      } else if (v == 2) {
        bus.crc_next = true;
        bus_send(2);
      } else if (v == 3) {
        bus.late_arm = true;
        bus_send(3);
      } else {
        bus_quiet(30);
      }
      bus.late_next = true;
      bus_send(20);
      bus_tail();
      CHECK_EQ(bus.lost, want_lost[v]);
      CHECK_EQ(bus.stamps, 0);
      CHECK_EQ(bus.holes, want_holes[v]);
      CHECK_EQ(out.gap_follows, v == 4 ? 1u : 0u);
      CHECK_EQ(out.errors, 0);
    }
  }
}

/* Late passes in a gap: the follow starts later and no packet is stamped
 * wrong. A first evidence served one microframe late counts in
 * incomp_other, evidence four apart and a skipped EOPF of the follow in
 * follow_late; a first evidence served two microframes late, or a pass
 * held for eight, is taken as the first evidence of a later microframe
 * and counts nowhere (v 1, 4) */
static void test_out_gap_late_pass(void)
{
  for (int v = 0; v < 5; v++) {
    for (int every_eopf = 0; every_eopf <= 1; every_eopf++) {
      bus_open(5000, true);
      bus.every_eopf = every_eopf != 0;
      bus_send(20);
      long last = bus.m - 1;
      if (v == 0) { /* the first INCOMPISOOUT served one microframe late */
        bus.block_from = last + 1;
        bus.block_until = last + 2;
      } else if (v == 1) { /* ... two microframes late */
        bus.block_from = last + 1;
        bus.block_until = last + 3;
      } else if (v == 2) { /* the second one served two microframes late */
        bus.block_from = last + 3;
        bus.block_until = last + 5;
      } else if (v == 3) { /* an EOPF of the follow served two late */
        bus.block_from = last + 8;
        bus.block_until = last + 10;
      } else { /* a pass blocked for eight microframes */
        bus.block_from = last + 1;
        bus.block_until = last + 9;
      }
      bus_quiet(30);
      bus_send(20);
      bus_tail();
      CHECK_EQ(bus.lost, 0);
      CHECK_EQ(bus.delivered, 40);
      CHECK_EQ(bus.holes, 30);
      CHECK_EQ(bus.stamps, 0);
      CHECK_EQ(out.gap_follows, 1);
      CHECK_EQ(out.errors, 0);
      if (v == 0 && every_eopf == 0)
        CHECK_EQ(out.incomp_other, 1); /* served after the SOF */
      if (v == 2 || v == 3)
        CHECK(out.follow_late >= 1);   /* evidence 4 apart; an EOPF skipped */
      if (v == 1 || v == 4)
        CHECK_EQ(out.follow_late, 0);  /* taken as the first evidence: unseen */
    }
  }

  /* The first evidence served after the SOF, and the host resuming right
   * after the next: the follow has not started, and that packet is lost
   * (counted in incomp_other only) */
  bus_open(5000, true);
  bus_send(20);
  long last = bus.m - 1;
  bus.block_from = last + 1;
  bus.block_until = last + 2;
  bus_quiet(3);
  bus_send(10);
  bus_tail();
  CHECK_EQ(bus.lost, 1);
  CHECK_EQ(bus.holes, 4);
  CHECK_EQ(bus.stamps, 0);
  CHECK_EQ(out.incomp_other, 1);
  CHECK_EQ(out.gap_follows, 0);
}

/* A fresh open: the follow from the open takes the first packet in either
 * parity, and INCOMPISOOUT starts no gap follow before it; with the follow
 * mode off (INCOMPISOOUT masked as well) the parity guessed at the open
 * stays until the first packet (0.1.0 had the follow from the open), and
 * the parity armed after a packet through a gap, as in 0.1.0 */
static void test_out_gap_open(void)
{
  static const long first[] = {1, 2, 3, 4, 1920, 1921};
  for (unsigned i = 0; i < sizeof first / sizeof first[0]; i++) {
    for (long open_at = 3000; open_at <= 3001; open_at++) {
      bus_open(open_at, true);
      bus_quiet(first[i] - 1);
      bus_send(10);
      bus_tail();
      CHECK_EQ(bus.lost, 0);
      CHECK_EQ(bus.delivered, 10);
      CHECK_EQ(bus.holes, 0);
      CHECK_EQ(bus.stamps, 0);
      CHECK_EQ(out.gap_follows, 0);
      CHECK_EQ(out.follows, (uint32_t)first[i] - 1u);
      CHECK_EQ(out.errors, 0);
    }
  }
  unsigned long lost = 0;
  for (long open_at = 3000; open_at <= 3001; open_at++) {
    for (long q = 0; q <= 1; q++) {
      bus_open(open_at, false);
      bus_quiet(q);
      bus_send(10);
      bus_quiet(5);
      bus_send(10);
      lost += bus.lost;
      CHECK_EQ(out.gap_follows, 0);
      CHECK_EQ(out.follows, 0);
    }
  }
  /* the first packet of the two opens in the other parity, and the first
   * after the gap of 5 in all four */
  CHECK_EQ(lost, 2 + 4);
}

/* The app holds every packet it may while the host pauses: the reserve
 * buffer stays armed through the gap, its parity follows the bus, and the
 * first packet after the gap is a reserve packet in its own microframe */
static void test_out_gap_reserve(void)
{
  for (long g = 7; g <= 8; g++) {
    bus_open(4000, true);
    bus.hold = true;
    bus_send(15); /* the device stack holds 15 of 16 */
    CHECK_EQ(out.held, 15);
    bus_send(3);  /* reserve packets */
    bus_quiet(g);
    bus_send(3);
    CHECK_EQ(bus.lost, 0);
    CHECK_EQ(out.no_buffer, 6);
    CHECK_EQ(out.no_packet, (uint32_t)g);
    CHECK_EQ(out.gap_follows, 1);
    bus.hold = false;
    u2ps_dwc2_out_release(&out, out.held);
    bus_send(1);
    CHECK_EQ(bus.lost, 0);
    CHECK_EQ(bus.delivered, 16);
    CHECK_EQ(bus.holes, 6 + (unsigned long)g);
    CHECK_EQ(bus.stamps, 0);
    CHECK_EQ(out.errors, 0);
  }
}

/* The decision at INCOMPISOOUT, case by case */
static void test_out_incomplete(void)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;

  open_out(4, 99, &odd); /* armed for 100: even */
  /* The follow from the open runs: nothing */
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(100), true, &odd));
  r = packet(100, 100, 100, &armed, &a); /* armed for 101: odd */
  u2ps_dwc2_out_release(&out, 1);
  /* Not idle (an XFRC pending or served): nothing, and no evidence */
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(101), false, &odd));
  CHECK(!out.gap_seen);
  /* Not the armed parity: a pass served after the SOF, counted apart */
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(102), true, &odd));
  CHECK_EQ(out.incomp_other, 1);
  CHECK_EQ(out.follow_late, 0);
  CHECK(!out.gap_seen);
  /* The first INCOMPISOOUT of the armed parity: evidence, no write */
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(101), true, &odd));
  CHECK(out.gap_seen);
  CHECK_EQ(out.gap_fn, 101);
  CHECK_EQ(out.armed_odd, true);
  /* The same microframe again: nothing more */
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(101), true, &odd));
  CHECK_EQ(out.gap_fn, 101);
  /* The next of the armed parity: the gap is sure, the follow starts */
  CHECK(u2ps_dwc2_out_incomplete(&out, uf(103), true, &odd));
  CHECK_EQ(odd, false); /* parity(104) */
  CHECK_EQ(out.armed_odd, false);
  CHECK(out.following);
  CHECK(!out.gap_seen);
  CHECK_EQ(out.gap_follows, 1);
  CHECK_EQ(out.follow_fn, 103);
  u2ps_dwc2_out_eopf_after(&out, uf(103));
  CHECK_EQ(out.follow_late, 0);
  CHECK_EQ(out.gap_cleared, 0);
  /* While it runs, INCOMPISOOUT changes nothing; EOPF writes */
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(104), true, &odd));
  CHECK(u2ps_dwc2_out_eopf(&out, uf(104), &odd));
  CHECK_EQ(odd, true); /* parity(105) */
  /* An EOPF pass that skipped a microframe, served early in 106: counted,
   * and no write; 106 is not armed (odd), 107 is */
  CHECK(!u2ps_dwc2_out_eopf(&out, uf(106), &odd));
  CHECK_EQ(out.follow_late, 1);
  CHECK_EQ(out.armed_odd, true);
  CHECK(out.following);
  /* ... and the follow goes on from 106's EOPF */
  CHECK(u2ps_dwc2_out_eopf(&out, uf(106), &odd));
  CHECK_EQ(odd, true); /* parity(107) */
  CHECK_EQ(out.follow_late, 1);
  /* The packet of 107: P from the parity written, a hole of 6 */
  r = packet(107, 107, 107, &armed, &a);
  CHECK_EQ(r.no_packet, 6);
  CHECK(!out.following);
  CHECK(armed);
  CHECK_EQ(out.errors, 0);

  /* Evidence four microframes apart (a pass skipped): counted, and the
   * evidence starts again there */
  u2ps_dwc2_out_release(&out, 1); /* armed for 108: even */
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(108), true, &odd));
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(112), true, &odd));
  CHECK_EQ(out.follow_late, 2);
  CHECK_EQ(out.gap_fn, 112);
  /* A completion clears the evidence, counted */
  r = packet(114, 114, 114, &armed, &a); /* armed for 115 */
  u2ps_dwc2_out_release(&out, 1);
  CHECK(!out.gap_seen);
  CHECK_EQ(out.gap_cleared, 1);
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(115), true, &odd));
  CHECK(out.gap_seen);
  CHECK_EQ(u2ps_dwc2_out_drop(&out, uf(117)), 3);
  CHECK(!out.gap_seen);
  CHECK_EQ(out.gap_cleared, 2);
  CHECK(u2ps_dwc2_out_arm(&out, uf(117), &a)); /* armed for 118 */

  /* Across the wrap: evidence in 0x3FFE, the follow from 0x0000 */
  open_out(4, 0x3FFC, &odd);
  r = packet(0x3FFD, 0x3FFD, 0x3FFD, &armed, &a); /* armed for 0x3FFE */
  u2ps_dwc2_out_release(&out, 1);
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(0x3FFE), true, &odd));
  CHECK(u2ps_dwc2_out_incomplete(&out, uf(0x0000), true, &odd));
  CHECK_EQ(odd, true); /* parity(0x0001) */
  r = packet(0x0001, 0x0001, 0x0001, &armed, &a);
  CHECK_EQ(r.no_packet, 3);
  CHECK_EQ(out.errors, 0);

  /* Not while a completion awaits its arm decision, nor in a close, nor
   * with no stream */
  open_out(4, 199, &odd);
  r = packet(200, 200, 200, &armed, &a);
  CHECK(u2ps_dwc2_out_complete(&out, uf(201), false, &r));
  CHECK_EQ(out.state, U2PS_DWC2_OUT_COMPLETED);
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(201), true, &odd));
  CHECK(!out.gap_seen);
  CHECK(u2ps_dwc2_out_arm(&out, uf(201), &a)); /* armed for 202 */
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(202), true, &odd));
  CHECK(out.gap_seen);
  u2ps_dwc2_out_closing(&out);
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(204), true, &odd));
  u2ps_dwc2_out_close(&out);
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(204), true, &odd));
  CHECK_EQ(out.gap_follows, 0);
  CHECK_EQ(out.armed_odd, false);
  /* An open clears the evidence */
  CHECK(u2ps_dwc2_out_open(&out, 4, uf(300), &odd));
  CHECK(!out.gap_seen);

  /* Two late EOPF passes of a follow in a row: the first (served early in
   * 406, the parity of 406 armed) writes nothing, nor does the second,
   * early in 407: both count */
  open_out(4, 399, &odd);
  r = packet(400, 400, 400, &armed, &a); /* armed for 401 */
  u2ps_dwc2_out_release(&out, 1);
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(401), true, &odd));
  CHECK(u2ps_dwc2_out_incomplete(&out, uf(403), true, &odd)); /* armed for 404 */
  CHECK(u2ps_dwc2_out_eopf(&out, uf(404), &odd));             /* armed for 405 */
  CHECK(!u2ps_dwc2_out_eopf(&out, uf(407), &odd)); /* EOPF 405, 406 missed */
  CHECK_EQ(out.follow_late, 1);
  CHECK_EQ(out.armed_odd, true); /* 407's parity stays armed */
  CHECK(!u2ps_dwc2_out_eopf(&out, uf(408), &odd)); /* EOPF 407 missed too */
  CHECK_EQ(out.follow_late, 2);
  CHECK_EQ(out.armed_odd, true); /* 409's */
  CHECK_EQ(out.follows, 1);

  /* An XFRC in a close neither counts a follow's missed passes nor the
   * evidence it clears */
  open_out(4, 499, &odd);
  r = packet(500, 500, 500, &armed, &a); /* armed for 501 */
  u2ps_dwc2_out_release(&out, 1);
  CHECK(!u2ps_dwc2_out_incomplete(&out, uf(501), true, &odd));
  CHECK(out.gap_seen);
  u2ps_dwc2_out_closing(&out);
  CHECK(u2ps_dwc2_out_complete(&out, uf(505), false, &r));
  CHECK(!out.gap_seen);
  CHECK_EQ(out.gap_cleared, 0);
  open_out(4, 599, &odd);
  out.following = true;
  out.follow_fn = 590;
  u2ps_dwc2_out_closing(&out);
  CHECK(u2ps_dwc2_out_complete(&out, uf(601), false, &r));
  CHECK_EQ(out.follow_late, 0);
}

/* ---- Feedback IN endpoint -------------------------------------------------- */

static void test_fb_limits(void)
{
  u2ps_dwc2_fb_t f;
  memset(&f, 0, sizeof f);
  CHECK(!u2ps_dwc2_fb_open(&f, 0, 0));
  CHECK(!u2ps_dwc2_fb_open(&f, 1, 0));
  CHECK(u2ps_dwc2_fb_open(&f, 2, 0));
  CHECK_EQ(f.period, 2);
  CHECK(u2ps_dwc2_fb_open(&f, 4, 0));
  CHECK_EQ(f.period, 8);
  CHECK(u2ps_dwc2_fb_open(&f, 13, 0));
  CHECK_EQ(f.period, 4096);
  CHECK(!u2ps_dwc2_fb_open(&f, 14, 0));
  CHECK(!u2ps_dwc2_fb_open(&f, 16, 0));
}

/* Opens in microframe 'at' with the follow turned off (a bench
 * comparison): the parity guessed then, parity(at + 1), stays, and no poll
 * is known; returns 'at' */
static long open_no_follow(u2ps_dwc2_fb_t *f, uint8_t interval, long at)
{
  CHECK(u2ps_dwc2_fb_open(f, interval, uf(at)));
  u2ps_dwc2_fb_follow_off(f);
  CHECK(!f->following);
  CHECK(!u2ps_dwc2_fb_eopf(f, uf(at)));
  CHECK_EQ(f->armed_odd, odd_of(at + 1));
  return at;
}

/* The first poll gets the value given at the open, whatever its
 * microframe: from the open on, the parity follows the bus at each EOPF
 * (the end of the periodic part of a microframe), so in every microframe
 * the endpoint is armed for that microframe's parity. The XFRC of the
 * first poll ends the follow and gives Q, on time and handled in the next
 * microframe (before that microframe's EOPF, which the handler takes after
 * the XFRC). bInterval 4, period 8. */
static void test_fb_first_poll(void)
{
  unsigned unanswered = 0, wrong_q = 0, still_following = 0, wrong_odd = 0, recovered = 0;
  unsigned eopf_after = 0;
  for (long open_at = 1000; open_at <= 1001; open_at++) {
    for (long first = open_at + 2; first <= open_at + 40; first++) {
      for (int late = 0; late <= 1; late++) {
        u2ps_dwc2_fb_t f;
        memset(&f, 0, sizeof f);
        CHECK(u2ps_dwc2_fb_open(&f, 4, uf(open_at)));
        CHECK(f.following);
        for (long m = open_at; m < first; m++) {
          CHECK(u2ps_dwc2_fb_eopf(&f, uf(m)));
          /* IISOIXFR while the parity follows means nothing */
          if (u2ps_dwc2_fb_iisoixfr(&f, uf(m)) != U2PS_DWC2_FB_NONE)
            recovered++;
        }
        /* The token in microframe 'first' finds its parity armed */
        if (f.armed_odd != odd_of(first))
          unanswered++;
        if (!u2ps_dwc2_fb_xfrc(&f, uf(first + late)))
          unanswered++;
        if (f.expect != uf(first + 8) || !f.expect_known)
          wrong_q++;
        if (f.following)
          still_following++;
        if (f.odd != odd_of(first))
          wrong_odd++;
        /* The follow is over: EOPF changes nothing more */
        if (u2ps_dwc2_fb_eopf(&f, uf(first)) || f.odd != odd_of(first))
          eopf_after++;
      }
    }
  }
  CHECK_EQ(unanswered, 0);
  CHECK_EQ(wrong_q, 0);
  CHECK_EQ(still_following, 0);
  CHECK_EQ(wrong_odd, 0);
  CHECK_EQ(recovered, 0);
  CHECK_EQ(eopf_after, 0);

  /* Across the wrap: open at 0x3FFC, first poll at 0x0002 */
  u2ps_dwc2_fb_t f;
  memset(&f, 0, sizeof f);
  CHECK(u2ps_dwc2_fb_open(&f, 4, 0x3FFC));
  for (long m = 0x3FFC; m < 0x4002; m++)
    CHECK(u2ps_dwc2_fb_eopf(&f, uf(m)));
  CHECK_EQ(f.armed_odd, false);
  CHECK(u2ps_dwc2_fb_xfrc(&f, 0x0002));
  CHECK_EQ(f.expect, 0x000A);
  CHECK_EQ(f.recoveries, 0);
}

/* The follow has no time limit: a first poll 240 ms (1,920 microframes)
 * after the open, as Linux may send it at high rates, and one 20,000
 * microframes after it (past the wrap of the microframe numbers), finds
 * its parity armed; IISOIXFR means nothing and is not needed until then,
 * and that poll still ends the follow. bInterval 4, period 8. */
static void test_fb_follow_late_poll(void)
{
  static const long after[] = {1920, 1921, 20000};
  for (unsigned i = 0; i < sizeof after / sizeof after[0]; i++) {
    for (long open_at = 1000; open_at <= 1001; open_at++) {
      long first = open_at + after[i];
      unsigned stopped = 0, wrong = 0, acted = 0, watched = 0;
      u2ps_dwc2_fb_t f;
      memset(&f, 0, sizeof f);
      CHECK(u2ps_dwc2_fb_open(&f, 4, uf(open_at)));
      for (long m = open_at; m < first; m++) {
        if (!u2ps_dwc2_fb_eopf(&f, uf(m)))
          stopped++;
        else if (f.armed_odd != odd_of(m + 1) || f.odd != odd_of(m + 1))
          wrong++;
        watched += u2ps_dwc2_fb_watch(&f, uf(m + 1));
        if (u2ps_dwc2_fb_iisoixfr(&f, uf(m + 1)) != U2PS_DWC2_FB_NONE)
          acted++;
      }
      CHECK_EQ(stopped, 0);
      CHECK_EQ(wrong, 0);
      CHECK_EQ(acted, 0);
      CHECK_EQ(watched, 0);
      CHECK(f.following);
      /* The token in microframe 'first' finds its parity armed */
      CHECK_EQ(f.armed_odd, odd_of(first));
      CHECK(u2ps_dwc2_fb_xfrc(&f, uf(first)));
      CHECK(f.expect_known);
      CHECK_EQ(f.expect, uf(first + 8));
      /* ... and the follow is over: EOPF changes nothing more, and the
       * poll window takes over */
      CHECK(!f.following);
      CHECK(!u2ps_dwc2_fb_eopf(&f, uf(first + 1)));
      CHECK_EQ(f.armed_odd, odd_of(first));
      CHECK(u2ps_dwc2_fb_watch(&f, uf(first + 7)));
      CHECK(!u2ps_dwc2_fb_watch(&f, uf(first + 3)));
      CHECK_EQ(f.recoveries + f.idle_recoveries, 0);
    }
  }
}

/* bInterval 4, period 8; the host polls odd microframes 1001, 1009, ... */
static void test_fb_kept(void)
{
  u2ps_dwc2_fb_t f;
  memset(&f, 0, sizeof f);
  CHECK(u2ps_dwc2_fb_open(&f, 4, 1000));
  CHECK_EQ(f.odd, true); /* parity(1001) */
  CHECK_EQ(f.armed_odd, true);
  /* IISOIXFR while the parity follows the bus */
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 1001), U2PS_DWC2_FB_NONE);

  /* XFRCs handled in the next microframe keep the armed parity */
  for (long q = 1001; q < 1001 + 8 * 20; q += 8) {
    CHECK(u2ps_dwc2_fb_xfrc(&f, uf(q + 1)));
    CHECK_EQ(f.odd, true);
    CHECK_EQ(f.expect, uf(q + 8));
    /* IISOIXFR in the armed-parity microframes without a poll, on time
     * and one late, is ignored */
    CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(q + 2)), U2PS_DWC2_FB_NONE);
    CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(q + 3)), U2PS_DWC2_FB_NONE);
    CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(q + 4)), U2PS_DWC2_FB_NONE);
    CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(q + 6)), U2PS_DWC2_FB_NONE);
    CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(q + 7)), U2PS_DWC2_FB_NONE);
  }
  /* Two microframes after an expected poll is not that poll */
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(1001 + 8 * 20 + 2)), U2PS_DWC2_FB_NONE);
  /* One raised in the poll microframe of the XFRC itself, taken after it */
  CHECK(u2ps_dwc2_fb_xfrc(&f, 2001));
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 2001), U2PS_DWC2_FB_NONE);
  CHECK_EQ(f.recoveries, 0);
  CHECK_EQ(f.odd, true);
  /* Also across the wrap: poll 0x3FFF, handled in 0x0000 */
  CHECK(u2ps_dwc2_fb_open(&f, 4, 0x3FFE));
  CHECK_EQ(f.odd, true);
  CHECK(u2ps_dwc2_fb_xfrc(&f, 0x0000));
  CHECK_EQ(f.odd, true);
  CHECK_EQ(f.expect, 0x0007);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 0x0001), U2PS_DWC2_FB_NONE);
}

/* Data flows: the last data packet came in the microframe before fn */
static void data_before(u2ps_dwc2_fb_t *f, long fn)
{
  u2ps_dwc2_fb_data(f, uf(fn - 1));
}

/* A missed poll in the expected microframe gets the recovery with the same
 * parity; a second one, a period after that arm, switches. Data flows
 * throughout, so each counts in recoveries once a poll follows it. */
static void test_fb_recover(void)
{
  u2ps_dwc2_fb_t f;
  memset(&f, 0, sizeof f);
  CHECK(u2ps_dwc2_fb_open(&f, 4, 1000));
  CHECK(u2ps_dwc2_fb_xfrc(&f, 1001));
  /* The host skips the poll of 1009 */
  data_before(&f, 1009);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 1009), U2PS_DWC2_FB_RECOVER);
  CHECK_EQ(f.odd, true);
  CHECK_EQ(f.recoveries, 0); /* until a poll follows */
  CHECK_EQ(f.pending, 1);
  /* Ignored while the recovery runs */
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 1011), U2PS_DWC2_FB_NONE);
  u2ps_dwc2_fb_rearm(&f, 1010);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 1013), U2PS_DWC2_FB_NONE);
  /* The poll of 1017 comes, and the recovery counts */
  CHECK(u2ps_dwc2_fb_xfrc(&f, 1017));
  CHECK_EQ(f.odd, true);
  CHECK_EQ(f.recoveries, 1);
  CHECK_EQ(f.pending, 0);
  /* Skipped at 1025, handled one microframe late */
  data_before(&f, 1026);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 1026), U2PS_DWC2_FB_RECOVER);
  CHECK_EQ(f.odd, true);
  u2ps_dwc2_fb_rearm(&f, 1027);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 1029), U2PS_DWC2_FB_NONE);
  /* No XFRC by 1033 either: switch; the armed parity stays until the arm */
  data_before(&f, 1033);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 1033), U2PS_DWC2_FB_SWITCH);
  CHECK_EQ(f.odd, false);
  CHECK_EQ(f.armed_odd, true);
  CHECK_EQ(f.recoveries, 1);
  CHECK_EQ(f.pending, 2);
  u2ps_dwc2_fb_rearm(&f, 1034);
  CHECK_EQ(f.armed_odd, false);
  /* The other parity's poll of 1041 comes: both count */
  CHECK(u2ps_dwc2_fb_xfrc(&f, 1041));
  CHECK_EQ(f.recoveries, 3);
  CHECK_EQ(f.pending, 0);
  CHECK_EQ(f.idle_recoveries, 0);
}

/* A missed poll at 1009 (period 8, last poll 1001), with the last data
 * packet in microframe 'data' (NO_DATA: none since the open): whether the
 * recovery counts in recoveries; it always counts somewhere, and the
 * action does not change */
#define NO_DATA (-1000000L)
static bool counted_with_data(long open_at, long data)
{
  u2ps_dwc2_fb_t f;
  memset(&f, 0, sizeof f);
  if (data != NO_DATA)
    u2ps_dwc2_fb_data(&f, uf(data)); /* a packet of an earlier stream */
  CHECK(u2ps_dwc2_fb_open(&f, 4, uf(open_at)));
  if (data != NO_DATA)
    u2ps_dwc2_fb_data(&f, uf(data));
  CHECK(u2ps_dwc2_fb_xfrc(&f, uf(open_at + 1)));
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(open_at + 9)), U2PS_DWC2_FB_RECOVER);
  u2ps_dwc2_fb_rearm(&f, uf(open_at + 10));
  CHECK(u2ps_dwc2_fb_xfrc(&f, uf(open_at + 17))); /* the polls go on */
  CHECK_EQ(f.pending, 0);
  CHECK_EQ(f.recoveries + f.idle_recoveries, 1);
  return f.recoveries == 1u;
}

/* Recoveries count in fb_recoveries only while data flows, a data packet
 * within the period before the IISOIXFR (-2 to +period microframes), and
 * once a poll has followed. At a stream's end hosts stop the feedback polls
 * some periods before the data, and the polls missed afterwards are not the
 * port's faults. */
static void test_fb_data_rule(void)
{
  CHECK(!counted_with_data(1000, NO_DATA));  /* no packet since the open */
  CHECK(counted_with_data(1000, 1008));      /* the microframe before */
  CHECK(counted_with_data(1000, 1001));      /* a period before: 8 */
  CHECK(!counted_with_data(1000, 1000));     /* 9 before */
  CHECK(counted_with_data(1000, 1011));      /* P read two microframes on */
  CHECK(!counted_with_data(1000, 1012));
  CHECK(counted_with_data(0x3FF8, 0x3FFE));  /* across the wrap: poll at 0x0001 */
  CHECK(!counted_with_data(1000, 1009 - 12000)); /* 1.5 s before */
  /* A packet before the open does not count for the new stream */
  {
    u2ps_dwc2_fb_t f;
    memset(&f, 0, sizeof f);
    u2ps_dwc2_fb_data(&f, 1000);
    CHECK(u2ps_dwc2_fb_open(&f, 4, 1000));
    CHECK(!f.data_seen);
  }

  /* A stream's end: the polls stop after 1993, the data 'tail'
   * microframes later (U5 behind Linux: about 32 at 48 and 192 kHz). No
   * poll follows any recovery or switch until the close, so none counts in
   * recoveries, whether the data still flowed at it or not */
  for (long tail = 7; tail <= 40; tail += 11) {
    u2ps_dwc2_fb_t f;
    memset(&f, 0, sizeof f);
    CHECK(u2ps_dwc2_fb_open(&f, 4, 1000));
    for (long q = 1001; q <= 1993; q += 8) {
      u2ps_dwc2_fb_data(&f, uf(q));
      CHECK(u2ps_dwc2_fb_xfrc(&f, uf(q)));
    }
    unsigned actions = 0;
    for (long m = 1994; m < 2001 + 8 * 10; m++) {
      if (m <= 1993 + tail)
        u2ps_dwc2_fb_data(&f, uf(m));
      if (u2ps_dwc2_fb_iisoixfr(&f, uf(m)) != U2PS_DWC2_FB_NONE) {
        actions++;
        u2ps_dwc2_fb_rearm(&f, uf(m + 1));
      }
    }
    CHECK(actions >= 5u);
    CHECK(f.pending >= 1u);
    CHECK_EQ(f.recoveries, 0);
    u2ps_dwc2_fb_close(&f);
    CHECK_EQ(f.pending, 0);
    CHECK_EQ(f.recoveries, 0);
    CHECK_EQ(f.idle_recoveries, actions);
    /* Both counters are kept across opens */
    CHECK(u2ps_dwc2_fb_open(&f, 4, 3000));
    CHECK_EQ(f.recoveries, 0);
    CHECK_EQ(f.idle_recoveries, actions);
  }
  /* Pending at the next open without a close: idle too */
  {
    u2ps_dwc2_fb_t f;
    memset(&f, 0, sizeof f);
    CHECK(u2ps_dwc2_fb_open(&f, 4, 1000));
    CHECK(u2ps_dwc2_fb_xfrc(&f, 1001));
    data_before(&f, 1009);
    CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 1009), U2PS_DWC2_FB_RECOVER);
    CHECK_EQ(f.pending, 1);
    CHECK(u2ps_dwc2_fb_open(&f, 4, 2000));
    CHECK_EQ(f.pending, 0);
    CHECK_EQ(f.recoveries, 0);
    CHECK_EQ(f.idle_recoveries, 1);
  }
}

/* An XFRC during a recovery: the poll came late, after IISOIXFR. It
 * confirms the armed parity, the register layer does not re-arm on it, and
 * a pending switch is cancelled */
static void test_fb_xfrc_in_recovery(void)
{
  u2ps_dwc2_fb_t f;
  memset(&f, 0, sizeof f);
  CHECK(u2ps_dwc2_fb_open(&f, 4, 1000));
  CHECK(u2ps_dwc2_fb_xfrc(&f, 1001));
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 1009), U2PS_DWC2_FB_RECOVER);
  CHECK(!u2ps_dwc2_fb_xfrc(&f, 1010)); /* the poll of 1009, late */
  CHECK_EQ(f.expect, 1017);
  CHECK(f.confirmed);
  u2ps_dwc2_fb_rearm(&f, 1011);
  CHECK_EQ(f.armed_odd, true);
  /* A missed poll now gets a recovery, not a switch */
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 1017), U2PS_DWC2_FB_RECOVER);

  /* A switch cancelled: the follow off, no poll for a period after the
   * open, then the poll of t+9 comes late, during the switch's recovery */
  long t = open_no_follow(&f, 4, 2000);
  bool p = odd_of(t + 1);
  CHECK_EQ(f.armed_odd, p);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(t + 8)), U2PS_DWC2_FB_SWITCH);
  CHECK_EQ(f.odd, !p);
  CHECK(!u2ps_dwc2_fb_xfrc(&f, uf(t + 10)));
  CHECK_EQ(f.odd, p);
  CHECK_EQ(f.expect, uf(t + 17)); /* Q from the armed parity: t+9 */
  u2ps_dwc2_fb_rearm(&f, uf(t + 11));
  CHECK_EQ(f.armed_odd, p);
  CHECK(u2ps_dwc2_fb_xfrc(&f, uf(t + 17)));
}

/* The follow turned off at the open (a bench comparison): the parity
 * guessed then, t+1's, stays. Here the host polls the other parity, t+10,
 * t+18, ...; no XFRC comes, and the parity switches after a period */
static void test_fb_switch(void)
{
  u2ps_dwc2_fb_t f;
  memset(&f, 0, sizeof f);
  long t = open_no_follow(&f, 4, 1000);
  bool p = odd_of(t + 1);
  CHECK_EQ(f.armed_odd, p);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(t + 2)), U2PS_DWC2_FB_NONE);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(t + 4)), U2PS_DWC2_FB_NONE);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(t + 6)), U2PS_DWC2_FB_NONE);
  CHECK_EQ(f.recoveries, 0);
  /* The one of t+8, a period after the open; data flows */
  data_before(&f, t + 8);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(t + 8)), U2PS_DWC2_FB_SWITCH);
  CHECK_EQ(f.odd, !p);
  CHECK_EQ(f.pending, 1);
  u2ps_dwc2_fb_rearm(&f, uf(t + 8));
  CHECK_EQ(f.armed_odd, !p);
  /* The other parity now; the poll of t+10 comes */
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(t + 9)), U2PS_DWC2_FB_NONE);
  CHECK(u2ps_dwc2_fb_xfrc(&f, uf(t + 10)));
  CHECK_EQ(f.odd, !p);
  CHECK_EQ(f.expect, uf(t + 18));
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(t + 12)), U2PS_DWC2_FB_NONE);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(t + 14)), U2PS_DWC2_FB_NONE);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(t + 16)), U2PS_DWC2_FB_NONE);
  CHECK_EQ(f.recoveries, 1);

  /* Never polled: it switches once per period, on time */
  t = open_no_follow(&f, 4, 2000);
  p = odd_of(t + 1);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(t + 7)), U2PS_DWC2_FB_NONE);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(t + 9)), U2PS_DWC2_FB_SWITCH);
  u2ps_dwc2_fb_rearm(&f, uf(t + 10));
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(t + 16)), U2PS_DWC2_FB_NONE);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, uf(t + 18)), U2PS_DWC2_FB_SWITCH);
  CHECK_EQ(f.odd, p);

  /* Period 2: every armed-parity microframe is a poll */
  CHECK(u2ps_dwc2_fb_open(&f, 2, 3000));
  CHECK(u2ps_dwc2_fb_xfrc(&f, 3001));
  CHECK_EQ(f.expect, 3003);
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 3003), U2PS_DWC2_FB_RECOVER);
}

/* IISOIXFR is needed only where the logic acts on it: in the microframe
 * before an expected poll and in the poll microframe itself (so the
 * register layer unmasks it there, clearing what came while it was
 * masked), and at every microframe while no poll is known; never while
 * the parity follows the bus or a recovery runs, and not in the poll
 * microframe of an XFRC just taken. bInterval 4, period 8. */
static void test_fb_watch(void)
{
  u2ps_dwc2_fb_t f;
  memset(&f, 0, sizeof f);
  CHECK(u2ps_dwc2_fb_open(&f, 4, 1000));
  CHECK(!u2ps_dwc2_fb_watch(&f, 1001));
  CHECK(!u2ps_dwc2_fb_watch(&f, 1008));
  CHECK(u2ps_dwc2_fb_eopf(&f, 1000));
  CHECK(u2ps_dwc2_fb_xfrc(&f, 1001)); /* Q 1001, expect 1009 */
  /* 1001..1017: E-1 = 1008 and E = 1009, then 1016 and 1017 */
  bool want[] = {false, false, false, false, false, false, false, true, true,
                 false, false, false, false, false, false, true, true};
  unsigned bad = 0;
  for (long fn = 1001; fn <= 1017; fn++)
    bad += u2ps_dwc2_fb_watch(&f, uf(fn)) != want[fn - 1001];
  CHECK_EQ(bad, 0);
  /* Long after the expected poll, at its multiples */
  CHECK(u2ps_dwc2_fb_watch(&f, uf(1009 + 8 * 100)));
  CHECK(u2ps_dwc2_fb_watch(&f, uf(1009 + 8 * 100 - 1)));
  CHECK(!u2ps_dwc2_fb_watch(&f, uf(1009 + 8 * 100 + 1)));
  /* Across the wrap */
  CHECK(u2ps_dwc2_fb_open(&f, 4, 0x3FF8));
  CHECK(u2ps_dwc2_fb_xfrc(&f, 0x3FF9)); /* expect 0x0001 */
  CHECK(u2ps_dwc2_fb_watch(&f, 0x0000));
  CHECK(u2ps_dwc2_fb_watch(&f, 0x0001));
  CHECK(!u2ps_dwc2_fb_watch(&f, 0x0002));
  CHECK(!u2ps_dwc2_fb_watch(&f, 0x3FF9));
  /* During a recovery: no */
  CHECK_EQ(u2ps_dwc2_fb_iisoixfr(&f, 0x0001), U2PS_DWC2_FB_RECOVER);
  CHECK(!u2ps_dwc2_fb_watch(&f, 0x0008));
  u2ps_dwc2_fb_rearm(&f, 0x0002);
  CHECK(u2ps_dwc2_fb_watch(&f, 0x0009));
  /* No poll known (the follow off at the open): every microframe */
  long t = open_no_follow(&f, 4, 2000);
  CHECK(u2ps_dwc2_fb_watch(&f, uf(t + 1)));
  CHECK(u2ps_dwc2_fb_watch(&f, uf(t + 4)));
  /* Period 2 */
  CHECK(u2ps_dwc2_fb_open(&f, 2, 3000));
  CHECK(u2ps_dwc2_fb_xfrc(&f, 3001)); /* expect 3003 */
  CHECK(!u2ps_dwc2_fb_watch(&f, 3001));
  CHECK(u2ps_dwc2_fb_watch(&f, 3002));
  CHECK(u2ps_dwc2_fb_watch(&f, 3003));
}

/* The rule of u2ps_dwc2_fb_watch() (u2ps_dwc2_iso.h), written out: never
 * while the parity follows the bus or a recovery runs; always while no
 * poll is known; otherwise in E - 1 and E and the same microframes of the
 * periods after E, up to half the wrap */
static bool watch_rule(const u2ps_dwc2_fb_t *f, u2ps_uframe_t fn)
{
  if (f->following || f->recovering)
    return false;
  if (!f->expect_known)
    return true;
  int16_t d = u2ps_uframe_dist(f->expect, fn);
  uint16_t off = (uint16_t)(u2ps_uframe_sub(fn, f->expect) & (f->period - 1u));
  return d >= -1 && (off == 0u || off == (uint16_t)(f->period - 1u));
}

static uint32_t rng = 1u;

static uint32_t rnd(uint32_t n)
{
  rng = rng * 1103515245u + 12345u;
  return (rng >> 8) % n;
}

/* The window the logic computes once per change of the feedback's state
 * (each poll, recovery, switch and follow step) against the rule, over
 * every microframe number, after each step of random sequences of the
 * feedback's calls; and every kind of state is seen */
static void test_fb_window(void)
{
  u2ps_dwc2_fb_t f;
  unsigned bad = 0, steps = 0, seen_follow = 0, seen_recover = 0, seen_unknown = 0,
           seen_window = 0;
  memset(&f, 0, sizeof f);
  for (unsigned seq = 0; seq < 24u; seq++) {
    long now = (long)rnd(0x4000u);
    CHECK(u2ps_dwc2_fb_open(&f, (uint8_t)(2u + seq % 12u), uf(now)));
    if (seq % 3u == 1u)
      u2ps_dwc2_fb_follow_off(&f);
    for (unsigned i = 0; i < 60u; i++) {
      /* Mostly small steps of the bus, sometimes a long gap */
      now += rnd(8u) == 0u ? (long)rnd(0x4000u) : (long)rnd(2u * f.period + 2u);
      switch (rnd(8u)) {
      case 0:
      case 1:
        (void)u2ps_dwc2_fb_eopf(&f, uf(now));
        break;
      case 2:
      case 3:
        (void)u2ps_dwc2_fb_xfrc(&f, uf(now));
        break;
      case 4:
      case 5:
        (void)u2ps_dwc2_fb_iisoixfr(&f, uf(now));
        break;
      case 6:
        u2ps_dwc2_fb_rearm(&f, uf(now));
        break;
      default:
        u2ps_dwc2_fb_data(&f, uf(now));
        break;
      }
      seen_follow += f.following;
      seen_recover += !f.following && f.recovering;
      seen_unknown += !f.following && !f.recovering && !f.expect_known;
      seen_window += !f.following && !f.recovering && f.expect_known;
      for (unsigned fn = 0; fn <= MASK; fn++)
        bad += u2ps_dwc2_fb_watch(&f, fn) != watch_rule(&f, (u2ps_uframe_t)fn);
      steps++;
    }
  }
  CHECK_EQ(bad, 0);
  CHECK_EQ(steps, 24u * 60u);
  CHECK(seen_follow > 0u && seen_recover > 0u && seen_unknown > 0u && seen_window > 0u);
  /* The follow turned off at the open: the switch rule at once */
  CHECK(u2ps_dwc2_fb_open(&f, 4, 500));
  CHECK(!u2ps_dwc2_fb_watch(&f, 501));
  u2ps_dwc2_fb_follow_off(&f);
  CHECK(!f.following);
  CHECK(u2ps_dwc2_fb_watch(&f, 501));
  CHECK(!u2ps_dwc2_fb_eopf(&f, 501));
}

/* ---- Ring of buffers -------------------------------------------------------- */

static void test_out_ring(void)
{
  static const uint16_t counts[] = {2, 3, 5, 16};
  unsigned bad = 0, reserves = 0;
  for (unsigned c = 0; c < sizeof counts / sizeof counts[0]; c++) {
    uint16_t count = counts[c];
    bool odd, armed;
    u2ps_dwc2_bnext_t a;
    u2ps_dwc2_out_packet_t r;
    long p = 0x3F00;
    unsigned delivered = 0, released = 0, pending = 0;
    open_out(count, p - 1, &odd);
    for (unsigned i = 0; i < 40u * count; i++) {
      uint16_t held = (uint16_t)(delivered - released);
      r = packet(p, p, p, &armed, &a);
      bad += !armed;
      if (held + U2PS_DWC2_OUT_RESERVE < count) {
        /* Delivered, the next buffer armed */
        bad += r.buf != delivered % count;
        bad += r.no_buffer != pending;
        delivered++;
        pending = 0;
        bad += r.arm_buf != delivered % count;
      } else {
        /* A reserve packet: the buffer after the held ones, armed again */
        bad += r.buf != U2PS_DWC2_NO_BUF || r.arm_buf != delivered % count;
        pending++;
        reserves++;
      }
      held = (uint16_t)(delivered - released);
      if (held != 0u && (held + U2PS_DWC2_OUT_RESERVE >= count || rnd(3u) == 0u)) {
        uint16_t n = (uint16_t)(1u + rnd(held));
        if (rnd(4u) != 0u) {
          u2ps_dwc2_out_release(&out, n);
          released += n;
        }
      }
      bad += out.head != released % count || out.held != delivered - released;
      p++;
    }
  }
  CHECK_EQ(bad, 0);
  CHECK(reserves != 0u);
  CHECK_EQ(out.errors, 0);
}

/* The ring above 32,768 buffers (U2PS_ISO_OUT_COUNT_MAX goes to 65,535):
 * head + held and head + n exceed 65,535, and the next buffer and the
 * release still take them modulo count. Count 40,000: head 39,999 and
 * 25,537 held, the next packet goes into buffer 25,536, not into buffer 0,
 * which the stack holds; a release of 25,537 moves head to 25,536. Then
 * the largest ring, 65,535 buffers, with head 65,534 and 65,533 held. */
static void ring_at(uint16_t count, uint16_t head, uint16_t held, int line)
{
  bool odd, armed;
  u2ps_dwc2_bnext_t a;
  u2ps_dwc2_out_packet_t r;
  long p = 0;
  unsigned bad = 0;

  open_out(count, p - 1, &odd);
  /* head packets delivered and released, then held more delivered */
  for (uint32_t i = 0; i < (uint32_t)head + held; i++) {
    r = packet(p, p, p, &armed, &a);
    bad += r.buf != (uint16_t)(i % count);
    p++;
    if (i < head)
      u2ps_dwc2_out_release(&out, 1);
  }
  CHECK_EQ(out.head, head);
  CHECK_EQ(out.held, held);
  uint16_t next = (uint16_t)(((uint32_t)head + held) % count);
  r = packet(p, p, p, &armed, &a);
  CHECK_EQ(r.buf, next);
  CHECK_EQ(r.arm_buf, (uint16_t)((next + 1u) % count));
  CHECK(armed);
  u2ps_dwc2_out_release(&out, held);
  CHECK_EQ(out.head, next);
  CHECK_EQ(out.held, 1);
  if (bad != 0u || out.errors != 0u)
    printf("  ring_at from line %d: %u packets in the wrong buffer, %u errors\n", line, bad,
           (unsigned)out.errors);
  CHECK_EQ(bad, 0);
  CHECK_EQ(out.errors, 0);
}

static void test_out_ring_large(void)
{
  ring_at(40000, 39999, 25537, __LINE__);
  ring_at(65535, 65534, 65533, __LINE__);
}

/* ---- FIFO RAM -------------------------------------------------------------- */

static void test_fifo(void)
{
  u2ps_dwc2_fifo_t f;

  CHECK(u2ps_dwc2_fifo_plan(776, 4, &f));
  CHECK_EQ(f.rx, 408);
  CHECK_EQ(f.tx0, 16);
  CHECK_EQ(f.tx_fb, 16);
  CHECK_EQ(f.tx0_start, 408);
  CHECK_EQ(f.tx_fb_start, 424);
  CHECK_EQ(f.total, 440);
  /* The largest isochronous packet */
  CHECK(u2ps_dwc2_fifo_plan(1024, 4, &f));
  CHECK_EQ(f.rx, 532);
  CHECK_EQ(f.total, 564);
  /* A size that is not a multiple of 4 keeps its last bytes */
  CHECK(u2ps_dwc2_fifo_plan(777, 4, &f));
  CHECK_EQ(f.rx, 410);
  /* Below EP0's 64 bytes, EP0's packet is the largest */
  CHECK(u2ps_dwc2_fifo_plan(8, 4, &f));
  CHECK_EQ(f.rx, 13 + 2 * 17 + 4 + 1);
  /* A feedback FIFO larger than the minimum */
  CHECK(u2ps_dwc2_fifo_plan(776, 100, &f));
  CHECK_EQ(f.tx_fb, 25);
  CHECK(!u2ps_dwc2_fifo_plan(1025, 4, &f));
}

int main(void)
{
  test_xfrsiz_len();
  test_mem_check();
  test_packet_uframe();
  test_bnext();
  test_out_rearm();
  test_out_wrap();
  test_out_guard();
  test_out_long_gaps();
  test_out_now_clamp();
  test_out_release();
  test_out_reserve_gap();
  test_out_reserve_late();
  test_out_reserve_long();
  test_out_reserve_tail();
  test_out_pending();
  test_out_ring();
  test_out_ring_large();
  test_out_bad_states();
  test_out_drop();
  test_out_closing();
  test_out_follow();
  test_out_follow_late_packet();
  test_out_incomplete();
  test_out_gap_resume();
  test_out_gap_streaming();
  test_out_gap_late_pass();
  test_out_gap_false();
  test_out_gap_open();
  test_out_gap_reserve();
  test_fb_limits();
  test_fb_first_poll();
  test_fb_follow_late_poll();
  test_fb_kept();
  test_fb_recover();
  test_fb_data_rule();
  test_fb_xfrc_in_recovery();
  test_fb_switch();
  test_fb_watch();
  test_fb_window();
  test_fifo();
  return check_done();
}
