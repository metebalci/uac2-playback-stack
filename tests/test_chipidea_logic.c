/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_chipidea_logic.c: the ChipIdea device port's decision logic, without
 * registers, against a simulated controller and dTD queue:
 * - dTD tokens, the dQH capabilities word, the ENDPT* register bits, and
 *   the port's configuration constants;
 * - the reserve: the stack holds at most count - 2, a packet that would
 *   leave fewer free is NO_BUFFER in its own microframe and its slot
 *   queued again, so the endpoint never runs dry while the stack holds, in
 *   every Frame Number unit, also with a host pause inside the stall; the
 *   tail at the stream's end;
 * - append, dry and re-prime: a release in thread that finds retired dTDs
 *   not yet taken (LATE after them), a lost prime and an overflow (LATE),
 *   and the release order;
 * - holes from the Frame Number in microframes (exact, before the packet
 *   that ends them, with exact microframe numbers) and in frames (exact
 *   per frame);
 * - the fallback count: interrupts entering one microframe late, singly
 *   (no hole reported) and in runs, several dTDs retired per interrupt,
 *   ITC 8, a real hole next to a late interrupt, and long random runs
 *   (totals within one microframe, every over-report counted in
 *   fallback_over, and errors 0);
 * - CORRUPTED from Transaction Error, the slot queued again;
 * - the close with dTDs queued, flushes that fail and are repeated;
 * - the feedback endpoint's two-dTD rotation and its dry re-prime;
 * - every unit across the 0x3FFF wrap of microframe numbers and the 0x7FF
 *   wrap of the Frame Number field.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "u2ps_chipidea_iso.h"

/* The port's configuration header */
#include "../port/chipidea/u2ps_dport_cfg.h"

#define MASK  U2PS_UFRAME_MASK
#define NONE  U2PS_CHIPIDEA_FN_NONE
#define UFR   U2PS_CHIPIDEA_FN_UFRAMES
#define FRM   U2PS_CHIPIDEA_FN_FRAMES
#define NO    U2PS_CHIPIDEA_NO_SLOT
#define MPS   776u
#define RUN   40000u

/* ---- Descriptors, registers and constants ----------------------------------- */

static void test_tokens(void)
{
  uint32_t t = u2ps_chipidea_dtd_token(776, 0);
  CHECK_EQ(t, 0x03088080u); /* Total Bytes 776, IOC, Active */
  CHECK_EQ(u2ps_chipidea_dtd_left(t), 776);
  CHECK_EQ(u2ps_chipidea_dtd_token(4, 1), 0x00048480u); /* feedback: MultO 1 */
  CHECK_EQ(u2ps_chipidea_dtd_token(0x5000, 0) >> 16, 0x4000u);
  CHECK_EQ(u2ps_chipidea_dtd_left(0xFFFF0000u), 0x7FFF);
  CHECK_EQ(u2ps_chipidea_dqh_iso_caps(776), 0x63080000u); /* Mult 1, ZLT, MPS 776 */
  CHECK_EQ(u2ps_chipidea_dqh_iso_caps(2000), 0x64000000u);
  CHECK_EQ(u2ps_chipidea_ep_bit(1, false), 0x00000002u);
  CHECK_EQ(u2ps_chipidea_ep_bit(1, true), 0x00020000u);
  CHECK_EQ(u2ps_chipidea_ep_bit(7, true), 0x00800000u);
  CHECK_EQ(u2ps_chipidea_ep_bit(8, false), 0);

  /* The configuration header */
  CHECK_EQ(U2PS_DPORT_ISO_OUT_MIN, U2PS_CHIPIDEA_ISO_OUT_MIN);
  CHECK_EQ(U2PS_DPORT_ISO_OUT_MIN, 3);
  CHECK_EQ(U2PS_DPORT_UFRAME_EXACT, 0); /* the fallback count by default */
  CHECK_EQ(U2PS_DPORT_EP_MAX, 7);
  CHECK_EQ(U2PS_DPORT_EP0_MAX, 16384);
  CHECK_EQ(U2PS_DPORT_FB_INTERVAL_MIN, U2PS_CHIPIDEA_FB_INTERVAL_MIN);
  CHECK_EQ(U2PS_DPORT_FB_INTERVAL_MAX, U2PS_CHIPIDEA_FB_INTERVAL_MAX);
  unsigned bad = 0;
  for (unsigned mps = 1; mps <= 1024u; mps++)
    if (U2PS_DPORT_STRIDE(mps) < mps || U2PS_DPORT_STRIDE(mps) % U2PS_DPORT_ALIGN != 0u ||
        U2PS_DPORT_STRIDE(mps) - mps >= U2PS_DPORT_ALIGN)
      bad++;
  CHECK_EQ(bad, 0);
}

static u2ps_chipidea_out_t out;

static void test_open(void)
{
  memset(&out, 0, sizeof out);
  CHECK(!u2ps_chipidea_out_open(&out, 2, MPS, NONE));
  CHECK(!u2ps_chipidea_out_open(&out, U2PS_CHIPIDEA_SLOT_MAX + 1u, MPS, NONE));
  CHECK(!u2ps_chipidea_out_open(&out, 4, 0, NONE));
  CHECK(!u2ps_chipidea_out_open(&out, 4, 1025, NONE));
  CHECK(!u2ps_chipidea_out_open(&out, 4, MPS, 3));
  CHECK_EQ(out.state, U2PS_CHIPIDEA_OUT_CLOSED);
  CHECK_EQ(u2ps_chipidea_out_at(&out, 0), NO);
  CHECK(u2ps_chipidea_out_open(&out, U2PS_CHIPIDEA_SLOT_MAX, 1024, FRM));
  CHECK(u2ps_chipidea_out_open(&out, 3, MPS, UFR));
  CHECK_EQ(u2ps_chipidea_out_at(&out, 0), 0);
  CHECK_EQ(u2ps_chipidea_out_at(&out, 2), 2);
  CHECK_EQ(u2ps_chipidea_out_at(&out, 3), NO);
  CHECK_EQ(out.errors, 0);
}

/* ---- The simulated controller ----------------------------------------------
 *
 * One isochronous OUT endpoint with the logic's queue as its dTD list. Each
 * microframe t (counted from the open, at absolute microframe base + t):
 * a lost prime if injected; the stack's thread (release of every held
 * packet); a handler that entered late, before the token; the host's
 * packet; the handler, at once (ITC 0) or at the ITC interval, unless it
 * is to enter in the next microframe. A prime written in t receives from
 * t + 1 (RM p. 2414). The handler and the append follow the register
 * layer's sequences of u2ps_chipidea_iso.h. */

enum { H_SEND, H_SKIP, H_CRC, H_LONG, H_SHORT };

static uint8_t host[RUN];  /* what the host does in t (H_*) */
static uint8_t late[RUN];  /* the handler due in t enters in t + 1, before its token */
static uint8_t lose[RUN];  /* the endpoint loses its prime at the start of t */
static uint8_t kick[RUN];  /* a handler pass at the start of t, for another interrupt */
static uint8_t trel[RUN];  /* the stack releases held packets in thread at the start of t:
                              that many, or every one for ALL */
#define ALL 255u
static uint8_t keep[RUN];  /* the stack keeps the packets delivered in t */
static uint8_t masked[RUN]; /* the vector is masked: no handler in t */
static uint8_t hazard[RUN]; /* a retirement in t opens the re-prime window: ENDPTSTAT
                               and ENDPTPRIME both read 0 until it closes */
static uint8_t rlag[RUN];   /* a retirement in t shows (Active 0) only from t + 1 */
static uint8_t plate[RUN];  /* a prime written in t receives only from t + 2 */
static uint8_t take_at[RUN]; /* the stack takes the port's pending holes in thread at the
                               start of t, as at a long gap (u2ps_chipidea_out_tail) */

/* One delivered packet */
typedef struct {
  uint32_t when;      /* its microframe, counted from the open */
  uint32_t rep;       /* holes reported before it */
  uint32_t cum;       /* holes reported up to and with it */
  u2ps_uframe_t uframe;
  uint16_t slot;
  uint16_t len;
} pkt_t;

static pkt_t pk[RUN];

static struct {
  /* configuration */
  int fn_unit;       /* how the controller writes the Frame Number */
  unsigned itc;      /* 0, or the interrupt interval */
  bool itc_start;    /* ITC interrupts enter at the microframe's start, before its token */
  bool plain_read;   /* the dry read without the tripwire (a negative control) */
  uint16_t lag;      /* packets the stack keeps held when it releases */
  uint32_t base;     /* absolute microframe of t = 0 */
  /* controller */
  uint32_t t;
  bool primed;
  uint32_t prime_from;
  uint16_t done;     /* retired dTDs at the queue's head that show retired */
  uint16_t unseen;   /* retired dTDs after them that do not show yet */
  bool window;       /* the re-prime window is open */
  uint32_t retries;  /* tripwire reads repeated */
  uint32_t reprimed; /* primes written to an endpoint that was still primed */
  uint32_t token[U2PS_CHIPIDEA_SLOT_MAX];
  uint16_t fn[U2PS_CHIPIDEA_SLOT_MAX];
  uint32_t when[U2PS_CHIPIDEA_SLOT_MAX];
  bool irq;
  bool late_pending;
  uint32_t last_irq;
  uint32_t prime_bad; /* primes of another dTD than the controller's next */
  uint32_t lates;     /* passes that entered late, with a packet in flight */
  uint32_t drys;      /* primes of an endpoint found dry (Case 1, or a lost prime) */
  /* stack */
  uint16_t held;
  uint32_t n;        /* packets delivered */
  uint32_t rep[4];   /* holes reported, by reason */
  uint32_t cum;
  uint32_t first_rep; /* holes reported before the first packet */
  uint32_t flushed;   /* holes taken at take_at[] since the last packet */
} S;

