/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dwc2_host_logic.c: the DWC2 host port's decision
 * logic, without registers:
 * - the sent microframe from FRNUM and ODDFRM at CHH, the CHH served in the
 *   packet's microframe and 1, 2 and 3 microframes late, and a read taken
 *   before it (at the interrupt's entry), which must be read again;
 * - the late-arm rule: an arm in the packet's own microframe is in time for
 *   the next one, an arm 1 or 2 microframes later is late and reports LATE
 *   before that packet; FRMOR; NO_BUFFER after the queue ran dry; the first
 *   packet; every microframe across the wrap, both parities;
 * - AHBERR: CORRUPTED, and the packet's on_sent;
 * - feedback polls: armed before their microframe; a poll armed in its own
 *   microframe or later is skipped, the next stays one period later;
 *   TXERR, babble, FRMOR;
 * - FIFO RAM in host mode: Rx room for two of the largest IN packets,
 *   non-periodic Tx 16, periodic Tx twice the data packet;
 * - the root port (u2ps_dwc2_hctl.h): OTG_HPRT writes that never write a 1
 *   to an rc_w1 bit unless asked (PENA would disable the port), for
 *   suspend, resume, test mode, disable and the change bits; PSPD to the
 *   speed; PTCTL per selector;
 * - the control channel at each speed and endpoint 0 packet size:
 *   OTG_HCCHARx (LSDEV at low speed only, MPSIZ mps0) and OTG_HCTSIZx per
 *   stage (SETUP DPID, packet counts, IN sizes rounded to mps0, a
 *   zero-length IN status stage reserving mps0), and the status stage's
 *   direction;
 * - a feedback poll is armed only in the microframe right before it, since
 *   an enabled channel polls in the next microframe of its parity;
 * - the streams the port takes;
 * - port events from OTG_HPRT: connect, the enable a reset ends (also one
 *   that comes while PRST is still set, reported at the reset's end), a
 *   disable by the hardware (babble, overcurrent) told from a disconnect
 *   and from the port's own disable, and the acknowledging write;
 * - the stream channels' OTG_HCCHARx and OTG_HCTSIZx, and the bytes an IN
 *   channel received;
 * - the control transfer stage by stage: the stages and their directions,
 *   NAK retries, three transaction errors in a row ending the transfer
 *   (the count restarted by data moved, a NAK-only halt and DTERR),
 *   STALL, AHBERR, abort, the SETUP stage alone, long data stages in parts
 *   with their data PIDs.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "u2ps_dwc2_hctl.h"
#include "u2ps_dwc2_hiso.h"

#define MASK U2PS_UFRAME_MASK

static u2ps_uframe_t uf(long v)
{
  return (u2ps_uframe_t)((unsigned long)v & MASK);
}

static bool odd_of(long v)
{
  return (uf(v) & 1u) != 0u;
}

/* FRNUM as the register gives it: other bits above 13 are ignored */
static uint32_t fr(long v)
{
  return uf(v) | 0xABCD0000u;
}

/* ---- Microframes ------------------------------------------------------------ */

static void test_hframe(void)
{
  unsigned bad = 0;
  for (long t = 0; t < (long)U2PS_UFRAME_COUNT; t++) {
    bool odd = odd_of(t);
    bad += u2ps_dwc2_hframe(fr(t), odd) != uf(t);
    bad += u2ps_dwc2_hframe(fr(t + 1), odd) != uf(t);
    bad += u2ps_dwc2_hframe(fr(t + 2), odd) != uf(t + 2);
    bad += u2ps_dwc2_hframe(fr(t - 1), odd) != uf(t - 2);
  }
  CHECK_EQ(bad, 0);
}

/* ---- The data channel --------------------------------------------------------- */

/* A stream whose first packet is armed at now = start, so it goes out in
 * start + 1 */
static void first(u2ps_dwc2_hout_t *o, long start)
{
  memset(o, 0, sizeof *o);
  u2ps_dwc2_hout_open(o);
  bool odd = u2ps_dwc2_hout_arm(o, fr(start));
  CHECK_EQ(odd, odd_of(start + 1));
  CHECK_EQ(o->target, uf(start + 1));
  CHECK_EQ(o->state, U2PS_DWC2_HOUT_ARMED);
}

/* The CHH of the armed packet served at now = at; returns the report */
static u2ps_dwc2_hout_report_t chh(u2ps_dwc2_hout_t *o, uint32_t hcint, long at)
{
  /* Every field wrong, so a field the call leaves shows */
  u2ps_dwc2_hout_report_t r = {.sent = false, .again = true, .corrupted = true,
                               .uframe = 0x3FFF, .no_buffer = 0xFFFF, .late = 0xFFFF};
  bool ok = u2ps_dwc2_hout_chh(o, hcint, fr(at), false, &r);
  CHECK(ok);
  return r;
}

/* The sent microframe at CHH, served 0..3 microframes after the packet,
 * at every microframe across the wrap; and a read before the packet */
static void test_sent_uframe(void)
{
  unsigned bad = 0;
  for (long t = 0; t < (long)U2PS_UFRAME_COUNT; t += 7) {
    for (long late = 0; late <= 3; late++) {
      u2ps_dwc2_hout_t o;
      first(&o, t - 1);
      u2ps_dwc2_hout_report_t r = chh(&o, U2PS_DWC2_HCINT_XFRC | U2PS_DWC2_HCINT_CHH, t + late);
      bad += !r.sent || r.again || r.corrupted || r.uframe != uf(t) || r.late != 0u ||
             r.no_buffer != 0u;
    }
    /* FRNUM taken at the interrupt's entry, in the microframe before the
     * packet: read again; on the second read, the target stands */
    u2ps_dwc2_hout_t o;
    first(&o, t - 1);
    u2ps_dwc2_hout_report_t r;
    bad += u2ps_dwc2_hout_chh(&o, U2PS_DWC2_HCINT_XFRC, fr(t - 1), false, &r);
    bad += o.state != U2PS_DWC2_HOUT_ARMED || o.errors != 0u;
    bad += !u2ps_dwc2_hout_chh(&o, U2PS_DWC2_HCINT_XFRC, fr(t), false, &r) || r.uframe != uf(t);
    first(&o, t - 1);
    bad += !u2ps_dwc2_hout_chh(&o, U2PS_DWC2_HCINT_XFRC, fr(t - 2), true, &r) ||
           r.uframe != uf(t) || o.errors != 1u;
  }
  CHECK_EQ(bad, 0);
}

/* The late-arm rule: after the CHH of the packet in M, an arm at now = M
 * is in time for M + 1; at M + 1 or M + 2 it is late: LATE M + 1..now,
 * reported before the packet, which goes out in now + 1 */
static void test_late_arm(void)
{
  unsigned bad = 0;
  for (long m = 0; m < (long)U2PS_UFRAME_COUNT; m += 3) {
    for (long d = 0; d <= 2; d++) {
      u2ps_dwc2_hout_t o;
      first(&o, m - 1);
      chh(&o, U2PS_DWC2_HCINT_XFRC, m);
      bool odd = u2ps_dwc2_hout_arm(&o, fr(m + d));
      long target = d == 0 ? m + 1 : m + d + 1;
      bad += odd != odd_of(target) || o.target != uf(target);
      bad += o.late_completions != (d == 0 ? 0u : 1u);
      u2ps_dwc2_hout_report_t r = chh(&o, U2PS_DWC2_HCINT_XFRC, target);
      bad += !r.sent || r.uframe != uf(target) || r.late != (uint16_t)d || r.no_buffer != 0u;
      bad += o.late_total != (uint32_t)d;
    }
  }
  CHECK_EQ(bad, 0);
}

/* FRMOR: the packet missed its target; armed again at now, it reports the
 * microframes target..now as LATE */
static void test_frmor(void)
{
  u2ps_dwc2_hout_t o;
  first(&o, 99);
  chh(&o, U2PS_DWC2_HCINT_XFRC, 100);
  CHECK(u2ps_dwc2_hout_arm(&o, fr(100))); /* 101, odd */
  u2ps_dwc2_hout_report_t r = chh(&o, U2PS_DWC2_HCINT_FRMOR | U2PS_DWC2_HCINT_CHH, 102);
  CHECK(!r.sent);
  CHECK(r.again);
  CHECK_EQ(o.state, U2PS_DWC2_HOUT_AGAIN);
  CHECK(u2ps_dwc2_hout_arm(&o, fr(102)));
  CHECK_EQ(o.target, 103);
  r = chh(&o, U2PS_DWC2_HCINT_XFRC, 103);
  CHECK(r.sent);
  CHECK_EQ(r.uframe, 103);
  CHECK_EQ(r.late, 2);
  CHECK_EQ(o.sent, 2);
  /* FRMOR of the first packet: no hole before a stream's first packet */
  first(&o, 10);
  r = chh(&o, U2PS_DWC2_HCINT_FRMOR, 12);
  CHECK(r.again);
  u2ps_dwc2_hout_arm(&o, fr(12));
  r = chh(&o, U2PS_DWC2_HCINT_XFRC, 13);
  CHECK(r.sent);
  CHECK_EQ(r.uframe, 13);
  CHECK_EQ(r.late, 0);
}

/* The queue ran dry after the packet in M; the next packet is queued at now
 * = M + k: NO_BUFFER k, and it goes out in M + k + 1 */
static void test_no_buffer(void)
{
  static const long ks[] = {0, 1, 5, 500, 8000};
  for (unsigned i = 0; i < sizeof ks / sizeof ks[0]; i++) {
    long k = ks[i], m = 16380;
    u2ps_dwc2_hout_t o;
    first(&o, m - 1);
    chh(&o, U2PS_DWC2_HCINT_XFRC, m);
    u2ps_dwc2_hout_dry(&o);
    CHECK_EQ(o.state, U2PS_DWC2_HOUT_DRY);
    bool odd = u2ps_dwc2_hout_arm(&o, fr(m + k));
    CHECK_EQ(odd, odd_of(m + k + 1));
    CHECK_EQ(o.target, uf(m + k + 1));
    u2ps_dwc2_hout_report_t r = chh(&o, U2PS_DWC2_HCINT_XFRC, m + k + 2);
    CHECK(r.sent);
    CHECK_EQ(r.uframe, uf(m + k + 1));
    CHECK_EQ(r.no_buffer, k);
    CHECK_EQ(r.late, 0);
    CHECK_EQ(o.no_buffer_total, (uint32_t)k);
    CHECK_EQ(o.late_completions, 0);
  }
}

/* AHBERR: the packet's microframe passes CORRUPTED, and it gets its
 * on_sent; the next one goes on */
static void test_ahberr(void)
{
  u2ps_dwc2_hout_t o;
  first(&o, 199);
  u2ps_dwc2_hout_report_t r = chh(&o, U2PS_DWC2_HCINT_AHBERR | U2PS_DWC2_HCINT_CHH, 200);
  CHECK(r.sent);
  CHECK(r.corrupted);
  CHECK(!r.again);
  CHECK_EQ(r.uframe, 200);
  CHECK_EQ(o.corrupted, 1);
  CHECK_EQ(o.errors, 1);
  CHECK_EQ(o.sent, 1);
  CHECK_EQ(o.state, U2PS_DWC2_HOUT_COMPLETED);
  u2ps_dwc2_hout_arm(&o, fr(200));
  r = chh(&o, U2PS_DWC2_HCINT_XFRC, 201);
  CHECK(r.sent);
  CHECK(!r.corrupted);
  CHECK_EQ(r.uframe, 201);
}

/* States the hardware cannot produce */
static void test_states(void)
{
  u2ps_dwc2_hout_t o;
  memset(&o, 0, sizeof o);
  u2ps_dwc2_hout_report_t r;
  CHECK(u2ps_dwc2_hout_chh(&o, U2PS_DWC2_HCINT_XFRC, fr(5), false, &r));
  CHECK(!r.sent);
  CHECK_EQ(o.errors, 1);
  first(&o, 5);
  chh(&o, U2PS_DWC2_HCINT_XFRC, 6);
  CHECK(u2ps_dwc2_hout_chh(&o, U2PS_DWC2_HCINT_XFRC, fr(7), false, &r));
  CHECK(!r.sent);
  CHECK_EQ(o.errors, 1);
  u2ps_dwc2_hout_close(&o);
  CHECK_EQ(o.state, U2PS_DWC2_HOUT_CLOSED);
  CHECK_EQ(o.sent, 1);
}

/* ---- Feedback polls ------------------------------------------------------------- */

static void test_polls(void)
{
  u2ps_dwc2_hfb_t f;
  memset(&f, 0, sizeof f);
  CHECK(!u2ps_dwc2_hfb_open(&f, 0, fr(0)));
  CHECK(!u2ps_dwc2_hfb_open(&f, 14, fr(0)));
  CHECK_EQ(f.state, U2PS_DWC2_HFB_CLOSED);
  for (long q0 = 16380; q0 < 16390; q0++) {
    memset(&f, 0, sizeof f);
    CHECK(u2ps_dwc2_hfb_open(&f, 4, fr(q0 - 1)));
    CHECK_EQ(f.period, 8);
    bool odd = !odd_of(q0);
    u2ps_uframe_t skipped = 0;
    CHECK_EQ(u2ps_dwc2_hfb_arm(&f, fr(q0 - 1), &odd, &skipped), 0);
    CHECK_EQ(odd, odd_of(q0));
    CHECK_EQ(f.poll, uf(q0));
    /* Its CHH, served 0..2 late: the poll's microframe, the data */
    u2ps_dwc2_hfb_result_t res;
    u2ps_uframe_t u;
    CHECK(u2ps_dwc2_hfb_chh(&f, U2PS_DWC2_HCINT_XFRC, fr(q0 + (q0 % 3)), false, &res, &u));
    CHECK_EQ(res, U2PS_DWC2_HFB_DATA);
    CHECK_EQ(u, uf(q0));
    CHECK_EQ(f.polls, 1);
    /* The next poll: not armed in q0 + 1..q0 + 6, since the channel would
     * poll in the next microframe of its parity; armed in q0 + 7 */
    for (long k = 1; k <= 6; k++) {
      bool before = odd;
      CHECK_EQ(u2ps_dwc2_hfb_arm(&f, fr(q0 + k), &odd, &skipped), 0);
      CHECK_EQ(f.state, U2PS_DWC2_HFB_IDLE);
      CHECK_EQ(odd, before);
    }
    CHECK_EQ(u2ps_dwc2_hfb_arm(&f, fr(q0 + 7), &odd, &skipped), 0);
    CHECK_EQ(f.state, U2PS_DWC2_HFB_ARMED);
    CHECK_EQ(f.poll, uf(q0 + 8));
    CHECK_EQ(odd, odd_of(q0 + 8));
    /* Armed already: nothing changes */
    CHECK_EQ(u2ps_dwc2_hfb_arm(&f, fr(q0 + 8), &odd, &skipped), 0);
    CHECK_EQ(f.poll, uf(q0 + 8));
    /* A read before the poll (interrupt entry): read again */
    CHECK(!u2ps_dwc2_hfb_chh(&f, U2PS_DWC2_HCINT_TXERR, fr(q0 + 7), false, &res, &u));
    CHECK(u2ps_dwc2_hfb_chh(&f, U2PS_DWC2_HCINT_TXERR, fr(q0 + 8), false, &res, &u));
    CHECK_EQ(res, U2PS_DWC2_HFB_NO_PACKET);
    CHECK_EQ(u, uf(q0 + 8));
    /* Armed in its own microframe: skipped, the next one period later,
     * armed in the microframe before it */
    CHECK_EQ(u2ps_dwc2_hfb_arm(&f, fr(q0 + 16), &odd, &skipped), 1);
    CHECK_EQ(skipped, uf(q0 + 16));
    CHECK_EQ(f.poll, uf(q0 + 24));
    CHECK_EQ(f.state, U2PS_DWC2_HFB_IDLE);
    CHECK_EQ(u2ps_dwc2_hfb_arm(&f, fr(q0 + 23), &odd, &skipped), 0);
    CHECK_EQ(f.state, U2PS_DWC2_HFB_ARMED);
    CHECK_EQ(odd, odd_of(q0 + 24));
    CHECK(u2ps_dwc2_hfb_chh(&f, U2PS_DWC2_HCINT_BBERR, fr(q0 + 25), false, &res, &u));
    CHECK_EQ(res, U2PS_DWC2_HFB_NO_PACKET);
    CHECK_EQ(u, uf(q0 + 24));
    CHECK_EQ(f.errors, 1);
    /* Armed 20 microframes late: the polls in q0 + 32 and q0 + 40 skipped,
     * q0 + 48 next (not now + 1), armed in q0 + 47 */
    CHECK_EQ(u2ps_dwc2_hfb_arm(&f, fr(q0 + 44), &odd, &skipped), 2);
    CHECK_EQ(skipped, uf(q0 + 32));
    CHECK_EQ(f.poll, uf(q0 + 48));
    CHECK_EQ(f.state, U2PS_DWC2_HFB_IDLE);
    CHECK_EQ(u2ps_dwc2_hfb_arm(&f, fr(q0 + 47), &odd, &skipped), 0);
    CHECK_EQ(f.state, U2PS_DWC2_HFB_ARMED);
    /* FRMOR: the poll did not happen */
    CHECK(u2ps_dwc2_hfb_chh(&f, U2PS_DWC2_HCINT_FRMOR, fr(q0 + 49), false, &res, &u));
    CHECK_EQ(res, U2PS_DWC2_HFB_LATE);
    CHECK_EQ(u, uf(q0 + 48));
    CHECK_EQ(f.polls, 4);
    CHECK_EQ(f.missed, 6);
    /* A CHH with nothing armed */
    CHECK(u2ps_dwc2_hfb_chh(&f, U2PS_DWC2_HCINT_XFRC, fr(q0 + 50), false, &res, &u));
    CHECK_EQ(res, U2PS_DWC2_HFB_NONE);
    CHECK_EQ(f.errors, 2);
    u2ps_dwc2_hfb_close(&f);
    CHECK_EQ(u2ps_dwc2_hfb_arm(&f, fr(q0 + 51), &odd, &skipped), 0);
    CHECK_EQ(f.state, U2PS_DWC2_HFB_CLOSED);
  }
  /* Period 1: a poll every microframe, armed in the microframe before */
  memset(&f, 0, sizeof f);
  CHECK(u2ps_dwc2_hfb_open(&f, 1, fr(41)));
  bool odd;
  u2ps_uframe_t skipped;
  u2ps_dwc2_hfb_result_t res;
  u2ps_uframe_t u;
  for (long q = 42; q < 50; q++) {
    CHECK_EQ(u2ps_dwc2_hfb_arm(&f, fr(q - 1), &odd, &skipped), 0);
    CHECK_EQ(odd, odd_of(q));
    CHECK(u2ps_dwc2_hfb_chh(&f, U2PS_DWC2_HCINT_XFRC, fr(q), false, &res, &u));
    CHECK_EQ(u, q);
  }
  /* Period 4096, the longest */
  memset(&f, 0, sizeof f);
  CHECK(u2ps_dwc2_hfb_open(&f, 13, fr(100)));
  CHECK_EQ(f.period, 4096);
  CHECK_EQ(u2ps_dwc2_hfb_arm(&f, fr(100), &odd, &skipped), 0);
  CHECK(u2ps_dwc2_hfb_chh(&f, U2PS_DWC2_HCINT_XFRC, fr(101), false, &res, &u));
  CHECK_EQ(u2ps_dwc2_hfb_arm(&f, fr(4196), &odd, &skipped), 0);
  CHECK_EQ(f.poll, 4197);
}

/* The arm window: for each period and poll, an arm only in the microframe
 * right before the poll; a skip that lands right before the next poll arms
 * it at once */
static void test_poll_window(void)
{
  unsigned bad = 0;
  for (uint8_t interval = 1; interval <= 5u; interval++) {
    long period = 1L << (interval - 1u);
    for (long p0 = 16370; p0 < 16400; p0 += 3) {
      for (long k = 1; k <= period + 3; k++) {
        u2ps_dwc2_hfb_t f;
        memset(&f, 0, sizeof f);
        bool odd = false, before;
        u2ps_uframe_t skipped = 0;
        /* Opened in p0 - 1: the first poll is p0 */
        CHECK(u2ps_dwc2_hfb_open(&f, interval, fr(p0 - 1)));
        before = odd;
        uint16_t n = u2ps_dwc2_hfb_arm(&f, fr(p0 - k), &odd, &skipped);
        if (k == 1)
          bad += n != 0u || f.state != U2PS_DWC2_HFB_ARMED || odd != odd_of(p0) ||
                 f.poll != uf(p0);
        else
          bad += n != 0u || f.state != U2PS_DWC2_HFB_IDLE || odd != before || f.poll != uf(p0);
      }
      /* Late by period - 1 microframes: the poll skipped, the next one is in
       * the next microframe and armed now */
      u2ps_dwc2_hfb_t f;
      memset(&f, 0, sizeof f);
      bool odd = false;
      u2ps_uframe_t skipped = 0;
      CHECK(u2ps_dwc2_hfb_open(&f, interval, fr(p0 - 1)));
      uint16_t n = u2ps_dwc2_hfb_arm(&f, fr(p0 + period - 1), &odd, &skipped);
      bad += n != 1u || skipped != uf(p0) || f.poll != uf(p0 + period) ||
             f.state != U2PS_DWC2_HFB_ARMED || odd != odd_of(p0 + period);
    }
  }
  CHECK_EQ(bad, 0);
}

/* The streams the port takes */
static void test_stream_fits(void)
{
  CHECK(u2ps_dwc2_hstream_fits(1, 1, 0, 0, 0));
  CHECK(u2ps_dwc2_hstream_fits(15, 1024, 0, 99, 9999)); /* no feedback: its fields unread */
  CHECK(u2ps_dwc2_hstream_fits(1, 1024, 2, 1, 4));
  CHECK(u2ps_dwc2_hstream_fits(1, 776, 15, 13, U2PS_DWC2_HFB_MPS_MAX));
  CHECK(!u2ps_dwc2_hstream_fits(0, 776, 0, 0, 0));
  CHECK(!u2ps_dwc2_hstream_fits(16, 776, 0, 0, 0));
  CHECK(!u2ps_dwc2_hstream_fits(1, 0, 0, 0, 0));
  CHECK(!u2ps_dwc2_hstream_fits(1, 1025, 0, 0, 0));
  CHECK(!u2ps_dwc2_hstream_fits(1, 776, 16, 4, 4));
  CHECK(!u2ps_dwc2_hstream_fits(1, 776, 2, 0, 4));
  CHECK(!u2ps_dwc2_hstream_fits(1, 776, 2, 14, 4));
  /* bInterval 1..13 taken, 14..16 refused (u2ps_dwc2_hiso.h) */
  for (uint8_t i = 1; i <= 16u; i++)
    CHECK_EQ(u2ps_dwc2_hstream_fits(1, 776, 2, i, 4), i <= 13u);
  CHECK(!u2ps_dwc2_hstream_fits(1, 776, 2, 4, 3));
  CHECK(!u2ps_dwc2_hstream_fits(1, 776, 2, 4, U2PS_DWC2_HFB_MPS_MAX + 1u));
}

/* ---- FIFO RAM ------------------------------------------------------------------- */

static void test_fifo(void)
{
  u2ps_dwc2_hfifo_t f;
  /* Rx: two of EP0's 64-byte packets, each with its status word, and the
   * transfer-complete word (RM0456 73.11.3, p. 3311) */
  CHECK(u2ps_dwc2_hfifo_plan(1024, 4, &f));
  CHECK_EQ(f.rx, 35);
  CHECK_EQ(f.nptx, 16);
  CHECK_EQ(f.ptx, 512);
  CHECK_EQ(f.nptx_start, 35);
  CHECK_EQ(f.ptx_start, 51);
  CHECK_EQ(f.total, 563);
  CHECK(u2ps_dwc2_hfifo_plan(776, 4, &f));
  CHECK_EQ(f.ptx, 388);
  CHECK(u2ps_dwc2_hfifo_plan(777, 0, &f));
  CHECK_EQ(f.ptx, 390);
  CHECK_EQ(f.rx, 35);
  CHECK(u2ps_dwc2_hfifo_plan(8, 4, &f));
  CHECK_EQ(f.ptx, 16);
  CHECK(u2ps_dwc2_hfifo_plan(388, 1024, &f));
  CHECK_EQ(f.rx, 515);
  CHECK_EQ(f.total, 515u + 16u + 194u);
  CHECK(!u2ps_dwc2_hfifo_plan(1025, 4, &f));
  CHECK(!u2ps_dwc2_hfifo_plan(0, 4, &f));
  CHECK(!u2ps_dwc2_hfifo_plan(1024, 3, &f));
  CHECK(!u2ps_dwc2_hfifo_plan(1024, 1025, &f));
  /* Every packet size: the periodic Tx FIFO holds two packets */
  unsigned bad = 0;
  for (unsigned mps = 1; mps <= 1024u; mps++) {
    bool ok = u2ps_dwc2_hfifo_plan((uint16_t)mps, 4, &f);
    bad += !ok || 4u * f.ptx < 2u * mps || f.ptx < 16u || f.total > 1024u ||
           f.total != f.rx + f.nptx + f.ptx;
  }
  CHECK_EQ(bad, 0);
  /* Every feedback packet size: the Rx FIFO holds two of the largest IN
   * packets, each with its status word, and the transfer-complete word */
  for (unsigned fb = 4; fb <= 388u; fb++) {
    bool ok = u2ps_dwc2_hfifo_plan(1024, (uint16_t)fb, &f);
    unsigned in_words = ((fb > 64u ? fb : 64u) + 3u) / 4u;
    bad += !ok || f.rx != 2u * (in_words + 1u) + 1u;
  }
  CHECK_EQ(bad, 0);
}

/* ---- The root port and the control channel ------------------------------------- */

static uint32_t rng = 12345u;

static uint32_t next_rand(void)
{
  rng = rng * 1664525u + 1013904223u;
  return rng;
}

/* Every write keeps the bits read but the rc_w1 ones, which it writes 0
 * unless 'set' names them */
static void test_hprt_write(void)
{
  unsigned bad = 0;
  const uint32_t w1c = U2PS_DWC2_HPRT_PCDET | U2PS_DWC2_HPRT_PENA | U2PS_DWC2_HPRT_PENCHNG |
                       U2PS_DWC2_HPRT_POCCHNG;
  CHECK_EQ(U2PS_DWC2_HPRT_W1C, w1c);
  for (unsigned i = 0; i < 100000u; i++) {
    uint32_t hprt = next_rand() & 0x7FFFFu;
    /* Suspend, resume, its end */
    uint32_t v = u2ps_dwc2_hprt_write(hprt, U2PS_DWC2_HPRT_PSUSP, 0);
    bad += v != ((hprt & ~w1c) | U2PS_DWC2_HPRT_PSUSP);
    v = u2ps_dwc2_hprt_write(hprt, U2PS_DWC2_HPRT_PRES, 0);
    bad += v != ((hprt & ~w1c) | U2PS_DWC2_HPRT_PRES);
    v = u2ps_dwc2_hprt_write(hprt, 0, U2PS_DWC2_HPRT_PRES);
    bad += v != (hprt & ~w1c & ~U2PS_DWC2_HPRT_PRES);
    /* Disable: PENA written 1, the change bits 0 */
    v = u2ps_dwc2_hprt_write(hprt, U2PS_DWC2_HPRT_PENA, 0);
    bad += v != ((hprt & ~w1c) | U2PS_DWC2_HPRT_PENA);
    /* A change acknowledged: that bit 1, PENA 0 */
    v = u2ps_dwc2_hprt_write(hprt, U2PS_DWC2_HPRT_PCDET | U2PS_DWC2_HPRT_PENCHNG, 0);
    bad += (v & U2PS_DWC2_HPRT_PENA) != 0u || (v & U2PS_DWC2_HPRT_POCCHNG) != 0u ||
           (v & (U2PS_DWC2_HPRT_PCDET | U2PS_DWC2_HPRT_PENCHNG)) !=
             (U2PS_DWC2_HPRT_PCDET | U2PS_DWC2_HPRT_PENCHNG);
    /* Test mode s: PTCTL replaced */
    uint8_t sel = (uint8_t)(1u + next_rand() % 5u);
    v = u2ps_dwc2_hprt_write(hprt, u2ps_dwc2_hprt_ptctl(sel), U2PS_DWC2_HPRT_PTCTL_MSK);
    bad += v != ((hprt & ~w1c & ~U2PS_DWC2_HPRT_PTCTL_MSK) | ((uint32_t)sel << 13));
    /* PPWR and PRST stay as read */
    bad += (v & (U2PS_DWC2_HPRT_PPWR | U2PS_DWC2_HPRT_PRST)) !=
           (hprt & (U2PS_DWC2_HPRT_PPWR | U2PS_DWC2_HPRT_PRST));
  }
  CHECK_EQ(bad, 0);
}

static void test_hprt_fields(void)
{
  /* PTCTL: 1 J, 2 K, 3 SE0_NAK, 4 Packet, 5 Force_Enable, in bits 16:13 */
  for (uint8_t s = 1; s <= 5u; s++)
    CHECK_EQ(u2ps_dwc2_hprt_ptctl(s), (uint32_t)s << 13);
  CHECK_EQ(u2ps_dwc2_hprt_ptctl(0), 0);
  CHECK_EQ(u2ps_dwc2_hprt_ptctl(6), 0);
  CHECK_EQ(u2ps_dwc2_hprt_ptctl(0xFF), 0);
  /* PSPD in bits 18:17, whatever the others */
  for (unsigned i = 0; i < 1000u; i++) {
    uint32_t rest = next_rand() & ~U2PS_DWC2_HPRT_PSPD_MSK;
    CHECK_EQ(u2ps_dwc2_hprt_speed(rest | (0u << 17)), U2PS_SPEED_HIGH);
    CHECK_EQ(u2ps_dwc2_hprt_speed(rest | (1u << 17)), U2PS_SPEED_FULL);
    CHECK_EQ(u2ps_dwc2_hprt_speed(rest | (2u << 17)), U2PS_SPEED_LOW);
    CHECK_EQ(u2ps_dwc2_hprt_speed(rest | (3u << 17)), U2PS_SPEED_NONE);
    CHECK_EQ(u2ps_dwc2_hprt_resumed(rest & ~U2PS_DWC2_HPRT_PRES), true);
    CHECK_EQ(u2ps_dwc2_hprt_resumed(rest | U2PS_DWC2_HPRT_PRES), false);
  }
}

static void test_hcchar_ep0(void)
{
  static const u2ps_speed_t speeds[3] = {U2PS_SPEED_HIGH, U2PS_SPEED_FULL, U2PS_SPEED_LOW};
  static const uint8_t mps[4] = {8, 16, 32, 64};
  for (unsigned s = 0; s < 3u; s++)
    for (unsigned m = 0; m < 4u; m++)
      for (unsigned addr = 0; addr < 128u; addr += 1u)
        for (unsigned in = 0; in < 2u; in++) {
          uint32_t v = u2ps_dwc2_hcchar_ep0((uint8_t)addr, mps[m], in != 0u, speeds[s]);
          uint32_t want = (uint32_t)addr << 22 | 1u << 20 | (in ? 1u << 15 : 0u) | mps[m] |
                          (speeds[s] == U2PS_SPEED_LOW ? 1u << 17 : 0u);
          if (v != want) {
            CHECK_EQ(v, want);
            return;
          }
        }
  /* EPTYP control, EPNUM 0, no CHENA, CHDIS or ODDFRM */
  uint32_t v = u2ps_dwc2_hcchar_ep0(127, 64, true, U2PS_SPEED_HIGH);
  CHECK_EQ(v & (3u << 18), 0);
  CHECK_EQ(v & (0xFu << 11), 0);
  CHECK_EQ(v & (7u << 29), 0);
}

/* OTG_HCTSIZx fields */
static uint32_t dpid(uint32_t v)
{
  return v >> 29 & 3u;
}

static uint32_t pktcnt(uint32_t v)
{
  return v >> 19 & 0x3FFu;
}

static uint32_t xfrsiz(uint32_t v)
{
  return v & 0x7FFFFu;
}

static void test_hctsiz_ctl(void)
{
  static const uint8_t mps[4] = {8, 16, 32, 64};
  unsigned bad = 0;
  for (unsigned m = 0; m < 4u; m++) {
    uint8_t mp = mps[m];
    uint32_t v = u2ps_dwc2_hctsiz_ctl(U2PS_DWC2_STAGE_SETUP, false, 8, mp);
    bad += dpid(v) != 3u || pktcnt(v) != 1u || xfrsiz(v) != 8u || (v >> 31) != 0u;
    /* The status stage: OUT zero bytes; IN one packet reserved (73.14.35:
     * an IN XFRSIZ is a multiple of the packet size) */
    v = u2ps_dwc2_hctsiz_ctl(U2PS_DWC2_STAGE_STATUS, false, 0, mp);
    bad += dpid(v) != 2u || pktcnt(v) != 1u || xfrsiz(v) != 0u;
    v = u2ps_dwc2_hctsiz_ctl(U2PS_DWC2_STAGE_STATUS, true, 0, mp);
    bad += dpid(v) != 2u || pktcnt(v) != 1u || xfrsiz(v) != mp;
    for (unsigned len = 1; len <= 1100u; len++) {
      uint32_t packets = (len + mp - 1u) / mp;
      v = u2ps_dwc2_hctsiz_ctl(U2PS_DWC2_STAGE_DATA_IN, true, (uint16_t)len, mp);
      bad += dpid(v) != 2u || pktcnt(v) != packets || xfrsiz(v) != packets * mp;
      v = u2ps_dwc2_hctsiz_ctl(U2PS_DWC2_STAGE_DATA_OUT, false, (uint16_t)len, mp);
      bad += dpid(v) != 2u || pktcnt(v) != packets || xfrsiz(v) != len;
    }
  }
  CHECK_EQ(bad, 0);
  /* GET_DESCRIPTOR(device, 18) at mps0 8: three packets, 24 bytes of buffer */
  uint32_t v = u2ps_dwc2_hctsiz_ctl(U2PS_DWC2_STAGE_DATA_IN, true, 18, 8);
  CHECK_EQ(pktcnt(v), 3);
  CHECK_EQ(xfrsiz(v), 24);
}

static void test_status_direction(void)
{
  static const uint8_t get_desc[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
  static const uint8_t get_zero[8] = {0x80, 6, 0, 1, 0, 0, 0, 0};
  static const uint8_t set_addr[8] = {0x00, 5, 1, 0, 0, 0, 0, 0};
  static const uint8_t set_cur[8] = {0x21, 1, 0, 1, 0, 4, 4, 0};
  CHECK(!u2ps_dwc2_status_in(get_desc));
  CHECK(u2ps_dwc2_status_in(get_zero));
  CHECK(u2ps_dwc2_status_in(set_addr));
  CHECK(u2ps_dwc2_status_in(set_cur));
}

/* ---- Port events ------------------------------------------------------------------ */

#define HPRT_CHANGES (U2PS_DWC2_HPRT_PCDET | U2PS_DWC2_HPRT_PENCHNG | U2PS_DWC2_HPRT_POCCHNG)
/* The states of u2ps_dwc2_port_t */
#define PORT_STATES 8u

static void test_hprt_event(void)
{
  u2ps_dwc2_hprt_event_t e;
  unsigned bad = 0;
  /* The acknowledging write: the change bits read, written 1; PENA and
   * PSUSP 0; every other bit as read */
  for (unsigned i = 0; i < 100000u; i++) {
    uint32_t hprt = next_rand() & 0x7FFFFu;
    uint8_t state = (uint8_t)(next_rand() % PORT_STATES);
    u2ps_dwc2_hprt_event(hprt, state, &e);
    uint32_t want = (hprt & ~(U2PS_DWC2_HPRT_W1C | U2PS_DWC2_HPRT_PSUSP)) | (hprt & HPRT_CHANGES);
    bad += e.ack != want;
    /* No change bit read: no event, the state stays */
    u2ps_dwc2_hprt_event(hprt & ~HPRT_CHANGES, state, &e);
    bad += e.connect || e.enabled || e.disabled || e.state != state;
  }
  CHECK_EQ(bad, 0);

  /* Connect */
  u2ps_dwc2_hprt_event(U2PS_DWC2_HPRT_PCDET | U2PS_DWC2_HPRT_PCSTS, U2PS_DWC2_PORT_IDLE, &e);
  CHECK(e.connect);
  CHECK(!e.enabled && !e.disabled);
  CHECK_EQ(e.state, U2PS_DWC2_PORT_IDLE);
  u2ps_dwc2_hprt_event(U2PS_DWC2_HPRT_PCDET, U2PS_DWC2_PORT_IDLE, &e);
  CHECK(!e.connect);

  /* The enable a reset ends, at each speed, once the port has ended the
   * reset; one that comes while PRST is still set (the reset sequence
   * complete before the application clears PRST, RM0456 p. 3358) is kept
   * with its speed and reported at the reset's end */
  static const u2ps_speed_t speeds[3] = {U2PS_SPEED_HIGH, U2PS_SPEED_FULL, U2PS_SPEED_LOW};
  for (uint32_t pspd = 0; pspd < 3u; pspd++) {
    uint32_t v = U2PS_DWC2_HPRT_PENCHNG | U2PS_DWC2_HPRT_PENA | U2PS_DWC2_HPRT_PCSTS | pspd << 17;
    for (uint8_t st = 0; st < PORT_STATES; st++) {
      u2ps_dwc2_hprt_event(v, st, &e);
      if (st == U2PS_DWC2_PORT_RESET_END) {
        CHECK(e.enabled);
        CHECK_EQ(e.speed, speeds[pspd]);
        CHECK_EQ(e.state, U2PS_DWC2_PORT_ENABLED);
      } else if (st == U2PS_DWC2_PORT_RESET) {
        CHECK(!e.enabled);
        CHECK(e.state != U2PS_DWC2_PORT_RESET);
        CHECK_EQ(e.state, U2PS_DWC2_PORT_RESET_ENABLED);
        CHECK_EQ(e.speed, speeds[pspd]);
      } else {
        CHECK(!e.enabled);
        CHECK_EQ(e.state, st);
      }
      CHECK(!e.disabled);
    }
  }

  /* A disable by the hardware: port babble (POCA 0) or an overcurrent, of
   * an enabled, suspended or resuming port with the device attached, or of
   * one the core enabled while PRST was still set */
  for (uint8_t st = 0; st < PORT_STATES; st++) {
    bool on = st == U2PS_DWC2_PORT_ENABLED || st == U2PS_DWC2_PORT_SUSPENDED ||
              st == U2PS_DWC2_PORT_RESUMING || st == U2PS_DWC2_PORT_RESET_ENABLED;
    u2ps_dwc2_hprt_event(U2PS_DWC2_HPRT_PENCHNG | U2PS_DWC2_HPRT_PCSTS, st, &e);
    CHECK_EQ(e.disabled, on);
    CHECK_EQ(e.babble, on);
    CHECK_EQ(e.state, on ? U2PS_DWC2_PORT_IDLE : st);
    CHECK(!e.enabled && !e.connect);
    u2ps_dwc2_hprt_event(U2PS_DWC2_HPRT_PENCHNG | U2PS_DWC2_HPRT_PCSTS | U2PS_DWC2_HPRT_POCA |
                           U2PS_DWC2_HPRT_POCCHNG,
                         st, &e);
    CHECK_EQ(e.disabled, on);
    CHECK(!e.babble);
    /* A disconnect (PCSTS 0) is DISCINT's: no disable reported, the port
     * is no longer enabled */
    u2ps_dwc2_hprt_event(U2PS_DWC2_HPRT_PENCHNG, st, &e);
    CHECK(!e.disabled);
    CHECK(!e.babble);
    CHECK_EQ(e.state, on ? U2PS_DWC2_PORT_IDLE : st);
  }
}

/* The reset's end (u2ps_hport_reset(false)): an enable kept from PRST is
 * reported now; otherwise the next PENCHNG reports it */
static void test_reset_end(void)
{
  for (uint8_t st = 0; st < PORT_STATES; st++) {
    bool now = true;
    uint8_t next = u2ps_dwc2_hprt_reset_end(st, &now);
    if (st == U2PS_DWC2_PORT_RESET) {
      CHECK_EQ(next, U2PS_DWC2_PORT_RESET_END);
      CHECK(!now);
    } else if (st == U2PS_DWC2_PORT_RESET_ENABLED) {
      CHECK_EQ(next, U2PS_DWC2_PORT_ENABLED);
      CHECK(now);
    } else {
      CHECK_EQ(next, st);
      CHECK(!now);
    }
  }
  /* Both orders give one enable: PENCHNG after the PRST clear, and before */
  u2ps_dwc2_hprt_event_t e;
  uint32_t v = U2PS_DWC2_HPRT_PENCHNG | U2PS_DWC2_HPRT_PENA | U2PS_DWC2_HPRT_PCSTS;
  bool now = true;
  uint8_t st = u2ps_dwc2_hprt_reset_end(U2PS_DWC2_PORT_RESET, &now);
  CHECK(!now);
  u2ps_dwc2_hprt_event(v, st, &e);
  CHECK(e.enabled);
  CHECK_EQ(e.state, U2PS_DWC2_PORT_ENABLED);
  u2ps_dwc2_hprt_event(v, U2PS_DWC2_PORT_RESET, &e);
  CHECK(!e.enabled);
  st = u2ps_dwc2_hprt_reset_end(e.state, &now);
  CHECK(now);
  CHECK_EQ(st, U2PS_DWC2_PORT_ENABLED);
  /* A second PENCHNG with PENA, the enable already reported: nothing */
  u2ps_dwc2_hprt_event(v, st, &e);
  CHECK(!e.enabled);
  CHECK_EQ(e.state, U2PS_DWC2_PORT_ENABLED);
}

/* ---- The stream's channels --------------------------------------------------------- */

static void test_iso_regs(void)
{
  unsigned bad = 0;
  static const uint16_t mps[] = {1, 4, 776, 1023, 1024};
  for (unsigned addr = 0; addr < 128u; addr++)
    for (unsigned ep = 1; ep < 16u; ep++)
      for (unsigned m = 0; m < sizeof mps / sizeof mps[0]; m++)
        for (unsigned in = 0; in < 2u; in++) {
          uint32_t v = u2ps_dwc2_hcchar_iso((uint8_t)addr, (uint8_t)ep, mps[m], in != 0u);
          uint32_t want = (uint32_t)addr << 22 | 1u << 20 | 1u << 18 | (in ? 1u << 15 : 0u) |
                          (uint32_t)ep << 11 | mps[m];
          bad += v != want;
        }
  CHECK_EQ(bad, 0);
  for (unsigned len = 0; len <= 1024u; len++) {
    uint32_t v = u2ps_dwc2_hctsiz_iso((uint16_t)len);
    bad += dpid(v) != 0u || pktcnt(v) != 1u || xfrsiz(v) != len || (v >> 31) != 0u;
  }
  CHECK_EQ(bad, 0);
  /* The bytes an IN transfer received: XFRSIZ counts down; DPID and PKTCNT
   * bits are not counted; at most cap */
  CHECK_EQ(u2ps_dwc2_hctsiz_in_len(64, 46, 64), 18);
  CHECK_EQ(u2ps_dwc2_hctsiz_in_len(64, 3u << 29 | 1u << 19 | 60u, 64), 4);
  CHECK_EQ(u2ps_dwc2_hctsiz_in_len(64, 0, 64), 64);
  CHECK_EQ(u2ps_dwc2_hctsiz_in_len(64, 0, 18), 18);
  CHECK_EQ(u2ps_dwc2_hctsiz_in_len(64, 64, 64), 0);
  CHECK_EQ(u2ps_dwc2_hctsiz_in_len(64, 100, 64), 0);
  CHECK_EQ(u2ps_dwc2_hctsiz_in_len(8184, 8084, 9000), 100);
}

/* ---- The control transfer ------------------------------------------------------------ */

/* OTG_HCTSIZx as the core leaves it at a halt */
static uint32_t hz(uint32_t pid, uint32_t packets, uint32_t size)
{
  return pid << 29 | packets << 19 | size;
}

#define XFRC (U2PS_DWC2_HCINT_XFRC | U2PS_DWC2_HCINT_CHH)
#define HALT(bit) ((bit) | U2PS_DWC2_HCINT_CHH)

static void request(uint8_t setup[8], uint8_t bm, uint16_t length)
{
  static const uint8_t base[8] = {0, 6, 0, 1, 0, 0, 0, 0};
  memcpy(setup, base, 8);
  setup[0] = bm;
  setup[6] = (uint8_t)length;
  setup[7] = (uint8_t)(length >> 8);
}

static u2ps_dwc2_ctl_step_t step(u2ps_dwc2_ctl_t *c, uint32_t hcint, uint32_t hctsiz)
{
  u2ps_dwc2_ctl_step_t s = {.what = 0xEE, .stage = 0xEE, .result = 0xEE, .len = 0xEEEE};
  u2ps_dwc2_ctl_chh(c, hcint, hctsiz, &s);
  return s;
}

static bool arms(u2ps_dwc2_ctl_step_t s, u2ps_dwc2_stage_t stage, bool in, uint32_t hctsiz)
{
  return s.what == U2PS_DWC2_CTL_ARM && s.stage == stage && s.in == in && s.hctsiz == hctsiz;
}

static bool done(u2ps_dwc2_ctl_step_t s, u2ps_dwc2_ctl_result_t result, uint16_t len)
{
  return s.what == U2PS_DWC2_CTL_DONE && s.result == result && s.len == len;
}

static uint32_t setup_tsiz(uint8_t mps0)
{
  return u2ps_dwc2_hctsiz_ctl(U2PS_DWC2_STAGE_SETUP, false, 8, mps0);
}

/* The status stage: OUT, zero bytes; IN, zero bytes in one packet of mps0
 * reserved (73.14.35) */
static const uint32_t STATUS_TSIZ = 2u << 29 | 1u << 19;

static uint32_t status_in(uint8_t mps0)
{
  return 2u << 29 | 1u << 19 | mps0;
}

/* A control read of 18 bytes, a control write of 4, a request without a
 * data stage: the stages in order, their directions, the bytes */
static void test_ctl_stages(void)
{
  static const uint8_t mps[4] = {8, 16, 32, 64};
  for (unsigned m = 0; m < 4u; m++) {
    uint8_t mp = mps[m];
    u2ps_dwc2_ctl_t c;
    memset(&c, 0, sizeof c);
    uint8_t setup[8];
    u2ps_dwc2_ctl_step_t s;
    /* GET_DESCRIPTOR(device, 18) */
    request(setup, 0x80, 18);
    u2ps_dwc2_ctl_start(&c, setup, mp, false, &s);
    CHECK(arms(s, U2PS_DWC2_STAGE_SETUP, false, setup_tsiz(mp)));
    CHECK(c.busy);
    s = step(&c, XFRC, hz(2, 0, 0));
    uint32_t rounded = (18u + mp - 1u) / mp * mp;
    uint32_t in18 = u2ps_dwc2_hctsiz_ctl(U2PS_DWC2_STAGE_DATA_IN, true, 18, mp);
    CHECK(arms(s, U2PS_DWC2_STAGE_DATA_IN, true, in18));
    CHECK_EQ(s.offset, 0);
    s = step(&c, XFRC, hz(0, 0, rounded - 18u));
    CHECK(arms(s, U2PS_DWC2_STAGE_STATUS, false, STATUS_TSIZ));
    s = step(&c, XFRC, hz(0, 0, 0));
    CHECK(done(s, U2PS_DWC2_CTL_OK, 18));
    CHECK(!c.busy);
    /* SET_ADDRESS: the status stage IN at once */
    request(setup, 0x00, 0);
    u2ps_dwc2_ctl_start(&c, setup, mp, false, &s);
    CHECK(arms(s, U2PS_DWC2_STAGE_SETUP, false, setup_tsiz(mp)));
    s = step(&c, XFRC, 0);
    CHECK(arms(s, U2PS_DWC2_STAGE_STATUS, true, status_in(mp)));
    s = step(&c, XFRC, hz(2, 0, mp));
    CHECK(done(s, U2PS_DWC2_CTL_OK, 0));
    /* SET_CUR of 4 bytes: data OUT, status IN */
    request(setup, 0x21, 4);
    u2ps_dwc2_ctl_start(&c, setup, mp, false, &s);
    s = step(&c, XFRC, 0);
    CHECK(arms(s, U2PS_DWC2_STAGE_DATA_OUT, false, 2u << 29 | 1u << 19 | 4u));
    CHECK_EQ(s.offset, 0);
    s = step(&c, XFRC, hz(0, 0, 0));
    CHECK(arms(s, U2PS_DWC2_STAGE_STATUS, true, status_in(mp)));
    s = step(&c, XFRC, 0);
    CHECK(done(s, U2PS_DWC2_CTL_OK, 4));
    /* A zero-length data packet ends a control read with no bytes */
    request(setup, 0x80, 18);
    u2ps_dwc2_ctl_start(&c, setup, mp, false, &s);
    (void)step(&c, XFRC, 0);
    s = step(&c, XFRC, hz(2, 0, rounded));
    CHECK(arms(s, U2PS_DWC2_STAGE_STATUS, false, STATUS_TSIZ));
    s = step(&c, XFRC, 0);
    CHECK(done(s, U2PS_DWC2_CTL_OK, 0));
    /* A device that sends more than wLength: the length stays wLength */
    request(setup, 0x80, 18);
    u2ps_dwc2_ctl_start(&c, setup, mp, false, &s);
    (void)step(&c, XFRC, 0);
    (void)step(&c, XFRC, hz(0, 0, 0));
    s = step(&c, XFRC, 0);
    CHECK(done(s, U2PS_DWC2_CTL_OK, 18));
    CHECK_EQ(c.faults, 0);
  }
}

/* STALL at each stage ends the transfer; the bytes moved before count */
static void test_ctl_stall(void)
{
  u2ps_dwc2_ctl_t c;
  memset(&c, 0, sizeof c);
  uint8_t setup[8];
  u2ps_dwc2_ctl_step_t s;
  request(setup, 0x80, 18);
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  s = step(&c, HALT(U2PS_DWC2_HCINT_STALL), 0);
  CHECK(done(s, U2PS_DWC2_CTL_STALL, 0));
  CHECK(!c.busy);
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  (void)step(&c, XFRC, 0);
  s = step(&c, HALT(U2PS_DWC2_HCINT_STALL), hz(2, 1, 64));
  CHECK(done(s, U2PS_DWC2_CTL_STALL, 0));
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  (void)step(&c, XFRC, 0);
  (void)step(&c, XFRC, hz(0, 0, 46));
  s = step(&c, HALT(U2PS_DWC2_HCINT_STALL), 0);
  CHECK(done(s, U2PS_DWC2_CTL_STALL, 18));
  CHECK_EQ(c.retries, 0);
}

/* Transaction errors (USB 2.0 4.5.2, 10.2.6): the third in a row ends the
 * transfer, so U2PS_DWC2_CTL_STRIKES - 1 are re-armed; data moved restarts
 * the count and is not sent again */
static void test_ctl_errors(void)
{
  CHECK_EQ(U2PS_DWC2_CTL_STRIKES, 3);
  static const uint32_t errs[3] = {U2PS_DWC2_HCINT_TXERR, U2PS_DWC2_HCINT_BBERR,
                                   U2PS_DWC2_HCINT_FRMOR};
  for (unsigned e = 0; e < 3u; e++) {
    u2ps_dwc2_ctl_t c;
    memset(&c, 0, sizeof c);
    uint8_t setup[8];
    u2ps_dwc2_ctl_step_t s;
    request(setup, 0x80, 18);
    u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
    for (unsigned i = 0; i + 1u < U2PS_DWC2_CTL_STRIKES; i++) {
      s = step(&c, HALT(errs[e]), setup_tsiz(64));
      CHECK(arms(s, U2PS_DWC2_STAGE_SETUP, false, setup_tsiz(64)));
    }
    s = step(&c, HALT(errs[e]), setup_tsiz(64));
    CHECK(done(s, U2PS_DWC2_CTL_ERROR, 0));
    CHECK_EQ(c.retries, U2PS_DWC2_CTL_STRIKES - 1u);
    CHECK(!c.busy);
    /* A success in between restarts the count */
    u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
    (void)step(&c, HALT(errs[e]), setup_tsiz(64));
    (void)step(&c, HALT(errs[e]), setup_tsiz(64));
    s = step(&c, XFRC, 0);
    CHECK(arms(s, U2PS_DWC2_STAGE_DATA_IN, true, 2u << 29 | 1u << 19 | 64u));
    for (unsigned i = 0; i + 1u < U2PS_DWC2_CTL_STRIKES; i++)
      CHECK_EQ(step(&c, HALT(errs[e]), hz(2, 1, 64)).what, U2PS_DWC2_CTL_ARM);
    CHECK(done(step(&c, HALT(errs[e]), hz(2, 1, 64)), U2PS_DWC2_CTL_ERROR, 0));
    /* A halt with NAK alone restarts it (10.2.6: a NAK is no transmission
     * error), and so does DTERR (the sample routine of RM0456 p. 3411) */
    static const uint32_t resets[2] = {U2PS_DWC2_HCINT_NAK, U2PS_DWC2_HCINT_DTERR};
    for (unsigned r = 0; r < 2u; r++) {
      u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
      (void)step(&c, XFRC, 0);
      for (unsigned k = 0; k < 4u; k++) {
        for (unsigned i = 0; i + 1u < U2PS_DWC2_CTL_STRIKES; i++)
          CHECK_EQ(step(&c, HALT(errs[e]), hz(2, 1, 64)).what, U2PS_DWC2_CTL_ARM);
        s = step(&c, HALT(resets[r]), hz(2, 1, 64));
        CHECK(arms(s, U2PS_DWC2_STAGE_DATA_IN, true, 2u << 29 | 1u << 19 | 64u));
      }
      for (unsigned i = 0; i + 1u < U2PS_DWC2_CTL_STRIKES; i++)
        CHECK_EQ(step(&c, HALT(errs[e]), hz(2, 1, 64)).what, U2PS_DWC2_CTL_ARM);
      CHECK(done(step(&c, HALT(errs[e]), hz(2, 1, 64)), U2PS_DWC2_CTL_ERROR, 0));
    }
  }

  /* DTERR alone never ends the transfer: each one re-arms what is left
   * and counts in retries */
  {
    u2ps_dwc2_ctl_t c;
    memset(&c, 0, sizeof c);
    uint8_t setup[8];
    u2ps_dwc2_ctl_step_t s;
    request(setup, 0x80, 18);
    u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
    (void)step(&c, XFRC, 0);
    for (unsigned i = 0; i < 50u; i++) {
      s = step(&c, HALT(U2PS_DWC2_HCINT_DTERR), hz(2, 1, 64));
      CHECK(arms(s, U2PS_DWC2_STAGE_DATA_IN, true, 2u << 29 | 1u << 19 | 64u));
    }
    CHECK_EQ(c.retries, 50);
    CHECK(arms(step(&c, XFRC, hz(0, 0, 46)), U2PS_DWC2_STAGE_STATUS, false, STATUS_TSIZ));
    CHECK(done(step(&c, XFRC, 0), U2PS_DWC2_CTL_OK, 18));
  }

  /* A control write of 200 bytes, packets of 64: 2 packets sent before an
   * error; the rest from byte 128, DATA1 (two packets moved), the count
   * restarted */
  u2ps_dwc2_ctl_t c;
  memset(&c, 0, sizeof c);
  uint8_t setup[8];
  u2ps_dwc2_ctl_step_t s;
  request(setup, 0x21, 200);
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  s = step(&c, XFRC, 0);
  CHECK(arms(s, U2PS_DWC2_STAGE_DATA_OUT, false, 2u << 29 | 4u << 19 | 200u));
  s = step(&c, HALT(U2PS_DWC2_HCINT_TXERR), hz(2, 4, 200));
  CHECK(arms(s, U2PS_DWC2_STAGE_DATA_OUT, false, 2u << 29 | 4u << 19 | 200u));
  CHECK_EQ(s.offset, 0);
  s = step(&c, HALT(U2PS_DWC2_HCINT_TXERR), hz(2, 4, 200));
  s = step(&c, HALT(U2PS_DWC2_HCINT_TXERR), hz(2, 2, 72));
  CHECK(arms(s, U2PS_DWC2_STAGE_DATA_OUT, false, 2u << 29 | 2u << 19 | 72u));
  CHECK_EQ(s.offset, 128);
  /* One packet more, then errors: DATA0 from byte 192 */
  s = step(&c, HALT(U2PS_DWC2_HCINT_TXERR), hz(0, 1, 8));
  CHECK(arms(s, U2PS_DWC2_STAGE_DATA_OUT, false, 0u << 29 | 1u << 19 | 8u));
  CHECK_EQ(s.offset, 192);
  CHECK_EQ(step(&c, HALT(U2PS_DWC2_HCINT_TXERR), hz(0, 1, 8)).what, U2PS_DWC2_CTL_ARM);
  CHECK(done(step(&c, HALT(U2PS_DWC2_HCINT_TXERR), hz(0, 1, 8)), U2PS_DWC2_CTL_ERROR, 192));

  /* A control read of 200 bytes: one packet received before an error; the
   * rest from byte 64, DATA0, 3 packets, 192 bytes reserved */
  request(setup, 0x80, 200);
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  s = step(&c, XFRC, 0);
  CHECK(arms(s, U2PS_DWC2_STAGE_DATA_IN, true, 2u << 29 | 4u << 19 | 256u));
  s = step(&c, HALT(U2PS_DWC2_HCINT_TXERR), hz(0, 3, 192));
  CHECK(arms(s, U2PS_DWC2_STAGE_DATA_IN, true, 0u << 29 | 3u << 19 | 192u));
  CHECK_EQ(s.offset, 64);
  s = step(&c, XFRC, hz(2, 0, 192 - 136));
  CHECK(arms(s, U2PS_DWC2_STAGE_STATUS, false, STATUS_TSIZ));
  CHECK(done(step(&c, XFRC, 0), U2PS_DWC2_CTL_OK, 200));

  /* The status stage retried as itself */
  request(setup, 0x00, 0);
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  (void)step(&c, XFRC, 0);
  s = step(&c, HALT(U2PS_DWC2_HCINT_TXERR), status_in(64));
  CHECK(arms(s, U2PS_DWC2_STAGE_STATUS, true, status_in(64)));
}

/* NAK: re-armed without limit and without an error; AHBERR ends it */
static void test_ctl_nak_ahberr(void)
{
  u2ps_dwc2_ctl_t c;
  memset(&c, 0, sizeof c);
  uint8_t setup[8];
  u2ps_dwc2_ctl_step_t s;
  request(setup, 0x80, 18);
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  (void)step(&c, XFRC, 0);
  for (unsigned i = 0; i < 100u; i++) {
    s = step(&c, HALT(U2PS_DWC2_HCINT_NAK), hz(2, 1, 64));
    CHECK(arms(s, U2PS_DWC2_STAGE_DATA_IN, true, 2u << 29 | 1u << 19 | 64u));
  }
  CHECK_EQ(c.naks, 100);
  CHECK_EQ(c.retries, 0);
  /* Errors after the NAKs: the full count of retries */
  for (unsigned i = 0; i + 1u < U2PS_DWC2_CTL_STRIKES; i++)
    CHECK_EQ(step(&c, HALT(U2PS_DWC2_HCINT_TXERR), hz(2, 1, 64)).what, U2PS_DWC2_CTL_ARM);
  s = step(&c, HALT(U2PS_DWC2_HCINT_AHBERR), hz(2, 1, 64));
  CHECK(done(s, U2PS_DWC2_CTL_ERROR, 0));
  CHECK_EQ(c.faults, 1);
}

/* Abort: ABORTED, unless the transfer ended first */
static void test_ctl_abort(void)
{
  u2ps_dwc2_ctl_t c;
  memset(&c, 0, sizeof c);
  uint8_t setup[8];
  u2ps_dwc2_ctl_step_t s;
  request(setup, 0x80, 18);
  CHECK(!u2ps_dwc2_ctl_abort(&c)); /* nothing on the channel */
  /* The halt of the disable itself */
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  (void)step(&c, XFRC, 0);
  CHECK(u2ps_dwc2_ctl_abort(&c));
  s = step(&c, U2PS_DWC2_HCINT_CHH, hz(2, 1, 64));
  CHECK(done(s, U2PS_DWC2_CTL_ABORTED, 0));
  CHECK(!c.busy);
  /* A stage that completed: no next stage */
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  CHECK(u2ps_dwc2_ctl_abort(&c));
  CHECK(done(step(&c, XFRC, 0), U2PS_DWC2_CTL_ABORTED, 0));
  /* An error or a NAK: not retried */
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  CHECK(u2ps_dwc2_ctl_abort(&c));
  CHECK(done(step(&c, HALT(U2PS_DWC2_HCINT_TXERR), 0), U2PS_DWC2_CTL_ABORTED, 0));
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  CHECK(u2ps_dwc2_ctl_abort(&c));
  CHECK(done(step(&c, HALT(U2PS_DWC2_HCINT_NAK), 0), U2PS_DWC2_CTL_ABORTED, 0));
  /* The transfer ended first: its status stage completed, or a STALL */
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  (void)step(&c, XFRC, 0);
  (void)step(&c, XFRC, hz(0, 0, 46));
  CHECK(u2ps_dwc2_ctl_abort(&c));
  CHECK(done(step(&c, XFRC, 0), U2PS_DWC2_CTL_OK, 18));
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  CHECK(u2ps_dwc2_ctl_abort(&c));
  CHECK(done(step(&c, HALT(U2PS_DWC2_HCINT_STALL), 0), U2PS_DWC2_CTL_STALL, 0));
  /* The next transfer is not aborted */
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  CHECK(arms(step(&c, XFRC, 0), U2PS_DWC2_STAGE_DATA_IN, true, 2u << 29 | 1u << 19 | 64u));
}

/* The SETUP stage alone, then the data and status stages */
static void test_ctl_split(void)
{
  u2ps_dwc2_ctl_t c;
  memset(&c, 0, sizeof c);
  uint8_t setup[8];
  u2ps_dwc2_ctl_step_t s;
  /* Data and status without a SETUP stage held: an error */
  u2ps_dwc2_ctl_data(&c, &s);
  CHECK(done(s, U2PS_DWC2_CTL_ERROR, 0));
  CHECK_EQ(c.faults, 1);
  request(setup, 0x80, 18);
  u2ps_dwc2_ctl_start(&c, setup, 64, true, &s);
  CHECK(arms(s, U2PS_DWC2_STAGE_SETUP, false, setup_tsiz(64)));
  s = step(&c, XFRC, 0);
  CHECK(done(s, U2PS_DWC2_CTL_OK, 0));
  CHECK(c.held);
  CHECK(!c.busy);
  u2ps_dwc2_ctl_data(&c, &s);
  CHECK(arms(s, U2PS_DWC2_STAGE_DATA_IN, true, 2u << 29 | 1u << 19 | 64u));
  CHECK(!c.held);
  CHECK(arms(step(&c, XFRC, hz(0, 0, 46)), U2PS_DWC2_STAGE_STATUS, false, STATUS_TSIZ));
  CHECK(done(step(&c, XFRC, 0), U2PS_DWC2_CTL_OK, 18));
  /* Without a data stage: the status stage */
  request(setup, 0x00, 0);
  u2ps_dwc2_ctl_start(&c, setup, 64, true, &s);
  CHECK(done(step(&c, XFRC, 0), U2PS_DWC2_CTL_OK, 0));
  u2ps_dwc2_ctl_data(&c, &s);
  CHECK(arms(s, U2PS_DWC2_STAGE_STATUS, true, status_in(64)));
  /* A SETUP stage that failed holds nothing */
  request(setup, 0x80, 18);
  u2ps_dwc2_ctl_start(&c, setup, 64, true, &s);
  (void)step(&c, XFRC, 0); /* the status stage of the request before */
  u2ps_dwc2_ctl_start(&c, setup, 64, true, &s);
  CHECK(done(step(&c, HALT(U2PS_DWC2_HCINT_STALL), 0), U2PS_DWC2_CTL_STALL, 0));
  CHECK(!c.held);
  u2ps_dwc2_ctl_data(&c, &s);
  CHECK(done(s, U2PS_DWC2_CTL_ERROR, 0));
}

/* A data stage longer than 1023 packets, in parts; a short packet ends it */
static void test_ctl_parts(void)
{
  u2ps_dwc2_ctl_t c;
  memset(&c, 0, sizeof c);
  uint8_t setup[8];
  u2ps_dwc2_ctl_step_t s;
  /* 9000 bytes OUT in packets of 8: 1023 packets, then 102 from DATA0 */
  request(setup, 0x21, 9000);
  u2ps_dwc2_ctl_start(&c, setup, 8, false, &s);
  s = step(&c, XFRC, 0);
  CHECK(arms(s, U2PS_DWC2_STAGE_DATA_OUT, false, 2u << 29 | 1023u << 19 | 8184u));
  s = step(&c, XFRC, hz(0, 0, 0));
  CHECK(arms(s, U2PS_DWC2_STAGE_DATA_OUT, false, 0u << 29 | 102u << 19 | 816u));
  CHECK_EQ(s.offset, 8184);
  CHECK(arms(step(&c, XFRC, hz(2, 0, 0)), U2PS_DWC2_STAGE_STATUS, true, status_in(8)));
  CHECK(done(step(&c, XFRC, 0), U2PS_DWC2_CTL_OK, 9000));
  /* The same IN */
  request(setup, 0x80, 9000);
  u2ps_dwc2_ctl_start(&c, setup, 8, false, &s);
  s = step(&c, XFRC, 0);
  CHECK(arms(s, U2PS_DWC2_STAGE_DATA_IN, true, 2u << 29 | 1023u << 19 | 8184u));
  s = step(&c, XFRC, hz(0, 0, 0));
  CHECK(arms(s, U2PS_DWC2_STAGE_DATA_IN, true, 0u << 29 | 102u << 19 | 816u));
  CHECK_EQ(s.offset, 8184);
  CHECK(arms(step(&c, XFRC, hz(2, 0, 0)), U2PS_DWC2_STAGE_STATUS, false, STATUS_TSIZ));
  CHECK(done(step(&c, XFRC, 0), U2PS_DWC2_CTL_OK, 9000));
  /* A short packet in the first part ends the data stage */
  u2ps_dwc2_ctl_start(&c, setup, 8, false, &s);
  (void)step(&c, XFRC, 0);
  s = step(&c, XFRC, hz(0, 0, 8184 - 100));
  CHECK(arms(s, U2PS_DWC2_STAGE_STATUS, false, STATUS_TSIZ));
  CHECK(done(step(&c, XFRC, 0), U2PS_DWC2_CTL_OK, 100));
  /* Every length at packets of 8 and 64, the whole data stage moved part
   * by part: no part over 1023 packets, the parts adding up to wLength,
   * each part's data PID from the packets before it */
  unsigned bad = 0;
  static const uint8_t mps[2] = {8, 64};
  for (unsigned m = 0; m < 2u; m++)
    for (uint32_t len = 1; len <= 65535u; len += 97u)
      for (unsigned in = 0; in < 2u; in++) {
        request(setup, in ? 0x80 : 0x21, (uint16_t)len);
        u2ps_dwc2_ctl_start(&c, setup, mps[m], false, &s);
        s = step(&c, XFRC, 0);
        uint32_t moved = 0;
        unsigned parts = 0;
        while (s.what == U2PS_DWC2_CTL_ARM && s.stage != U2PS_DWC2_STAGE_STATUS && parts < 100u) {
          uint32_t n = pktcnt(s.hctsiz), size = xfrsiz(s.hctsiz);
          uint32_t want_pid = (moved / mps[m]) % 2u == 0u ? 2u : 0u;
          bad += n == 0u || n > 1023u || s.offset != moved || dpid(s.hctsiz) != want_pid;
          uint32_t part = in ? (len - moved < n * mps[m] ? len - moved : n * mps[m]) : size;
          moved += part;
          s = step(&c, XFRC, hz(0, 0, in ? size - part : 0));
          parts++;
        }
        bad += !arms(s, U2PS_DWC2_STAGE_STATUS, !in, in ? STATUS_TSIZ : status_in(mps[m])) ||
               moved != len;
        bad += !done(step(&c, XFRC, 0), U2PS_DWC2_CTL_OK, (uint16_t)len);
      }
  CHECK_EQ(bad, 0);
}

/* Calls the hardware cannot cause */
static void test_ctl_faults(void)
{
  u2ps_dwc2_ctl_t c;
  memset(&c, 0, sizeof c);
  uint8_t setup[8];
  u2ps_dwc2_ctl_step_t s;
  CHECK_EQ(step(&c, XFRC, 0).what, U2PS_DWC2_CTL_NONE);
  CHECK_EQ(c.faults, 1);
  request(setup, 0x80, 18);
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s);
  u2ps_dwc2_ctl_start(&c, setup, 64, false, &s); /* a second while busy */
  CHECK_EQ(s.what, U2PS_DWC2_CTL_NONE);
  CHECK_EQ(c.faults, 2);
  CHECK(arms(step(&c, XFRC, 0), U2PS_DWC2_STAGE_DATA_IN, true, 2u << 29 | 1u << 19 | 64u));
  /* Dropped (disable, disconnect): nothing follows */
  u2ps_dwc2_ctl_drop(&c);
  CHECK(!c.busy);
  CHECK_EQ(step(&c, XFRC, 0).what, U2PS_DWC2_CTL_NONE);
  CHECK_EQ(c.faults, 3);
  u2ps_dwc2_ctl_start(&c, setup, 64, true, &s);
  (void)step(&c, XFRC, 0);
  CHECK(c.held);
  u2ps_dwc2_ctl_drop(&c);
  CHECK(!c.held);
}

int main(void)
{
  test_hframe();
  test_sent_uframe();
  test_late_arm();
  test_frmor();
  test_no_buffer();
  test_ahberr();
  test_states();
  test_polls();
  test_poll_window();
  test_stream_fits();
  test_fifo();
  test_hprt_write();
  test_hprt_fields();
  test_hcchar_ep0();
  test_hctsiz_ctl();
  test_status_direction();
  test_hprt_event();
  test_reset_end();
  test_iso_regs();
  test_ctl_stages();
  test_ctl_stall();
  test_ctl_errors();
  test_ctl_nak_ahberr();
  test_ctl_abort();
  test_ctl_split();
  test_ctl_parts();
  test_ctl_faults();
  return check_done();
}