static u2ps_uframe_t frindex(uint32_t t)
{
  return (u2ps_uframe_t)((S.base + t) & MASK);
}

static void sim_open(int fn_unit, int logic_unit, uint16_t count, unsigned itc, uint32_t base)
{
  memset(host, 0, sizeof host);
  memset(late, 0, sizeof late);
  memset(lose, 0, sizeof lose);
  memset(kick, 0, sizeof kick);
  memset(trel, 0, sizeof trel);
  memset(keep, 0, sizeof keep);
  memset(masked, 0, sizeof masked);
  memset(hazard, 0, sizeof hazard);
  memset(rlag, 0, sizeof rlag);
  memset(plate, 0, sizeof plate);
  memset(take_at, 0, sizeof take_at);
  memset(&S, 0, sizeof S);
  memset(&out, 0, sizeof out);
  S.fn_unit = fn_unit;
  S.itc = itc;
  S.base = base;
  CHECK(u2ps_chipidea_out_open(&out, count, MPS, (uint8_t)logic_unit));
  for (uint16_t i = 0; i < count; i++)
    S.token[i] = u2ps_chipidea_dtd_token(MPS, 0);
  /* The open primes in t = 0: packets from t = 1 */
  S.primed = true;
  S.prime_from = 1;
}

static void prime(uint16_t slot)
{
  if (slot == NO)
    return;
  if (S.primed)
    S.reprimed++;
  if (slot != u2ps_chipidea_out_at(&out, S.done))
    S.prime_bad++;
  S.primed = true;
  S.prime_from = S.t + (plate[S.t] ? 2u : 1u);
}

/* The dry read: ENDPTPRIME and ENDPTSTAT under the ATDTW tripwire, which
 * the controller clears in the re-prime window; the read is repeated until
 * the tripwire holds, by which time the window has closed */
static bool dry(void)
{
  if (S.window) {
    if (S.plain_read)
      return true; /* both bits read 0 for the moment */
    S.retries++;
    S.window = false;
  }
  return !S.primed;
}

/* The append sequence */
static void append(const u2ps_chipidea_append_t *a)
{
  for (uint16_t i = 0; i < a->n; i++)
    S.token[u2ps_chipidea_out_at(&out, (uint16_t)(a->first + i))] =
        u2ps_chipidea_dtd_token(MPS, 0);
  if (a->tail != NO && !dry())
    return; /* linked while the controller still runs the list */
  S.drys++;
  prime(u2ps_chipidea_out_primed(&out, S.done, frindex(S.t)));
}

/* The stack's release: bookkeeping only; the slots are queued at the next
 * retirement */
static void release(uint16_t n)
{
  if (n == 0u)
    return;
  u2ps_chipidea_out_release(&out, n);
  S.held = (uint16_t)(S.held - n);
}

/* The device stack: records, then keeps or releases */
static void take(const u2ps_chipidea_out_packet_t *r)
{
  uint32_t h = (uint32_t)r->holes.no_packet + r->holes.late + r->holes.no_buffer + r->holes.corrupted;
  S.rep[0] += r->holes.no_packet;
  S.rep[1] += r->holes.late;
  S.rep[2] += r->holes.no_buffer;
  S.rep[3] += r->holes.corrupted;
  if (r->buf == NO) {
    CHECK_EQ(h, 0); /* holes come with the packet that follows them */
    return;
  }
  h += S.flushed; /* the holes the stack took since the last packet lie before it */
  S.flushed = 0;
  if (S.n == 0u)
    S.first_rep = h;
  else
    S.cum += h;
  pkt_t *p = &pk[S.n++];
  p->when = S.when[r->buf];
  p->rep = h;
  p->cum = S.cum;
  p->uframe = r->uframe;
  p->slot = r->buf;
  p->len = r->len;
  S.held++;
  if (!keep[S.t] && S.held > S.lag)
    release((uint16_t)(S.held - S.lag));
}

/* The stack takes the port's pending holes, in thread */
static void take_pending(void)
{
  u2ps_chipidea_holes_t h;
  u2ps_chipidea_out_tail(&out, &h);
  S.rep[0] += h.no_packet;
  S.rep[1] += h.late;
  S.rep[2] += h.no_buffer;
  S.rep[3] += h.corrupted;
  S.flushed += h.no_packet + h.late + h.no_buffer + h.corrupted;
}

static void handler(void)
{
  u2ps_chipidea_out_packet_t r;
  uint16_t n = S.done;

  if (out.state == U2PS_CHIPIDEA_OUT_CLOSED)
    return;
  u2ps_chipidea_out_pass(&out, n, frindex(S.t));
  for (uint16_t i = 0; i < n; i++) {
    uint16_t slot = u2ps_chipidea_out_at(&out, 0);
    u2ps_chipidea_out_retire(&out, S.token[slot], S.fn[slot], &r);
    S.done--;
    take(&r);
    if (r.append.n != 0u)
      append(&r.append);
  }
  /* At every entry: a lost prime with dTDs queued */
  if (out.state == U2PS_CHIPIDEA_OUT_OPEN && out.q_len > S.done + S.unseen && dry()) {
    S.drys++;
    prime(u2ps_chipidea_out_primed(&out, S.done, frindex(S.t)));
  }
  S.irq = false;
  S.last_irq = S.t;
}

static void token(void)
{
  uint32_t t = S.t;
  uint8_t h = host[t];

  uint16_t at = (uint16_t)(S.done + S.unseen);
  if (h == H_SKIP || !S.primed || t < S.prime_from || at >= out.q_len)
    return; /* skipped, or ignored by an unprimed endpoint */
  uint16_t slot = u2ps_chipidea_out_at(&out, at);
  uint32_t len = h == H_SHORT ? MPS / 2u : MPS;
  uint32_t status = 0;
  if (h == H_CRC)
    status = U2PS_CHIPIDEA_DTD_XACT_ERROR;
  if (h == H_LONG) {
    status = U2PS_CHIPIDEA_DTD_BUF_ERROR | U2PS_CHIPIDEA_DTD_HALTED;
    len = MPS;
  }
  S.token[slot] = (MPS - len) << 16 | 1u << 15 | status;
  uint32_t abs = S.base + t;
  S.fn[slot] = S.fn_unit == UFR ? (uint16_t)(abs & 0x7FFu)
             : S.fn_unit == FRM ? (uint16_t)((abs >> 3) & 0x7FFu)
                                : 0u;
  S.when[slot] = t;
  at++;
  if (h == H_LONG || at == out.q_len) {
    S.primed = false; /* halted, or the list ran out */
    S.done = (uint16_t)(S.done + S.unseen + 1u);
    S.unseen = 0;
  } else {
    /* The controller primes the next dTD */
    if (rlag[t])
      S.unseen++;
    else
      S.done++;
    if (hazard[t])
      S.window = true;
  }
  S.irq = true;
}

/* The simulation, up to microframe 'until' */
static void sim_run(uint32_t until)
{
  for (; S.t < until; S.t++) {
    uint32_t t = S.t;
    S.done = (uint16_t)(S.done + S.unseen); /* retirements of the last microframe show */
    S.unseen = 0;
    S.window = false;
    if (lose[t])
      S.primed = false;
    if (trel[t])
      release(trel[t] == ALL || trel[t] > S.held ? S.held : trel[t]);
    if (take_at[t])
      take_pending();
    if (!masked[t] && (S.late_pending || kick[t])) {
      if (S.late_pending && host[t] != H_SKIP && S.primed)
        S.lates++;
      S.late_pending = false;
      handler();
    }
    if (S.itc_start && !masked[t] && S.irq && t - S.last_irq >= S.itc)
      handler(); /* an ITC interrupt at the microframe's start */
    token();
    if (!masked[t] && S.irq && t - S.last_irq >= S.itc) {
      if (late[t])
        S.late_pending = true;
      else
        handler();
    }
  }
}

/* Holes in truth between packets i and j (i < j) */
static uint32_t true_between(uint32_t i, uint32_t j)
{
  return pk[j].when - pk[i].when - (j - i);
}

/* Packets delivered at microframes up to x, from index 0 */
static uint32_t delivered_upto(uint32_t x)
{
  uint32_t lo = 0, hi = S.n;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2u;
    if (pk[mid].when <= x)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo;
}

/* Checks for a Frame Number in microframes: every hole exact and reported
 * before the packet that ends it; microframe numbers exact */
static void check_exact(void)
{
  unsigned hole_bad = 0, uf_bad = 0;
  for (uint32_t i = 0; i < S.n; i++) {
    if (i > 0u && pk[i].rep != true_between(i - 1u, i))
      hole_bad++;
    if (pk[i].uframe != (u2ps_uframe_t)((S.base + pk[i].when) & MASK))
      uf_bad++;
  }
  CHECK_EQ(hole_bad, 0);
  CHECK_EQ(uf_bad, 0);
  CHECK_EQ(S.first_rep, 0);
}

/* Holes in truth in the first packet's frame, after it */
static uint32_t first_frame_holes(void)
{
  uint32_t abs = S.base + pk[0].when;
  uint32_t end = pk[0].when + (7u - (abs & 7u)); /* the frame's last microframe */
  return end - pk[0].when - (delivered_upto(end) - 1u);
}

/* Checks for a Frame Number in frames: before each packet, every hole up to
 * the start of its frame is reported, except those of the stream's first
 * frame, whose base is an estimate; none after the packet; the microframe
 * numbers follow the reports */
static void check_frames(void)
{
  unsigned over = 0, short_of = 0, uf_bad = 0;
  for (uint32_t i = 1; i < S.n; i++) {
    uint32_t truth = true_between(0, i);
    if (pk[i].cum > truth)
      over++;
    uint32_t abs = S.base + pk[i].when;
    int64_t start = (int64_t)(abs - (abs & 7u)) - (int64_t)S.base; /* its frame's start, from the open */
    if (start > (int64_t)pk[0].when + 1) {
      uint32_t upto = (uint32_t)start - 1u; /* holes in (when0, upto] */
      uint32_t holes = upto - pk[0].when - (delivered_upto(upto) - 1u) - first_frame_holes();
      if (pk[i].cum < holes)
        short_of++;
    }
    if ((u2ps_uframe_t)((pk[i].uframe - pk[i - 1u].uframe - 1u) & MASK) != pk[i].rep)
      uf_bad++;
  }
  CHECK_EQ(over, 0);
  CHECK_EQ(short_of, 0);
  CHECK_EQ(uf_bad, 0);
  CHECK_EQ(S.first_rep, 0);
}

/* Checks for the fallback count: never more than one microframe over the
 * truth, every over-report found by the end counted in fallback_over and
 * none in errors (an over-report is the fallback count's expected error,
 * not a fault), the total within one microframe; the microframe numbers
 * follow the reports */
static void check_fallback(void)
{
  int32_t over_max = 0;
  unsigned uf_bad = 0;
  for (uint32_t i = 1; i < S.n; i++) {
    int32_t over = (int32_t)pk[i].cum - (int32_t)true_between(0, i);
    if (over > over_max)
      over_max = over;
    if ((u2ps_uframe_t)((pk[i].uframe - pk[i - 1u].uframe - 1u) & MASK) != pk[i].rep)
      uf_bad++;
  }
  int32_t diff = (int32_t)pk[S.n - 1u].cum - (int32_t)true_between(0, S.n - 1u);
  CHECK(over_max <= 1);
  CHECK(diff <= 1 && diff >= -1);
  CHECK(diff <= (int32_t)out.fallback_over);
  CHECK_EQ(out.errors, 0);
  CHECK_EQ(uf_bad, 0);
  CHECK_EQ(S.first_rep, 0);
}

static uint32_t rng = 1;

static uint32_t rnd(uint32_t n)
{
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng % n;
}

/* ---- Append, dry and re-prime ------------------------------------------------ */

/* The reserve: the port keeps U2PS_CHIPIDEA_ISO_OUT_RESERVE (2) buffers
 * queued, so the stack holds at most count - 2. A packet that would leave
 * fewer free is a reserve packet: NO_BUFFER in its own microframe, its
 * slot queued again. The endpoint never runs dry while the stack holds. */
static void test_reserve_held(void)
{
  u2ps_chipidea_holes_t h;

  CHECK_EQ(U2PS_CHIPIDEA_ISO_OUT_RESERVE, 2);
  CHECK_EQ(U2PS_DPORT_ISO_OUT_RESERVE, U2PS_CHIPIDEA_ISO_OUT_RESERVE);
  CHECK_EQ(U2PS_DPORT_ISO_OUT_MIN, U2PS_DPORT_ISO_OUT_RESERVE + 1u);
  CHECK_EQ(U2PS_CHIPIDEA_ISO_OUT_MIN, U2PS_CHIPIDEA_ISO_OUT_RESERVE + 1u);

  /* Three slots, the stack keeps every packet: the first is delivered, the
   * packets of 2..9 are reserve packets, a release in thread at 10 ends the
   * stall, and from 11 on they are reserve packets again. In every Frame
   * Number unit, also with the pass of 3 entering a microframe late. */
  for (int u = NONE; u <= FRM + 1; u++) {
    int unit = u <= FRM ? u : UFR;
    sim_open(unit, unit, 3, 0, 1000);
    for (uint32_t t = 0; t < 40; t++)
      keep[t] = 1;
    if (u > FRM)
      late[3] = 1;
    trel[10] = ALL;
    sim_run(40);
    CHECK_EQ(S.n, 2);
    CHECK_EQ(pk[0].when, 1);
    CHECK_EQ(pk[1].when, 10);
    CHECK_EQ(pk[1].rep, 8);
    CHECK_EQ(S.rep[2], 8);
    CHECK_EQ(S.rep[0] + S.rep[1] + S.rep[3], 0);
    CHECK_EQ(out.no_buffer_total, 8u + 29u);
    CHECK_EQ(out.late_total, 0);
    CHECK_EQ(S.drys, 0); /* never dry */
    CHECK_EQ(out.q_len, 2);
    CHECK_EQ(out.errors, 0);
    CHECK_EQ(S.prime_bad, 0);
    /* The stream's end: the reserve packets of 11..39 */
    u2ps_chipidea_out_tail(&out, &h);
    CHECK_EQ(h.no_buffer, 29);
    CHECK_EQ(h.no_packet + h.late + h.corrupted, 0);
    u2ps_chipidea_out_tail(&out, &h);
    CHECK_EQ(h.no_buffer, 0);
  }

  /* A host pause of 500 microframes inside the stall stays NO_PACKET */
  for (int u = NONE; u <= FRM; u++) {
    sim_open(u, u, 3, 0, 5000);
    for (uint32_t t = 0; t < 700; t++)
      keep[t] = 1;
    for (uint32_t t = 100; t < 600; t++)
      host[t] = H_SKIP;
    trel[650] = ALL;
    sim_run(700);
    CHECK_EQ(S.n, 2);
    CHECK_EQ(pk[1].when, 650);
    CHECK_EQ(pk[1].rep, 648);
    CHECK_EQ(S.rep[2], 148); /* 2..99 and 600..649 */
    CHECK_EQ(S.rep[0], 500);
    CHECK_EQ(S.rep[1] + S.rep[3], 0);
    CHECK_EQ(S.drys, 0);
    CHECK_EQ(out.errors, 0);
  }

  /* Releases in arrival order while the stack holds two of five: the
   * slots come back in the order they were delivered */
  sim_open(UFR, UFR, 5, 0, 1000);
  S.lag = 2;
  sim_run(30);
  unsigned order_bad = 0;
  for (uint32_t i = 5; i < S.n; i++)
    if (pk[i].slot != pk[i - 5u].slot)
      order_bad++;
  CHECK_EQ(order_bad, 0);
  CHECK_EQ(S.n, 29);
  CHECK_EQ(S.rep[0] + S.rep[1] + S.rep[2] + S.rep[3], 0);
  check_exact();
}

static void test_dry_unwalked(void)
{
  /* The vector is masked while the queue runs out; a release in thread
   * only frees a slot. The pass after the mask takes the three retired
   * dTDs, and its first append finds the endpoint dry with two of them not
   * yet taken: the dry microframes are LATE, reported after those two
   * packets. Four slots: the stack holds one, and the one it releases in
   * each packet's upcall is queued at the next retirement, so two are
   * queued. */
  for (int u = NONE; u <= FRM; u++) {
    sim_open(u, u, 4, 0, 2000);
    S.lag = 1;
    for (uint32_t t = 20; t < 30; t++)
      masked[t] = 1;
    trel[25] = ALL;
    sim_run(60);
    CHECK_EQ(out.errors, 0);
    CHECK_EQ(S.prime_bad, 0);
    CHECK_EQ(S.rep[2], 0);
    /* Packets up to 21 (the queue held two at 20); dry 22..30, primed at
     * the pass in 30 */
    uint32_t i = 0;
    while (i < S.n && pk[i].when != 31u)
      i++;
    CHECK(i + 1u < S.n);
    CHECK_EQ(pk[i - 1u].when, 21);
    CHECK_EQ(pk[i - 1u].rep, 0); /* not before the packets that precede it */
    CHECK_EQ(pk[i].rep, 9);
    CHECK_EQ(pk[i + 1u].when, 32);
    CHECK_EQ(S.rep[1], 9);
    CHECK_EQ(S.drys, 1);
    if (u == UFR)
      check_exact();
    else if (u == FRM)
      check_frames();
    else
      check_fallback();
  }
}

static void test_lost_prime(void)
{
  /* The endpoint loses its prime at 20 with dTDs queued; a pass for another
   * interrupt at 25 finds it and primes again: 20..25 are LATE */
  for (int u = NONE; u <= FRM; u++) {
    sim_open(u, u, 4, 0, 3000);
    lose[20] = 1;
    kick[25] = 1;
    sim_run(60);
    uint32_t i = 0;
    while (i < S.n && pk[i].when != 26u)
      i++;
    CHECK(i < S.n);
    CHECK_EQ(pk[i - 1u].when, 19);
    CHECK_EQ(pk[i].rep, 6);
    CHECK_EQ(S.rep[1], 6);
    CHECK_EQ(S.rep[0] + S.rep[2] + S.rep[3], 0);
    CHECK_EQ(out.late_total, 6);
    CHECK_EQ(out.errors, 0);
    CHECK_EQ(S.prime_bad, 0);
  }
}

static void test_overflow(void)
{
  /* A packet longer than the dTD: Data Buffer Error halts the endpoint; its
   * slot is queued again and the endpoint primed. Its microframe is LATE,
   * and it counts in errors. */
  for (int u = NONE; u <= FRM; u++) {
    sim_open(u, u, 4, 0, 4000);
    host[30] = H_LONG;
    sim_run(80);
    uint32_t i = 0;
    while (i < S.n && pk[i].when != 31u)
      i++;
    CHECK(i < S.n);
    CHECK_EQ(pk[i - 1u].when, 29);
    CHECK_EQ(pk[i].rep, 1);
    CHECK_EQ(S.rep[1], 1);
    CHECK_EQ(S.rep[0] + S.rep[2] + S.rep[3], 0);
    CHECK_EQ(out.errors, 1);
    CHECK_EQ(S.prime_bad, 0);
    CHECK_EQ(S.n, 78); /* 1..79 less 30 */
    CHECK_EQ(out.q_len + out.h_len, 4); /* the slot is queued again */
  }
}

/* ---- CORRUPTED ---------------------------------------------------------------- */

static void test_corrupted(void)
{
  for (int u = NONE; u <= FRM; u++) {
    sim_open(u, u, 3, 0, 5000);
    host[40] = H_CRC;
    host[41] = H_CRC;
    host[60] = H_CRC;
    host[61] = H_SKIP;
    sim_run(100);
    CHECK_EQ(S.rep[3], 3);
    CHECK_EQ(out.corrupted_total, 3);
    CHECK_EQ(S.n, 99 - 4);
    uint32_t i = 0;
    while (i < S.n && pk[i].when != 42u)
      i++;
    CHECK(i < S.n);
    CHECK_EQ(pk[i].rep, 2); /* CORRUPTED 2, before the next packet */
    /* No corrupted packet is delivered */
    unsigned delivered_bad = 0;
    for (uint32_t j = 0; j < S.n; j++)
      if (pk[j].when == 40u || pk[j].when == 41u || pk[j].when == 60u)
        delivered_bad++;
    CHECK_EQ(delivered_bad, 0);
    CHECK_EQ(out.errors, 0);
    if (u == UFR)
      check_exact();
    else if (u == FRM)
      check_frames();
    else
      check_fallback();
    /* The slot came back into the queue: the stream went on with 3 slots */
    CHECK_EQ(out.q_len + out.h_len, 3);
  }
  /* A CORRUPTED first packet is counted, not reported: no hole comes
   * before the stream's first delivered packet */
  for (int u = NONE; u <= FRM; u++) {
    sim_open(u, u, 3, 0, 5000);
    host[1] = H_CRC;
    host[3] = H_SKIP;
    sim_run(40);
    CHECK_EQ(pk[0].when, 2);
    CHECK_EQ(S.first_rep, 0);
    CHECK_EQ(S.rep[3], 0);
    CHECK_EQ(out.corrupted_total, 1);
    CHECK_EQ(pk[S.n - 1u].cum, true_between(0, S.n - 1u));
  }
  /* A short packet's length */
  sim_open(UFR, UFR, 3, 0, 0);
  host[5] = H_SHORT;
  sim_run(10);
  CHECK_EQ(pk[4].when, 5);
  CHECK_EQ(pk[4].len, MPS / 2u);
  CHECK_EQ(pk[3].len, MPS);
}

/* ---- Frame Number in microframes and in frames -------------------------------- */

static void test_uframes(void)
{
  /* Host skips, late interrupts, ITC 0 and 4, across the 0x3FFF wrap and
   * the 0x7FF wrap of the field */
  for (unsigned itc = 0; itc <= 4u; itc += 4u) {
    rng = 7u + itc;
    sim_open(UFR, UFR, 16, itc, 0x4000u * 3u - 3000u);
    for (uint32_t t = 0; t < 20000u; t++) {
      if (rnd(50) == 0u)
        host[t] = H_SKIP;
      if (rnd(8) == 0u)
        late[t] = 1;
    }
    for (uint32_t t = 9000; t < 9020u; t++)
      host[t] = H_SKIP; /* a run of 20 */
    sim_run(20100);
    check_exact();
    CHECK(S.rep[0] > 300u);
    CHECK_EQ(S.rep[1] + S.rep[2] + S.rep[3], 0);
    CHECK_EQ(out.errors, 0);
  }
  /* A host pause of 9000 microframes */
  sim_open(UFR, UFR, 4, 0, 0x3F00);
  for (uint32_t t = 100; t < 9100u; t++)
    host[t] = H_SKIP;
  sim_run(9200);
  check_exact();
  CHECK_EQ(S.rep[0], 9000);
}

static void test_frames(void)
{
  for (unsigned itc = 0; itc <= 8u; itc += 8u) {
    rng = 11u + itc;
    sim_open(FRM, FRM, 16, itc, 0x4000u * 2u - 2500u);
    for (uint32_t t = 0; t < 20000u; t++) {
      if (rnd(40) == 0u)
        host[t] = H_SKIP;
      if (rnd(8) == 0u)
        late[t] = 1;
    }
    for (uint32_t t = 7003; t < 7030u; t++)
      host[t] = H_SKIP;
    sim_run(20100);
    check_frames();
    CHECK_EQ(pk[S.n - 1u].cum, true_between(0, S.n - 1u));
    CHECK(S.rep[0] > 400u);
    CHECK_EQ(out.errors, 0);
  }
  /* A frame's holes come before the first packet of the next frame: the
   * stream starts at microframe 0 of a frame (the open in the last
   * microframe of the frame before), 3 holes in the first frame */
  sim_open(FRM, FRM, 4, 0, 8u * 100u - 1u);
  host[3] = host[4] = host[6] = H_SKIP;
  sim_run(30);
  CHECK_EQ(pk[0].when, 1);
  uint32_t i = 0;
  while (i < S.n && pk[i].when != 9u)
    i++;
  CHECK(i < S.n);
  CHECK_EQ(pk[i].rep, 3);
  CHECK_EQ(pk[i - 1u].rep, 0);
  CHECK_EQ(out.errors, 0);
}

/* ---- The fallback count ------------------------------------------------------- */

static void test_fallback_single_late(void)
{
  /* One interrupt entering a microframe late now and then: no hole */
  sim_open(NONE, NONE, 8, 0, 0x3F00);
  for (uint32_t t = 10; t < 3000u; t += 7u)
    late[t] = 1;
  sim_run(3100);
  CHECK(S.lates > 400u);
  CHECK_EQ(S.rep[0] + S.rep[1] + S.rep[2] + S.rep[3], 0);
  CHECK_EQ(out.errors, 0);
  CHECK_EQ(S.n, 3099);
  check_fallback();

  /* Every other interrupt late */
  sim_open(NONE, NONE, 8, 0, 0);
  for (uint32_t t = 10; t < 3000u; t += 2u)
    late[t] = 1;
  sim_run(3100);
  CHECK(S.lates > 1400u);
  CHECK_EQ(S.rep[0], 0);
  CHECK_EQ(out.errors, 0);

  /* The stream's first interrupt late: the base moves, nothing reported */
  sim_open(NONE, NONE, 8, 0, 0);
  late[1] = 1;
  sim_run(100);
  CHECK_EQ(S.lates, 1);
  CHECK_EQ(S.rep[0], 0);
  CHECK_EQ(out.errors, 0);

  /* ITC 8: eight dTDs per interrupt, single interrupts late: one due
   * interrupt falls in each window of 8 microframes, and the next one
   * after it lies beyond the window */
  sim_open(NONE, NONE, 16, 8, 0x3000);
  for (uint32_t t = 16; t < 5000u; t++)
    if (t % 40u < 8u)
      late[t] = 1;
  sim_run(5100);
  CHECK(S.lates > 100u);
  CHECK(pk[S.n - 1u].when - pk[0].when + 1u == S.n);
  CHECK_EQ(S.rep[0], 0);
  CHECK_EQ(out.errors, 0);
}

static void test_fallback_runs(void)
{
  /* Runs of 2 to 5 late interrupts, no hole: the first run reports one
   * microframe too many, the next pass finds it and counts it in
   * fallback_over, and the later runs are absorbed by it */
  for (uint32_t len = 2; len <= 5u; len++) {
    sim_open(NONE, NONE, 8, 0, 0x3FF0);
    for (uint32_t r = 0; r < 20u; r++)
      for (uint32_t k = 0; k < len; k++)
        late[100u + r * 50u + k] = 1;
    sim_run(1200);
    CHECK_EQ(S.lates, 20u * len);
    check_fallback();
    CHECK_EQ(S.rep[0], 1);
    CHECK_EQ(out.fallback_over, 1);
    CHECK_EQ(out.errors, 0);
  }
  /* Runs, then real holes: a later hole absorbs the over-report */
  sim_open(NONE, NONE, 8, 0, 0);
  late[100] = late[101] = late[102] = 1;
  host[300] = H_SKIP;
  host[500] = host[501] = H_SKIP;
  sim_run(700);
  check_fallback();
  CHECK_EQ(S.rep[0], 3);
  CHECK_EQ(out.fallback_over, 1);
  CHECK_EQ(out.errors, 0);
}

static void test_fallback_hole_late(void)
{
  /* A real hole right after a late interrupt, and right before one */
  sim_open(NONE, NONE, 8, 0, 0x3FFA);
  late[100] = 1;
  host[101] = H_SKIP;
  host[200] = H_SKIP;
  late[201] = 1;
  host[300] = H_SKIP;
  late[299] = late[301] = 1;
  sim_run(400);
  CHECK_EQ(S.lates, 2); /* the entry in 300 has no packet in flight: 300 is the hole */
  CHECK_EQ(S.rep[0], 3);
  CHECK_EQ(out.errors, 0);
  check_fallback();
  /* Each reported before the packet after the hole or the one after it */
  uint32_t i = 0;
  while (i < S.n && pk[i].when != 102u)
    i++;
  CHECK(i + 1u < S.n);
  CHECK_EQ(pk[i].rep + pk[i + 1u].rep, 1);

  /* Several holes in one ITC 8 window, with a late entry */
  sim_open(NONE, NONE, 16, 8, 100);
  host[50] = host[52] = host[53] = H_SKIP;
  late[56] = 1;
  sim_run(200);
  CHECK_EQ(S.lates, 1);
  CHECK_EQ(S.rep[0], 3);
  CHECK_EQ(out.errors, 0);
  check_fallback();
  /* The same with every pass late, a run: at most one over, counted */
  sim_open(NONE, NONE, 16, 8, 100);
  host[50] = host[52] = host[53] = H_SKIP;
  for (uint32_t t = 40; t < 70u; t++)
    late[t] = 1;
  sim_run(200);
  CHECK(S.rep[0] == 3u || S.rep[0] == 4u);
  CHECK_EQ(out.fallback_over, S.rep[0] - 3u);
  CHECK_EQ(out.errors, 0);
  check_fallback();
}

static void test_fallback_random(void)
{
  static const unsigned itcs[3] = {0, 4, 8};
  for (unsigned k = 0; k < 9u; k++) {
    unsigned itc = itcs[k % 3u];
    rng = 101u + k;
    sim_open(NONE, NONE, 16, itc, rnd(0x4000));
    uint32_t late_div = 3u + k;
    for (uint32_t t = 0; t < 30000u; t++) {
      if (rnd(60) == 0u)
        host[t] = H_SKIP;
      if (rnd(late_div) == 0u)
        late[t] = 1;
      if (rnd(3000) == 0u)
        host[t] = H_CRC;
    }
    sim_run(30100);
    CHECK(S.lates > 300u);
    check_fallback();
    CHECK(S.rep[0] > 300u);
    CHECK_EQ(S.rep[1] + S.rep[2], 0);
  }
}

/* The units checked against a controller that writes another unit: the
 * logic must not take frames for microframes nor the reverse */
static void test_unit_mismatch(void)
{
  sim_open(FRM, UFR, 8, 0, 0);
  sim_run(200);
  CHECK(out.errors > 0u);
  sim_open(UFR, FRM, 8, 0, 0);
  sim_run(200);
  CHECK(S.rep[0] > 0u);
}


/* Two dry primes, each made while retired dTDs were still queued, before
 * a pass takes them (a lost prime found twice while the controller went on
 * retiring): each dry run is LATE, placed after its own retired dTDs */
static void test_dry_primes_pending(void)
{
  u2ps_chipidea_out_packet_t r;

  memset(&out, 0, sizeof out);
  CHECK(u2ps_chipidea_out_open(&out, 6, MPS, UFR));
  u2ps_chipidea_out_pass(&out, 1, 10);
  u2ps_chipidea_out_retire(&out, 1u << 15, 10, &r);
  CHECK_EQ(r.buf, 0);
  u2ps_chipidea_out_release(&out, 1);
  /* Slots 1 and 2 retired in 11 and 12, the prime lost; found in 15 */
  CHECK_EQ(u2ps_chipidea_out_primed(&out, 2, 15), 3);
  /* Slot 3 retired in 16, the prime lost again; found in 20 */
  CHECK_EQ(u2ps_chipidea_out_primed(&out, 3, 20), 4);
  CHECK_EQ(out.pend_len, 2);
  /* Slot 4 retired in 21; a pass in 22 takes the four */
  static const uint32_t fn[4] = {11, 12, 16, 21};
  static const uint32_t want_late[4] = {0, 0, 3, 4};
  u2ps_chipidea_out_pass(&out, 4, 22);
  for (unsigned i = 0; i < 4u; i++) {
    u2ps_chipidea_out_retire(&out, 1u << 15, fn[i], &r);
    CHECK_EQ(r.buf, (uint16_t)(i + 1u));
    CHECK_EQ(r.uframe, fn[i]);
    CHECK_EQ(r.holes.late, want_late[i]);
    CHECK_EQ(r.holes.no_packet + r.holes.no_buffer, 0);
    u2ps_chipidea_out_release(&out, 1);
  }
  CHECK_EQ(out.pend_len, 0);
  CHECK_EQ(out.late_total, 7);
  CHECK_EQ(out.errors, 0);
}

/* A host skip among the retired dTDs a dry prime finds
 * still queued. With a Frame Number unit they are placed by it: the skip is
 * NO_PACKET, the dry runs LATE, and the microframe numbers exact. Then
 * three releases while the vector stays masked, which only free: the pass
 * after the mask primes once. The stack holds one of four slots, then
 * three of six, and a released slot is queued at the next retirement. */
static void test_dry_unwalked_skip(void)
{
  for (int u = NONE; u <= FRM; u++) {
    sim_open(u, u, 4, 0, 2000);
    S.lag = 1;
    for (uint32_t t = 20; t < 30; t++)
      masked[t] = 1;
    host[21] = H_SKIP;
    trel[25] = ALL;
    sim_run(60);
    /* 20, 22 retire (21 skipped); dry 23..30 */
    CHECK_EQ(S.rep[0] + S.rep[1], 9);
    CHECK_EQ(S.rep[2] + S.rep[3], 0);
    CHECK_EQ(out.errors, 0);
    CHECK_EQ(S.prime_bad, 0);
    if (u == UFR) {
      CHECK_EQ(S.rep[1], 8);
      CHECK_EQ(S.rep[0], 1);
      check_exact();
    } else if (u == FRM) {
      check_frames();
    } else {
      check_fallback();
    }
  }
  for (int u = NONE; u <= FRM; u++) {
    sim_open(u, u, 6, 0, 0x3FF0);
    S.lag = 3;
    for (uint32_t t = 20; t < 41; t++)
      masked[t] = 1;
    trel[25] = trel[30] = trel[35] = 1;
    host[31] = H_SKIP;
    sim_run(100);
    CHECK_EQ(out.errors, 0);
    CHECK_EQ(S.prime_bad, 0);
    CHECK_EQ(S.rep[2] + S.rep[3], 0);
    if (u == UFR) {
      check_exact();
      CHECK_EQ(S.rep[0], 0); /* 31 lies in the dry run: LATE */
    } else if (u == FRM) {
      check_frames();
    } else {
      check_fallback();
    }
    CHECK_EQ(pk[S.n - 1u].cum, true_between(0, S.n - 1u));
    CHECK_EQ(S.drys, 1);
  }
}

/* A prime late in its microframe receives only from the one after (RM
 * p. 2416): that microframe counts with the dry run's reason, LATE. A
 * lost prime primed again late, in every unit. */
static void test_prime_late(void)
{
  for (int u = NONE; u <= FRM; u++) {
    sim_open(u, u, 4, 0, 3000);
    lose[20] = 1;
    kick[25] = 1;
    plate[25] = 1;
    sim_run(60);
    uint32_t i = 0;
    while (i < S.n && pk[i].when != 27u)
      i++;
    CHECK(i < S.n);
    CHECK_EQ(pk[i - 1u].when, 19);
    if (u == UFR)
      CHECK_EQ(pk[i].rep, 7); /* the others place it a pass or a frame later */
    CHECK_EQ(S.rep[1], 7); /* 20..26 */
    CHECK_EQ(S.rep[0] + S.rep[2], 0);
    CHECK_EQ(out.errors, 0);
    if (u == UFR)
      check_exact();
  }
  /* An empty microframe after a prime that was in time stays NO_PACKET
   * only once a packet has come after the prime: primed in 25, packets in
   * 26 and 27, none in 28 */
  sim_open(UFR, UFR, 4, 0, 3000);
  lose[20] = 1;
  kick[25] = 1;
  host[28] = H_SKIP;
  sim_run(60);
  CHECK_EQ(S.rep[1], 6);
  CHECK_EQ(S.rep[0], 1);
  CHECK_EQ(S.rep[2], 0);
  check_exact();
}

/* A CRC error while the stack holds all it may: its slot is queued again,
 * as a reserve packet's is, and the queue never empties */
static void test_corrupted_reserve(void)
{
  u2ps_chipidea_out_packet_t r;

  memset(&out, 0, sizeof out);
  CHECK(u2ps_chipidea_out_open(&out, 3, MPS, UFR));
  u2ps_chipidea_out_pass(&out, 1, 1);
  u2ps_chipidea_out_retire(&out, 1u << 15, 1, &r);
  CHECK_EQ(r.buf, 0);
  /* 2: a reserve packet, its slot 1 queued after slot 2 */
  u2ps_chipidea_out_pass(&out, 1, 2);
  u2ps_chipidea_out_retire(&out, 1u << 15, 2, &r);
  CHECK_EQ(r.buf, NO);
  CHECK_EQ(r.append.n, 1);
  CHECK_EQ(r.append.first, 1);
  CHECK_EQ(r.append.tail, 2);
  CHECK_EQ(u2ps_chipidea_out_at(&out, 1), 1);
  /* 3: corrupted, slot 2 queued after slot 1 */
  u2ps_chipidea_out_pass(&out, 1, 3);
  u2ps_chipidea_out_retire(&out, 1u << 15 | U2PS_CHIPIDEA_DTD_XACT_ERROR, 3, &r);
  CHECK_EQ(r.buf, NO);
  CHECK_EQ(r.append.n, 1);
  CHECK_EQ(r.append.tail, 1);
  /* 4: a reserve packet again */
  u2ps_chipidea_out_pass(&out, 1, 4);
  u2ps_chipidea_out_retire(&out, 1u << 15, 4, &r);
  CHECK_EQ(r.buf, NO);
  CHECK_EQ(r.append.tail, 2);
  CHECK_EQ(out.q_len, 2);
  u2ps_chipidea_out_release(&out, 1);
  CHECK_EQ(out.h_free, 1);
  /* 5: delivered, with the holes since 1; the released slot 0 queued
   * after slot 1 */
  u2ps_chipidea_out_pass(&out, 1, 5);
  u2ps_chipidea_out_retire(&out, 1u << 15, 5, &r);
  CHECK_EQ(r.buf, 2);
  CHECK_EQ(r.append.n, 1);
  CHECK_EQ(r.append.tail, 1);
  CHECK_EQ(u2ps_chipidea_out_at(&out, 1), 0);
  CHECK_EQ(r.uframe, 5);
  CHECK_EQ(r.holes.no_buffer, 2);
  CHECK_EQ(r.holes.corrupted, 1);
  CHECK_EQ(r.holes.late, 0);
  CHECK_EQ(r.holes.no_packet, 0);
  CHECK_EQ(out.no_buffer_total, 2);
  CHECK_EQ(out.corrupted_total, 1);
  CHECK_EQ(out.errors, 0);
}

/* The stream's end: the holes after the last delivered packet, up to the
 * last packet received, taken once */
static void test_tail(void)
{
  u2ps_chipidea_out_packet_t r;
  u2ps_chipidea_holes_t h;

  memset(&out, 0, sizeof out);
  CHECK(u2ps_chipidea_out_open(&out, 3, MPS, UFR));
  /* Nothing before the first packet, not even a CRC error */
  u2ps_chipidea_out_tail(&out, &h);
  CHECK_EQ(h.no_packet + h.late + h.no_buffer + h.corrupted, 0);
  u2ps_chipidea_out_pass(&out, 1, 99);
  u2ps_chipidea_out_retire(&out, 1u << 15 | U2PS_CHIPIDEA_DTD_XACT_ERROR, 99, &r);
  CHECK_EQ(r.buf, NO);
  u2ps_chipidea_out_tail(&out, &h);
  CHECK_EQ(h.no_packet + h.late + h.no_buffer + h.corrupted, 0);
  u2ps_chipidea_out_pass(&out, 1, 100);
  u2ps_chipidea_out_retire(&out, 1u << 15, 100, &r);
  CHECK_EQ(r.buf, 1); /* slot 0 was queued again after the CRC error */
  u2ps_chipidea_out_tail(&out, &h);
  CHECK_EQ(h.no_packet + h.late + h.no_buffer + h.corrupted, 0);
  /* Reserve packets in 101, 104 and 105, a CRC error in 106 */
  static const uint32_t at[4] = {101, 104, 105, 106};
  for (unsigned i = 0; i < 4u; i++) {
    u2ps_chipidea_out_pass(&out, 1, at[i]);
    u2ps_chipidea_out_retire(&out, 1u << 15 | (i == 3u ? U2PS_CHIPIDEA_DTD_XACT_ERROR : 0u), at[i], &r);
    CHECK_EQ(r.buf, NO);
  }
  /* A close begins: the tail before on_closed */
  u2ps_chipidea_out_closing(&out);
  u2ps_chipidea_out_tail(&out, &h);
  CHECK_EQ(h.no_buffer, 3);
  CHECK_EQ(h.no_packet, 2);
  CHECK_EQ(h.corrupted, 1);
  CHECK_EQ(h.late, 0);
  u2ps_chipidea_out_tail(&out, &h);
  CHECK_EQ(h.no_packet + h.late + h.no_buffer + h.corrupted, 0);
  u2ps_chipidea_out_close(&out);
  CHECK_EQ(out.errors, 0);

  /* 70,000 reserve packets: counted in full, not cut at 16 bits */
  CHECK(u2ps_chipidea_out_open(&out, 3, MPS, UFR));
  u2ps_chipidea_out_pass(&out, 1, 0);
  u2ps_chipidea_out_retire(&out, 1u << 15, 0, &r);
  CHECK_EQ(r.buf, 0);
  unsigned bad = 0;
  for (uint32_t t = 1; t <= 70000u; t++) {
    u2ps_chipidea_out_pass(&out, 1, t);
    u2ps_chipidea_out_retire(&out, 1u << 15, t & 0x7FFu, &r);
    bad += r.buf != NO || r.append.n != 1u;
  }
  CHECK_EQ(bad, 0);
  u2ps_chipidea_out_tail(&out, &h);
  CHECK_EQ(h.no_buffer, 70000);
  CHECK_EQ(h.no_packet + h.late + h.corrupted, 0);
  CHECK_EQ(out.no_buffer_total, 70000u + 3u);
  CHECK_EQ(out.errors, 0);
}

/* The pending holes taken in the middle of a stream (u2ps_dport_report_holes
 * at a long gap): with them, every packet's holes, its microframe and the
 * totals are those of the same run without the takes, in every Frame
 * Number unit; nothing is reported twice, and the takes find what a
 * stall left (the reserve packets before a host pause) */
static pkt_t pk0[RUN];

static void run_twice(int u, uint32_t seed, uint32_t until, bool stall)
{
  uint32_t rep0[4] = {0}, n0 = 0, errors0 = 0;
  for (unsigned pass = 0; pass < 2u; pass++) {
    rng = seed;
    sim_open(u, u, stall ? 3 : 16, stall ? 0u : 8u, stall ? 5000u : rnd(0x4000));
    S.lag = stall ? 0u : 1u;
    for (uint32_t t = 0; t < until; t++) {
      if (stall) {
        keep[t] = 1;
        host[t] = t >= 100u && t < 600u ? H_SKIP : H_SEND;
        continue;
      }
      if (rnd(50) == 0u)
        host[t] = H_SKIP;
      if (rnd(2000) == 0u)
        host[t] = H_CRC;
      if (rnd(10) == 0u)
        late[t] = 1;
      if (rnd(3) == 0u)
        hazard[t] = 1;
      if (t % 1000u == 500u) {
        for (uint32_t j = t; j < t + 30u; j++)
          keep[j] = 1;
        trel[t + 40u] = ALL;
      }
      bool take = rnd(97) == 0u; /* drawn in both passes: the same run */
      if (pass == 1u && take)
        take_at[t] = 1;
    }
    if (stall) {
      trel[650] = ALL;
      if (pass == 1u)
        take_at[300] = 1;
    }
    sim_run(until);
    if (pass == 0u) {
      memcpy(pk0, pk, sizeof pk);
      memcpy(rep0, S.rep, sizeof rep0);
      n0 = S.n;
      errors0 = out.errors;
    }
  }
  unsigned bad = 0;
  for (uint32_t i = 0; i < S.n && i < n0; i++)
    bad += pk[i].when != pk0[i].when || pk[i].rep != pk0[i].rep || pk[i].uframe != pk0[i].uframe;
  CHECK_EQ(bad, 0);
  CHECK_EQ(S.n, n0);
  CHECK_EQ(S.rep[0], rep0[0]);
  CHECK_EQ(S.rep[1], rep0[1]);
  CHECK_EQ(S.rep[2], rep0[2]);
  CHECK_EQ(S.rep[3], rep0[3]);
  CHECK_EQ(out.errors, errors0);
}

static void test_pending_taken(void)
{
  u2ps_chipidea_holes_t h;

  for (int u = NONE; u <= FRM; u++) {
    /* A stall with a host pause inside it: the take in the pause finds
     * the 98 reserve packets of 2..99 */
    run_twice(u, 7, 700, true);
    CHECK_EQ(S.n, 2);
    CHECK_EQ(pk[1].rep, 648);
    CHECK_EQ(S.rep[2], 148);
    CHECK_EQ(S.rep[0], 500);
    /* The tail after it: only the reserve packets of 651..699 */
    u2ps_chipidea_out_tail(&out, &h);
    CHECK_EQ(h.no_buffer, 49);
    CHECK_EQ(h.no_packet + h.late + h.corrupted, 0);
    for (unsigned k = 0; k < 3u; k++)
      run_twice(u, 901u + 7u * k, 20100, false);
  }
  /* The take itself, inside the host's pause */
  sim_open(UFR, UFR, 3, 0, 5000);
  for (uint32_t t = 0; t < 700; t++)
    keep[t] = 1;
  for (uint32_t t = 100; t < 600; t++)
    host[t] = H_SKIP;
  sim_run(300);
  u2ps_chipidea_out_tail(&out, &h);
  CHECK_EQ(h.no_buffer, 98);
  CHECK_EQ(h.no_packet + h.late + h.corrupted, 0);
  u2ps_chipidea_out_tail(&out, &h);
  CHECK_EQ(h.no_packet + h.late + h.no_buffer + h.corrupted, 0);
}

/* Halted alone is an overflow halt too, taken even with Active set */
static void test_halted(void)
{
  u2ps_chipidea_out_packet_t r;

  memset(&out, 0, sizeof out);
  CHECK(u2ps_chipidea_out_open(&out, 4, MPS, UFR));
  u2ps_chipidea_out_pass(&out, 2, 101);
  u2ps_chipidea_out_retire(&out, 1u << 15, 100, &r);
  CHECK_EQ(r.buf, 0);
  u2ps_chipidea_out_retire(&out, 1u << 15 | U2PS_CHIPIDEA_DTD_HALTED, 101, &r);
  CHECK_EQ(r.buf, NO);
  CHECK_EQ(r.append.n, 1);
  CHECK_EQ(out.errors, 1);
  CHECK_EQ(u2ps_chipidea_out_primed(&out, 0, 102), 2);
  u2ps_chipidea_out_pass(&out, 1, 104);
  u2ps_chipidea_out_retire(&out, u2ps_chipidea_dtd_token(MPS, 0) | U2PS_CHIPIDEA_DTD_HALTED |
                                       U2PS_CHIPIDEA_DTD_BUF_ERROR, 103, &r);
  CHECK_EQ(r.buf, NO);
  CHECK_EQ(r.append.n, 1);
  CHECK_EQ(out.errors, 2);
  CHECK_EQ(u2ps_chipidea_out_primed(&out, 0, 104), 3);
  u2ps_chipidea_out_pass(&out, 1, 105);
  u2ps_chipidea_out_retire(&out, 1u << 15, 105, &r);
  CHECK_EQ(r.buf, 3);
  CHECK_EQ(r.holes.late, 4); /* 101..104 */
  CHECK_EQ(r.holes.no_packet, 0);
  CHECK_EQ(r.uframe, 105);

  /* In the simulation */
  sim_open(NONE, NONE, 4, 0, 0);
  host[30] = H_SHORT;
  sim_run(40);
  CHECK_EQ(out.errors, 0);
}

/* An over-report after an earlier one that later holes absorbed is found
 * and counted again */
static void test_fallback_over_again(void)
{
  sim_open(NONE, NONE, 8, 0, 0);
  late[100] = late[101] = late[102] = 1;
  host[300] = H_SKIP;
  host[500] = H_SKIP;
  late[700] = late[701] = late[702] = 1;
  sim_run(900);
  check_fallback();
  CHECK_EQ(S.rep[0], 3);
  CHECK_EQ(out.fallback_over, 2);
  CHECK_EQ(out.errors, 0);
}

/* The dry read under the tripwire: the re-prime window after a retirement
 * makes both bits read 0; the tripwire repeats the read, and no prime is
 * written to an endpoint that is still primed. Read without it, the same
 * stream rewrites a primed endpoint (the model's negative control). */
static void test_dry_read(void)
{
  for (int plain = 0; plain <= 1; plain++) {
    rng = 31;
    sim_open(UFR, UFR, 6, 0, 0x2000);
    S.plain_read = plain != 0;
    for (uint32_t t = 0; t < 5000u; t++) {
      if (rnd(4) == 0u)
        hazard[t] = 1;
      if (rnd(40) == 0u)
        host[t] = H_SKIP;
      if (rnd(100) == 0u)
        kick[t] = 1;
    }
    for (uint32_t t = 1000; t < 1040u; t++)
      keep[t] = 1;
    trel[1050] = ALL;
    sim_run(5100);
    if (plain) {
      CHECK(S.reprimed > 0u);
    } else {
      CHECK(S.retries > 100u);
      CHECK_EQ(S.reprimed, 0);
      CHECK_EQ(out.errors, 0);
      check_exact();
    }
  }
}

/* Random streams with the hardware effects: retirement that shows a
 * microframe late, ITC interrupts at a microframe's start, primes that take
 * effect at t + 2, the re-prime window, holds and releases in thread */
static void test_effects_random(void)
{
  for (int u = NONE; u <= FRM; u++) {
    for (unsigned k = 0; k < 4u; k++) {
      rng = 501u + (uint32_t)u * 10u + k;
      unsigned itc = k < 2u ? 0u : 8u;
      sim_open(u, u, 16, itc, rnd(0x4000));
      S.itc_start = (k & 1u) != 0u;
      S.lag = (uint16_t)(k == 3u ? 4u : 1u);
      for (uint32_t t = 0; t < 20000u; t++) {
        if (rnd(50) == 0u)
          host[t] = H_SKIP;
        if (rnd(2000) == 0u)
          host[t] = H_CRC;
        if (rnd(6) == 0u)
          rlag[t] = 1;
        if (rnd(3) == 0u)
          hazard[t] = 1;
        if (rnd(2) == 0u)
          plate[t] = 1;
        if (rnd(10) == 0u)
          late[t] = 1;
        if (t % 1000u == 500u) {
          for (uint32_t j = t; j < t + 30u; j++)
            keep[j] = 1;
          trel[t + 40u] = ALL;
        }
      }
      sim_run(20100);
      CHECK_EQ(S.reprimed, 0);
      CHECK_EQ(S.prime_bad, 0);
      CHECK(S.rep[2] > 100u);
      if (u == UFR) {
        CHECK_EQ(out.errors, 0);
        check_exact();
      } else if (u == FRM) {
        CHECK_EQ(out.errors, 0);
        check_frames();
        CHECK(pk[S.n - 1u].cum + first_frame_holes() >= true_between(0, S.n - 1u));
      } else {
        check_fallback();
      }
    }
  }
}

/* ---- Release and close ---------------------------------------------------------- */

static void test_release_calls(void)
{
  memset(&out, 0, sizeof out);
  u2ps_chipidea_out_release(&out, 1); /* no stream */
  CHECK_EQ(out.errors, 1);
  CHECK(u2ps_chipidea_out_open(&out, 4, MPS, NONE));
  u2ps_chipidea_out_release(&out, 0);
  CHECK_EQ(out.errors, 1);
  u2ps_chipidea_out_release(&out, 1); /* none held */
  CHECK_EQ(out.errors, 2);
  CHECK_EQ(out.h_free, 0);
  /* A primed() outside OPEN, an active dTD, an empty queue */
  u2ps_chipidea_out_packet_t r;
  u2ps_chipidea_out_retire(&out, u2ps_chipidea_dtd_token(MPS, 0), 0, &r);
  CHECK_EQ(out.errors, 3);
  CHECK_EQ(out.q_len, 4); /* not taken */
  CHECK_EQ(u2ps_chipidea_out_primed(&out, 5, 0), NO);
  CHECK_EQ(out.errors, 4);
  /* Three packets in one pass: two delivered, and the third a reserve
   * packet, queued again after slot 3 (four slots, the stack holds at
   * most two); released in arrival order */
  u2ps_chipidea_out_pass(&out, 3, 100);
  for (int i = 0; i < 3; i++) {
    u2ps_chipidea_out_retire(&out, 1u << 15, 0, &r);
    CHECK_EQ(r.buf, i < 2 ? (uint16_t)i : NO);
    CHECK_EQ(r.len, i < 2 ? MPS : 0u);
  }
  CHECK_EQ(u2ps_chipidea_out_at(&out, 1), 2);
  u2ps_chipidea_out_release(&out, 3); /* two held */
  CHECK_EQ(out.errors, 5);
  /* Releases only free; the next retirement queues the slots */
  u2ps_chipidea_out_release(&out, 1);
  CHECK_EQ(out.h_free, 1);
  CHECK_EQ(out.q_len, 2);
  u2ps_chipidea_out_release(&out, 1);
  CHECK_EQ(out.h_free, 2);
  u2ps_chipidea_out_release(&out, 1); /* none held */
  CHECK_EQ(out.errors, 6);
  /* A remainder above mps cannot come from the controller; the retirement
   * appends the released slots in arrival order */
  u2ps_chipidea_out_pass(&out, 1, 101);
  u2ps_chipidea_out_retire(&out, (uint32_t)(MPS + 1u) << 16, 0, &r);
  CHECK_EQ(r.buf, 3);
  CHECK_EQ(r.len, 0);
  CHECK_EQ(out.errors, 7);
  CHECK_EQ(r.append.n, 2);
  CHECK_EQ(r.append.first, 1);
  CHECK_EQ(r.append.tail, 2);
  CHECK_EQ(u2ps_chipidea_out_at(&out, 1), 0);
  CHECK_EQ(u2ps_chipidea_out_at(&out, 2), 1);
  CHECK_EQ(out.h_free, 0);
  /* A pass of more dTDs than queued */
  u2ps_chipidea_out_pass(&out, 9, 102);
  CHECK_EQ(out.errors, 8);
}

static void test_close(void)
{
  u2ps_chipidea_close_t c;
  u2ps_chipidea_fb_t f;
  uint32_t flush;

  memset(&c, 0, sizeof c);
  memset(&f, 0, sizeof f);
  CHECK_EQ(u2ps_chipidea_close_sof(&c, 0, 0, 0, &flush), U2PS_CHIPIDEA_CLOSE_DONE);

  /* ENDPTCTRLn: the stream's directions isochronous, the unused one bulk */
  CHECK_EQ(u2ps_chipidea_ep_ctrl(1, 1, 1, true), 0x00C400C4u);
  CHECK_EQ(u2ps_chipidea_ep_ctrl(1, 1, 1, false), 0x00040004u);
  CHECK_EQ(u2ps_chipidea_ep_ctrl(1, 1, 2, true), 0x000800C4u);
  CHECK_EQ(u2ps_chipidea_ep_ctrl(2, 1, 2, true), 0x00C40008u);
  CHECK_EQ(u2ps_chipidea_ep_ctrl(1, 1, 0, true), 0x000800C4u);
  CHECK_EQ(u2ps_chipidea_ep_ctrl(0, 0, 0, true), 0);
  CHECK_EQ(u2ps_chipidea_ep_ctrl(8, 8, 8, true), 0);

  /* A stream with packets held, one released and not yet queued, and dTDs
   * queued */
  sim_open(NONE, NONE, 6, 0, 0);
  CHECK(u2ps_chipidea_fb_open(&f, 4));
  S.lag = 3;
  sim_run(50);
  CHECK_EQ(S.held, 3);
  CHECK_EQ(out.h_free, 1);
  CHECK_EQ(out.q_len, 2);
  uint32_t m = u2ps_chipidea_close_begin(&c, 1, 2);
  CHECK_EQ(m, 0x00040002u);
  u2ps_chipidea_out_closing(&out);
  u2ps_chipidea_fb_closing(&f);
  /* A packet in progress completes during the flush: dropped */
  S.done = 1;
  S.token[u2ps_chipidea_out_at(&out, 0)] = 1u << 15;
  uint32_t errors = out.errors;
  handler();
  CHECK_EQ(out.close_drops, 1);
  CHECK_EQ(S.n, 49);
  /* The stack releases what it holds: freed, nothing appended */
  uint16_t q = out.q_len;
  release(2);
  CHECK_EQ(out.q_len, q);
  CHECK_EQ(out.h_len, 1);
  CHECK_EQ(out.h_free, 0);
  CHECK_EQ(out.errors, errors);
  /* SOFs: the flush runs; it fails for the data endpoint and runs again;
   * then the endpoints must show stopped at two SOFs in a row, and a prime
   * that shows in between starts the count again */
  CHECK_EQ(u2ps_chipidea_close_sof(&c, 0x00040002u, 0, 0x00040002u, &flush), U2PS_CHIPIDEA_CLOSE_WAIT);
  CHECK_EQ(u2ps_chipidea_close_sof(&c, 0x00000080u, 0, 0x00000082u, &flush), U2PS_CHIPIDEA_CLOSE_FLUSH);
  CHECK_EQ(flush, 0x00000002u);
  CHECK_EQ(u2ps_chipidea_close_sof(&c, 0x00000002u, 0, 0, &flush), U2PS_CHIPIDEA_CLOSE_WAIT);
  CHECK_EQ(u2ps_chipidea_close_sof(&c, 0, 0, 0, &flush), U2PS_CHIPIDEA_CLOSE_WAIT);
  CHECK_EQ(u2ps_chipidea_close_sof(&c, 0, 0x00000002u, 0, &flush), U2PS_CHIPIDEA_CLOSE_WAIT);
  CHECK_EQ(u2ps_chipidea_close_sof(&c, 0x00800000u, 0x00000001u, 0x00000001u, &flush), U2PS_CHIPIDEA_CLOSE_WAIT);
  CHECK(c.active);
  CHECK_EQ(u2ps_chipidea_close_sof(&c, 0, 0, 0, &flush), U2PS_CHIPIDEA_CLOSE_DONE);
  CHECK_EQ(c.sofs, 7);
  CHECK_EQ(c.flushes, 2);
  CHECK(!c.active);
  /* The next open checks that neither endpoint is still primed */
  CHECK(!u2ps_chipidea_close_idle(&c, 0, 0x00000002u, 1, 2));
  CHECK(!u2ps_chipidea_close_idle(&c, 0x00040000u, 0, 1, 2));
  CHECK_EQ(c.open_busy, 2);
  CHECK(u2ps_chipidea_close_idle(&c, 0x00000080u, 0x00000001u, 1, 2));
  CHECK(u2ps_chipidea_close_idle(&c, 0x00040000u, 0, 1, 0));
  u2ps_chipidea_out_close(&out);
  u2ps_chipidea_fb_close(&f);
  /* After the close a release counts in errors */
  release(1);
  CHECK_EQ(out.errors, errors + 1u);
  /* Without a feedback endpoint only the data endpoint is flushed */
  CHECK_EQ(u2ps_chipidea_close_begin(&c, 3, 0), 0x00000008u);
  /* The next open starts clean */
  CHECK(u2ps_chipidea_out_open(&out, 6, MPS, NONE));
  CHECK_EQ(out.q_len, 6);
  CHECK_EQ(out.h_len, 0);
}

/* ---- Feedback ------------------------------------------------------------------ */

static void test_feedback(void)
{
  u2ps_chipidea_fb_t f;
  uint16_t tail;
  const uint32_t done = 1u << 15; /* retired, IOC */

  memset(&f, 0, sizeof f);
  CHECK(!u2ps_chipidea_fb_open(&f, 1));
  CHECK(!u2ps_chipidea_fb_open(&f, 17));
  CHECK(u2ps_chipidea_fb_open(&f, 16));
  CHECK(u2ps_chipidea_fb_open(&f, 2));
  /* Rotation: each retired dTD goes behind the other */
  CHECK_EQ(u2ps_chipidea_fb_retire(&f, done, &tail), 0);
  CHECK_EQ(tail, 1);
  CHECK_EQ(u2ps_chipidea_fb_retire(&f, done, &tail), 1);
  CHECK_EQ(tail, 0);
  CHECK_EQ(u2ps_chipidea_fb_retire(&f, done, &tail), 0);
  CHECK_EQ(tail, 1);
  CHECK_EQ(f.polls, 3);
  CHECK_EQ(f.recoveries, 0);
  /* An active token is not taken */
  CHECK_EQ(u2ps_chipidea_fb_retire(&f, u2ps_chipidea_dtd_token(4, 1), &tail), NO);
  CHECK_EQ(f.errors, 1);
  /* Both retired before the pass: the first one's append finds the
   * endpoint dry with the second retired at the head; the dQH gets the
   * first */
  CHECK_EQ(u2ps_chipidea_fb_retire(&f, done, &tail), 1);
  CHECK_EQ(tail, 0);
  CHECK_EQ(u2ps_chipidea_fb_primed(&f, 1), 1);
  CHECK_EQ(f.recoveries, 1);
  CHECK_EQ(u2ps_chipidea_fb_retire(&f, done, &tail), 0);
  CHECK_EQ(tail, 1);
  /* An append that finds the endpoint dry with nothing retired before it
   * primes the queue's head */
  CHECK_EQ(u2ps_chipidea_fb_primed(&f, 0), 1);
  CHECK_EQ(f.recoveries, 2);
  /* Nothing to prime when both are retired */
  CHECK_EQ(u2ps_chipidea_fb_primed(&f, 2), NO);
  CHECK_EQ(f.errors, 1);
  CHECK_EQ(u2ps_chipidea_fb_primed(&f, 3), NO);
  CHECK_EQ(f.errors, 2);
  /* In CLOSING, retired dTDs are taken and not appended */
  u2ps_chipidea_fb_closing(&f);
  CHECK_EQ(u2ps_chipidea_fb_retire(&f, done, &tail), NO);
  CHECK_EQ(f.len, 1);
  u2ps_chipidea_fb_close(&f);
  CHECK_EQ(u2ps_chipidea_fb_retire(&f, done, &tail), NO);

  /* A simulated poll every 8 microframes, the value fetched when a dTD is
   * primed: a new value goes out within two polls */
  CHECK(u2ps_chipidea_fb_open(&f, 4));
  uint32_t polls = f.polls;
  uint32_t word = 100, latched[2] = {100, 100};
  uint16_t head = 0; /* the primed dTD */
  unsigned within_bad = 0, sent = 0;
  for (uint32_t t = 0; t < 2000u; t++) {
    if (t % 100u == 50u)
      word++; /* fb_set */
    if (t % 8u == 3u) {
      uint32_t v = latched[head];
      if (t % 100u >= 50u + 16u && t % 100u < 99u && v != word)
        within_bad++;
      sent++;
      uint16_t d = u2ps_chipidea_fb_retire(&f, done, &tail);
      CHECK_EQ(d, head);
      head ^= 1u;
      latched[head] = word; /* the other dTD is primed now */
    }
  }
  CHECK_EQ(within_bad, 0);
  CHECK_EQ(f.polls - polls, sent);
}

int main(void)
{
  test_tokens();
  test_open();
  test_reserve_held();
  test_dry_unwalked();
  test_lost_prime();
  test_overflow();
  test_corrupted();
  test_uframes();
  test_frames();
  test_fallback_single_late();
  test_fallback_runs();
  test_fallback_hole_late();
  test_fallback_random();
  test_unit_mismatch();
  test_dry_unwalked_skip();
  test_dry_primes_pending();
  test_prime_late();
  test_corrupted_reserve();
  test_tail();
  test_pending_taken();
  test_halted();
  test_fallback_over_again();
  test_dry_read();
  test_effects_random();
  test_release_calls();
  test_close();
  test_feedback();
  return check_done();
}
