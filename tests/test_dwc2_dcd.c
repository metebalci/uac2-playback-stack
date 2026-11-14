/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dwc2_dcd.c: the DWC2 device port's interrupt handler
 * (port/dwc2/dcd/u2ps_dwc2_dcd.c, built for the STM32U5A5) against a
 * register model, for the order in which it serves the data endpoint's
 * XFRC, INCOMPISOOUT and EOPF:
 * - a stream with a packet in every microframe, also with packets that
 *   complete after the EOPF point (INCOMPISOOUT raised for them while
 *   EPENA is still set), served in their own pass or in the pass of their
 *   XFRC, and with INCOMPISOOUT raised at every EOPF while the endpoint is
 *   enabled: every packet delivered, in its own microframe, no hole;
 * - gaps of the host of 1 to 31 microframes: the packet after a gap of 2
 *   or more is delivered in either parity, the one after a single skip is
 *   lost (as in 0.1.0); EOPF is unmasked for the gap's follow and masked
 *   again after it, INCOMPISOOUT stays unmasked;
 * - a packet that completes inside the pass of its INCOMPISOOUT, after the
 *   handler read OTG_DAINT, at the second evidence of a gap and while a
 *   follow runs: its parity is not changed under it;
 * - one miss (a gap of 2, a skip, a corrupted packet) or a gap of 30, then a
 *   packet that completes past its EOPF: NAKSTS (set as the packet enters
 *   the Rx FIFO) keeps the port from taking it for a gap or writing a
 *   parity under it.
 * The model (fake_regs.h): each register access gets a fresh slot holding
 * the value a read returns; the next access, or the end of a call into the
 * port, compares the slot with it, and a difference is the port's write.
 * So that a write of the value just read is seen too, reads of the
 * registers with write-1-to-clear bits and of OTG_DOEPCTLx carry a counter
 * in bits the port never writes (reserved or host-mode bits). Each case
 * runs in a child process, so that the port's state starts at zero.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"
#include "fake_regs.h"
#include "u2ps_dport.h"
#include "u2ps_dwc2.h"
#include "u2ps_dwc2_dcd.h"
#include "u2ps_dwc2_regs.h"

#define EP 1u
#define A(off) ((uintptr_t)U2PS_DWC2_OTG_BASE + (uintptr_t)(off))
#define A_GINTSTS  A(0x014u)
#define A_GINTMSK  A(0x018u)
#define A_DSTS     A(0x808u)
#define A_DAINT    A(0x818u)
#define A_DAINTMSK A(0x81Cu)
#define A_DOEPCTL  A(0xB00u + 0x20u * EP)
#define A_DOEPINT  A(0xB08u + 0x20u * EP)
#define A_DOEPTSIZ A(0xB10u + 0x20u * EP)

/* ---- The core ----------------------------------------------------------------- */

static struct {
  long fn;                     /* the current microframe */
  uint32_t gintsts, gintmsk, daintmsk;
  bool epena, eonum;           /* OTG_DOEPCTLx.EPENA and EONUM */
  bool nak;                    /* OTG_DOEPCTLx.NAKSTS, cleared by CNAK; set as nak_mode says */
  int nak_mode;                /* 0: NAK set as a packet enters the Rx FIFO (PKTCNT 0,
                                  RM0456 p. 3444, flow 3); 1: only at its completion
                                  (the SNAK text, p. 3390); 2: never */
  bool busy;                   /* a packet received, the endpoint not yet enabled again */
  uint32_t doepctl;            /* its other bits, as written */
  uint32_t doepint, doeptsiz;
  bool draining;               /* a received packet completes after the EOPF */
  bool every_eopf;             /* INCOMPISOOUT at every EOPF while enabled */
  bool in_pass;                /* a draining packet completes inside the next pass,
                                  right after the handler read in_pass_addr */
  uintptr_t in_pass_addr;
  uintptr_t sof_addr;          /* the next SOF comes right after the handler reads this
                                  register sof_count more times */
  unsigned sof_count;
  bool sof_early;              /* that SOF has come: the step does not count it again */
  bool closed;                 /* on_closed came */
  uintptr_t rx_addr;           /* the host's packet of this microframe comes right after the
                                  handler reads this register rx_count more times */
  unsigned rx_count;
  long hold_until;             /* the handler cannot run before microframe abs hold_until
                                  starts */
  long hold_after_eopf;        /* from this step's EOPF, hold the handler for so many
                                  microframes */
  long abs;                    /* microframes since the open, without the 14-bit wrap */
  long sof_n;                  /* microframes the SOF hook advances */
  unsigned canary;
  /* Other registers: plain storage */
  struct {
    uintptr_t addr;
    uint32_t v;
  } plain[64];
  unsigned nplain;
  /* The access in progress */
  bool pend;
  uintptr_t addr;
  uint32_t slot, shown;
} core;

static uint32_t *plain_reg(uintptr_t addr)
{
  for (unsigned i = 0; i < core.nplain; i++)
    if (core.plain[i].addr == addr)
      return &core.plain[i].v;
  CHECK(core.nplain < 64u);
  core.plain[core.nplain].addr = addr;
  core.plain[core.nplain].v = 0;
  return &core.plain[core.nplain++].v;
}

static uint32_t next_canary(unsigned bits)
{
  core.canary = core.canary % ((1u << bits) - 1u) + 1u; /* 1 .. 2^bits - 1 */
  return core.canary;
}

static uint32_t reg_read(uintptr_t addr)
{
  if (addr == A_GINTSTS)
    return core.gintsts | next_canary(3) << 24; /* HPRTINT, HCINT, PTXFE: host mode */
  if (addr == A_GINTMSK)
    return core.gintmsk;
  if (addr == A_DAINTMSK)
    return core.daintmsk;
  if (addr == A_DAINT)
    return (core.doepint & 0xFFFFu) != 0u ? DAINT_OEP(EP) : 0u;
  if (addr == A_DSTS)
    return (uint32_t)(core.fn & 0x3FFF) << DSTS_FNSOF_POS;
  if (addr == A_DOEPCTL)
    return (core.epena ? EPCTL_EPENA : 0u) | (core.eonum ? (1u << 16) : 0u) |
           (core.nak ? (1u << 17) : 0u) | core.doepctl |
           next_canary(4) << 22; /* bits 25:22 reserved */
  if (addr == A_DOEPINT)
    return core.doepint | next_canary(7) << 24; /* bits 30:24 reserved */
  if (addr == A_DOEPTSIZ)
    return core.doeptsiz;
  return *plain_reg(addr);
}

static void reg_write(uintptr_t addr, uint32_t v)
{
  if (addr == A_GINTSTS) {
    core.gintsts &= ~(v & ~(7u << 24));
  } else if (addr == A_GINTMSK) {
    core.gintmsk = v;
  } else if (addr == A_DAINTMSK) {
    core.daintmsk = v;
  } else if (addr == A_DOEPCTL) {
    if ((v & EPCTL_SODDFRM) != 0u)
      core.eonum = true;
    if ((v & EPCTL_SEVNFRM) != 0u)
      core.eonum = false;
    if ((v & EPCTL_EPENA) != 0u) {
      core.epena = true;
      core.busy = false;
    }
    if ((v & EPCTL_EPDIS) != 0u && core.epena) {
      core.epena = false; /* disabled at once: the model has no packet in flight then */
      core.doepint |= DOEPINT_EPDISD;
    }
    if ((v & EPCTL_CNAK) != 0u)
      core.nak = false;
    if ((v & EPCTL_SNAK) != 0u)
      core.nak = true;
    core.doepctl = v & ~(EPCTL_EPENA | EPCTL_EPDIS | EPCTL_SODDFRM | EPCTL_SEVNFRM |
                         EPCTL_SNAK | EPCTL_CNAK | (3u << 16) | (15u << 22));
  } else if (addr == A_DOEPINT) {
    core.doepint &= ~(v & 0xFFFFu);
  } else if (addr == A_DOEPTSIZ) {
    core.doeptsiz = v;
  } else if (addr == A(0x804u)) { /* OTG_DCTL: the global OUT NAK takes effect at once */
    if ((v & DCTL_SGONAK) != 0u)
      core.gintsts |= GINT_GONAKEFF;
    if ((v & DCTL_CGONAK) != 0u)
      core.gintsts &= ~GINT_GONAKEFF;
    *plain_reg(addr) = v & ~(DCTL_SGONAK | DCTL_CGONAK);
  } else if (addr == A_DSTS || addr == A_DAINT) {
    CHECK(false); /* read-only */
  } else {
    *plain_reg(addr) = v;
  }
}

static void commit(void)
{
  if (core.pend && core.slot != core.shown)
    reg_write(core.addr, core.slot);
  core.pend = false;
}

static void complete(void);
static void host_token(void);

uint32_t *fake_reg32(uintptr_t addr)
{
  commit();
  core.pend = true;
  core.addr = addr;
  core.shown = core.slot = reg_read(addr);
  if (addr == core.in_pass_addr && core.in_pass && core.draining) {
    core.in_pass = false;
    core.draining = false;
    complete();
  }
  if (addr == core.rx_addr && core.rx_count > 0u && --core.rx_count == 0u)
    host_token();
  if (addr == core.sof_addr && core.sof_count > 0u && --core.sof_count == 0u) {
    long n = core.sof_n > 0 ? core.sof_n : 1;
    core.fn = (core.fn + n) & 0x3FFF;
    core.abs += n;
    core.sof_early = true;
  }
  return &core.slot;
}

uint16_t *fake_reg16(uintptr_t addr)
{
  static uint16_t dummy;
  (void)addr;
  CHECK(false);
  return &dummy;
}

/* ---- What the port calls ----------------------------------------------------- */

#define MAXPK 4096
static struct {
  unsigned packets;
  long uframe[MAXPK];
  unsigned long holes;
} app;

void u2ps_dev_on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  (void)data;
  (void)len;
  if (app.packets < MAXPK)
    app.uframe[app.packets] = uframe;
  app.packets++;
  u2ps_dport_release(1);
}

void u2ps_dev_on_missed(uint16_t uframes, u2ps_miss_t why)
{
  (void)why;
  app.holes += uframes;
}

void u2ps_dev_on_reset(void) {}
void u2ps_dev_on_speed(u2ps_speed_t speed) { (void)speed; }
void u2ps_dev_on_suspend(void) {}
void u2ps_dev_on_resume(void) {}
void u2ps_dev_on_setup(const uint8_t setup[8]) { (void)setup; }
void u2ps_dev_on_ep0_data(uint16_t len) { (void)len; }
void u2ps_dev_on_ep0_status(void) {}
void u2ps_dev_on_closed(void) { core.closed = true; }

/* The parts of u2ps_dwc2_common the stream does not reach, or that need
 * the chip (u2ps_dwc2.c is built for ARM only) */
bool u2ps_dwc2_dma_ok(const void *addr, uint32_t len)
{
  (void)addr;
  (void)len;
  return true;
}
bool u2ps_dwc2_wait_us(volatile uint32_t *reg, uint32_t mask, uint32_t value, uint32_t us)
{
  (void)reg;
  (void)mask;
  (void)value;
  (void)us;
  return true;
}
bool u2ps_dwc2_flush_tx(uint32_t fifo) { (void)fifo; return true; }
bool u2ps_dwc2_flush_rx(void) { return true; }
bool u2ps_dwc2_fifo_setup(uint16_t out_mps, uint16_t fb_mps) { (void)out_mps; (void)fb_mps; return true; }
void u2ps_dwc2_irq_mask(bool masked) { (void)masked; }
void u2ps_dwc2_delay_us(uint32_t us) { (void)us; }
bool u2ps_dwc2_core_reset(void) { return true; }
void u2ps_dwc2_pwr_clock(void) {}
u2ps_dwc2_err_t u2ps_dwc2_phy_start(void) { return U2PS_DWC2_OK; }
bool u2ps_dwc2_cycles_start(void) { return true; }
u2ps_dev_err_t u2ps_dwc2_dev_err(u2ps_dwc2_err_t e) { (void)e; return U2PS_DEV_OK; }
/* The DWT, which the port reads only around a close here: the model's
 * microframe in cycles at 160 MHz */
uint32_t u2ps_dwc2_cycles(void)
{
  return (uint32_t)((unsigned long)core.abs * 20000ul);
}

/* ---- The bus ------------------------------------------------------------------ */

static _Alignas(32) uint8_t mem[8 * 64];

/* The handler, as the NVIC calls it while an unmasked interrupt is
 * pending */
static void irq(void)
{
  if (core.abs < core.hold_until)
    return;
  for (int i = 0; i < 4; i++) {
    bool pending = (core.gintsts & core.gintmsk) != 0u ||
                   ((core.doepint & 0xFFFFu) != 0u && (core.daintmsk & DAINT_OEP(EP)) != 0u);
    if (!pending)
      return;
    u2ps_dwc2_dcd_irq();
    commit();
  }
}

static void complete(void)
{
  if (core.nak_mode == 1)
    core.nak = true;
  core.epena = false;
  core.doepint |= DOEPINT_XFRC;
  core.doeptsiz = 0; /* DATA0, PKTCNT 0, XFRSIZ 0: one full packet */
}

static unsigned long sent, lost, corrupted;
static bool late_next, same_pass_next, in_pass_next, crc_next;

static bool defer_eopf; /* the pass of this EOPF waits for the packet's completion */

/* The host's packet in the current microframe, at its token */
static void host_token(void)
{
  sent++;
  if (crc_next) {
    crc_next = false;
    corrupted++; /* flushed from the Rx FIFO: no NAK, no XFRC */
  } else if (core.epena && core.eonum == ((core.fn & 1) != 0) && !core.busy) {
    core.busy = true;
    if (core.nak_mode == 0)
      core.nak = true;
    if (late_next) {
      late_next = false;
      core.draining = true;
      defer_eopf = same_pass_next;
      same_pass_next = false;
      core.in_pass = in_pass_next;
      in_pass_next = false;
    } else {
      complete();
    }
  } else {
    lost++;
  }
}

/* One microframe; the host sends in it if 'send' */
static void step(bool send)
{
  defer_eopf = false;
  /* The SOF; a packet that completed late; the pass of a deferred EOPF,
   * 2 us after the SOF */
  if (core.draining) {
    core.draining = false;
    complete();
  }
  irq();
  /* The token at 5 us; the pass of its XFRC at 25 us */
  if (send)
    host_token();
  irq();
  /* The EOPF at 118.75 us; its pass at 119 us */
  core.gintsts |= GINT_EOPF;
  if (core.draining || (core.epena && (core.every_eopf || core.eonum == ((core.fn & 1) != 0))))
    core.gintsts |= GINT_INCOMPISOOUT;
  if (core.hold_after_eopf > 0) {
    core.hold_until = core.abs + core.hold_after_eopf;
    core.hold_after_eopf = 0;
  }
  if (!defer_eopf)
    irq();
  if (core.sof_early) {
    core.sof_early = false; /* the SOF came inside the pass */
  } else {
    core.fn = (core.fn + 1) & 0x3FFF;
    core.abs++;
  }
}

static int nak_mode; /* for every core of this process (test_models) */
static bool follow_off; /* the data endpoint's follow off (u2ps_dwc2_dcd_mode_t.out_follow) */

/* A stream open in microframe fn, with a feedback endpoint (never polled,
 * so its follow keeps EOPF unmasked) if fb; the port is started first */
static void reopen_at(long fn, bool fb)
{
  core.fn = fn;
  u2ps_stream_t s = {.ep_out = EP, .ep_fb = fb ? 2u : 0u, .fb_interval = 4, .fb_size = 4,
                     .fb_value = 0x00060000u, .mps = 64, .stride = 64, .count = 8, .mem = mem};
  CHECK(u2ps_dport_stream_open(&s));
  commit();
  CHECK(core.epena);
  CHECK(((core.gintmsk & GINT_INCOMPISOOUT) != 0u) == !follow_off);
  CHECK(((core.gintmsk & GINT_EOPF) != 0u) == (!follow_off || fb)); /* the follow from the open */
  if (core.sof_early) {
    core.sof_early = false; /* SOFs came during the open: in that microframe now */
  } else {
    core.fn = (fn + 1) & 0x3FFF; /* armed for this microframe */
    core.abs++;
  }
}


static void open_fb(long fn, bool fb)
{
  memset(&core, 0, sizeof core);
  core.nak_mode = nak_mode;
  memset(&app, 0, sizeof app);
  sent = lost = corrupted = 0;
  late_next = same_pass_next = in_pass_next = crc_next = false;
  core.in_pass_addr = A_DAINT;
  CHECK_EQ(u2ps_dport_init(), U2PS_DEV_OK);
  commit();
  u2ps_dwc2_dcd_mode_t m = {.fb_follow = true, .iiso_window = true, .out_follow = !follow_off};
  u2ps_dwc2_dcd_mode(&m);
  reopen_at(fn, fb);
}

static void open_at(long fn)
{
  open_fb(fn, false);
}

/* Every packet delivered in the microframe it came in */
static unsigned stamps_wrong(long first)
{
  unsigned bad = 0;
  for (unsigned i = 0; i < app.packets && i < MAXPK; i++)
    if (app.uframe[i] != (u2ps_uframe_t)((first + (long)i) & 0x3FFF))
      bad++;
  return bad;
}

/* ---- The tests ---------------------------------------------------------------- */

static void test_streaming(int v)
{
  {
    open_at(1000);
    core.every_eopf = v >= 3;
    long first = core.fn;
    for (int i = 0; i < 1000; i++) {
      if (i % 100 == 50) {
        late_next = v == 1 || v == 2 || v == 4;
        same_pass_next = v == 2;
      }
      step(true);
    }
    u2ps_dwc2_dcd_diag_t dg;
    u2ps_dwc2_dcd_diag(&dg);
    CHECK_EQ(lost, 0);
    CHECK_EQ(app.packets, 1000);
    CHECK_EQ(stamps_wrong(first), 0);
    CHECK_EQ(app.holes, 0);
    CHECK_EQ(dg.out_gap_follows, 0);
    if (v == 0)
      CHECK_EQ(dg.incompisoout, 0);
    /* every_eopf: one of the other parity per packet, nothing done; a late
     * packet's own INCOMPISOOUT finds NAKSTS set: not counted */
    if (v < 3)
      CHECK_EQ(dg.incompisoout_other, 0);
    else
      CHECK(dg.incompisoout_other >= 900u);
    CHECK_EQ(dg.out_gap_cleared, 0);
    CHECK_EQ(u2ps_dport_stats()->errors, 0);
  }
}

static void test_gap(int gi)
{
  static const long gaps[] = {1, 2, 3, 4, 30, 31};
  {
    long g = gaps[gi % 6];
    open_fb(2000, gi >= 6);
    for (int k = 0; k < 20; k++)
      step(true);
    unsigned before = app.packets;
    long resume = core.fn + g;
    for (long k = 0; k < g; k++)
      step(false);
    if (g >= 3)
      CHECK((core.gintmsk & GINT_EOPF) != 0u); /* the follow runs */
    step(true);
    for (int k = 0; k < 9; k++)
      step(true);
    unsigned long want_lost = g == 1 ? 1u : 0u;
    CHECK_EQ(lost, want_lost);
    CHECK_EQ(app.packets, before + 10u - want_lost);
    if (g > 1 && app.packets > before)
      CHECK_EQ(app.uframe[before], (u2ps_uframe_t)(resume & 0x3FFF));
    CHECK_EQ(app.holes, (unsigned long)g + want_lost);
    if (gi < 6)
      CHECK((core.gintmsk & GINT_EOPF) == 0u);      /* the follow is over */
    CHECK((core.gintmsk & GINT_INCOMPISOOUT) != 0u); /* still unmasked */
    u2ps_dwc2_dcd_diag_t dg;
    u2ps_dwc2_dcd_diag(&dg);
    CHECK_EQ(dg.out_gap_follows, g >= 3 ? 1u : 0u);
    /* one EOPF write per microframe from the follow's start to the
     * resumed packet: none twice */
    CHECK_EQ(dg.out_follows, g >= 3 ? (uint32_t)g - 3u : 0u);
    CHECK_EQ(dg.out_follow_late, 0);
    CHECK_EQ(u2ps_dport_stats()->errors, 0);
  }
}

/* A packet that completes inside the pass of its INCOMPISOOUT, after the
 * handler read OTG_DAINT: v 0, the host's first packet after a single
 * skip and a lost one, when the skip's INCOMPISOOUT is the first evidence;
 * v 1, the first packet after a gap of 30, while the follow runs; v 2, as
 * v 0, completing right after the handler read OTG_DOEPCTLx. The packet
 * set NAKSTS as it entered the Rx FIFO, and EPENA clears at its
 * completion, so neither INCOMPISOOUT nor EOPF changes the parity under
 * it: it is delivered in its own microframe. */
static void test_in_pass(int v)
{
  open_at(3000);
  for (int k = 0; k < 20; k++)
    step(true);
  unsigned before = app.packets;
  long resume;
  if (v == 2) {
    /* as v 0, completing right after the handler read OTG_DOEPCTLx */
    core.in_pass_addr = A_DOEPCTL;
    v = 0;
  }
  if (v == 0) {
    step(false); /* the first evidence */
    step(true);  /* the other parity: lost */
    resume = core.fn;
  } else {
    for (int k = 0; k < 30; k++)
      step(false);
    resume = core.fn;
  }
  late_next = true;
  in_pass_next = true;
  for (int k = 0; k < 10; k++)
    step(true);
  CHECK_EQ(lost, v == 0 ? 1u : 0u);
  CHECK_EQ(app.packets, before + 10u);
  unsigned bad = 0;
  for (unsigned i = before; i < app.packets && i < MAXPK; i++)
    bad += app.uframe[i] != (u2ps_uframe_t)((resume + (long)(i - before)) & 0x3FFF);
  CHECK_EQ(bad, 0);
  CHECK_EQ(u2ps_dport_stats()->errors, 0);
}

/* One miss, then the next packet of the armed parity completes past its
 * EOPF (in its own pass): v 0, a gap of 2; v 1, a single skip and a packet
 * lost to the other parity; v 2, a corrupted packet and the next one
 * lost; v 3, a gap of 30, whose first packet completes late while the
 * follow runs. Its INCOMPISOOUT finds NAKSTS set: no gap evidence, no
 * parity write under it; it is delivered in its own microframe. */
static void test_false_gap(int v)
{
  static const unsigned long want_lost[] = {0, 1, 1, 0};
  open_at(4000);
  for (int k = 0; k < 20; k++)
    step(true);
  unsigned before = app.packets;
  if (v == 0) {
    step(false);
    step(false);
  } else if (v == 1) {
    step(false);
    step(true);
  } else if (v == 2) {
    crc_next = true;
    step(true);
    step(true);
  } else {
    for (int k = 0; k < 30; k++)
      step(false);
  }
  long resume = core.fn;
  late_next = true;
  for (int k = 0; k < 10; k++)
    step(true);
  CHECK_EQ(lost, want_lost[v]);
  CHECK_EQ(app.packets, before + 10u);
  unsigned bad = 0;
  for (unsigned i = before; i < app.packets && i < MAXPK; i++)
    bad += app.uframe[i] != (u2ps_uframe_t)((resume + (long)(i - before)) & 0x3FFF);
  CHECK_EQ(bad, 0);
  CHECK_EQ(app.holes, v == 3 ? 30u : 2u);
  u2ps_dwc2_dcd_diag_t dg;
  u2ps_dwc2_dcd_diag(&dg);
  CHECK_EQ(dg.out_gap_follows, v == 3 ? 1u : 0u);
  /* the miss's evidence, cleared by the late packet's XFRC */
  CHECK_EQ(dg.out_gap_cleared, v == 3 ? 0u : 1u);
  CHECK_EQ(u2ps_dport_stats()->errors, 0);
}

/* A gap of 3: the SOF of the host's first microframe after it comes inside
 * the pass whose INCOMPISOOUT makes the gap sure, after its FNSOF read.
 * The parity is written for that microframe, late (counted), and the pass
 * writes it once: with a feedback endpoint (v 1) EOPF is in the same pass,
 * and its follow must not write again from the new FNSOF. The host's
 * packet in that microframe comes after the pass and is received. */
static void test_sof_in_pass(int v)
{
  open_fb(5000, v == 1);
  for (int k = 0; k < 20; k++)
    step(true);
  unsigned before = app.packets;
  step(false);
  step(false);
  core.sof_addr = A_DSTS;
  core.sof_count = 1; /* right after out_incomplete reads FNSOF */
  step(false);
  long resume = core.fn;
  for (int k = 0; k < 10; k++)
    step(true);
  CHECK_EQ(lost, 0);
  CHECK_EQ(app.packets, before + 10u);
  if (app.packets > before)
    CHECK_EQ(app.uframe[before], (u2ps_uframe_t)(resume & 0x3FFF));
  u2ps_dwc2_dcd_diag_t dg;
  u2ps_dwc2_dcd_diag(&dg);
  CHECK_EQ(dg.out_gap_follows, 1);
  CHECK_EQ(dg.out_follow_late, 1);
  CHECK_EQ(dg.out_follows, 0);
}

/* A bus reset (v 0, 2) or a close (v 1, 3) during a gap's follow (v 0,
 * 1) or with one gap evidence pending (v 2, 3): EOPF and INCOMPISOOUT are
 * masked and the endpoint disabled. A new stream then opens: a single
 * skip loses the next packet (no evidence is left from the old stream),
 * and a gap of 5 is followed. */
static void test_end_in_follow(int v)
{
  open_at(6000);
  for (int k = 0; k < 20; k++)
    step(true);
  for (int k = 0; k < (v < 2 ? 20 : 1); k++)
    step(false);
  CHECK(((core.gintmsk & GINT_EOPF) != 0u) == (v < 2));
  v &= 1;
  if (v == 0) {
    core.gintsts |= GINT_USBRST;
    irq();
  } else {
    u2ps_dport_stream_close();
    commit();
    irq();
    CHECK(core.closed);
  }
  CHECK((core.gintmsk & (GINT_EOPF | GINT_INCOMPISOOUT)) == 0u);
  CHECK(!core.epena);
  reopen_at(core.fn + 5, false);
  for (int k = 0; k < 20; k++)
    step(true);
  unsigned before = app.packets;
  step(false); /* would be the second evidence if any were left */
  long resume = core.fn;
  for (int k = 0; k < 10; k++)
    step(true);
  CHECK_EQ(lost, 1); /* a single skip, as in 0.1.0: no evidence left from the old stream */
  CHECK_EQ(app.packets, before + 9u);
  u2ps_dwc2_dcd_diag_t dg;
  u2ps_dwc2_dcd_diag(&dg);
  uint32_t follows = dg.out_gap_follows;
  (void)resume;
  for (int k = 0; k < 5; k++)
    step(false);
  resume = core.fn;
  before = app.packets;
  for (int k = 0; k < 10; k++)
    step(true);
  CHECK_EQ(lost, 1);
  CHECK_EQ(app.packets, before + 10u);
  if (app.packets > before)
    CHECK_EQ(app.uframe[before], (u2ps_uframe_t)(resume & 0x3FFF));
  u2ps_dwc2_dcd_diag(&dg);
  CHECK_EQ(dg.out_gap_follows, follows + 1u);
}

/* A follow runs (a gap of 20). v 0: the EOPF passes of microframes k and
 * k + 1 wait until k + 2 starts, and the host's packet of k + 2 comes right
 * after that late pass read OTG_DOEPCTLx: the late pass writes nothing, so
 * the packet keeps its parity and its stamp. v 1: the passes from the EOPF
 * of k wait until k + 3 starts, the host's packet of k + 1 is lost (the
 * parity of k stays armed) and that of k + 2 ends the follow at its XFRC:
 * counted in out_follow_late. v 2: the first packets after the gap all
 * complete past their EOPF: EOPF is masked again at the EOPF after the
 * first one's XFRC, as the follow is over. */
static void test_follow_window(int v)
{
  open_at(8000);
  for (int k = 0; k < 20; k++)
    step(true);
  for (int k = 0; k < 20; k++)
    step(false);
  unsigned before = app.packets;
  long k0 = core.fn;
  u2ps_dwc2_dcd_diag_t dg;
  if (v == 0) {
    core.hold_until = core.abs + 2;
    step(false);
    step(false);
    core.rx_addr = A_DOEPCTL;
    core.rx_count = 2; /* out_incomplete's read, then stream_eopf's */
    step(false);       /* the packet comes inside the pass */
    for (int k = 0; k < 9; k++)
      step(true);
    CHECK_EQ(lost, 0);
    CHECK_EQ(app.packets, before + 10u);
    if (app.packets > before)
      CHECK_EQ(app.uframe[before], (u2ps_uframe_t)((k0 + 2) & 0x3FFF));
    CHECK_EQ(app.holes, 22u);
  } else if (v == 1) {
    core.hold_until = core.abs + 3;
    step(false);
    step(true); /* k + 1: the other parity */
    step(true); /* k + 2: received, its XFRC waits */
    for (int k = 0; k < 9; k++)
      step(true);
    CHECK_EQ(lost, 1);
    u2ps_dwc2_dcd_diag(&dg);
    CHECK(dg.out_follow_late >= 1u);
  } else {
    for (int k = 0; k < 5; k++) {
      late_next = true;
      step(true);
    }
    CHECK_EQ(lost, 0);
    CHECK((core.gintmsk & GINT_EOPF) == 0u);
  }
  u2ps_dwc2_dcd_diag(&dg);
  CHECK_EQ(dg.eonum_mismatch, 0);
}

/* The scenarios of test_models. The packets 0.1.0 loses in each are
 * measured in a run with the data endpoint's follow off (out_follow
 * false), which handles a gap as 0.1.0 did: the parity armed after the
 * last packet stays, so a host packet in a microframe of the other parity
 * is lost, and a corrupted packet leaves the endpoint armed for its own
 * microframe's parity. (It also turns the follow from the open off, which
 * the scenarios do not reach: their first packet comes in the microframe
 * the open armed.) */
enum {
  S_LATE,       /* 1,000 packets, every 100th completing late, in its own pass */
  S_LATE_SAME,  /* ... served with its XFRC */
  S_GAP1,       /* gaps of 1, 2, 3, 30 and 31 microframes */
  S_GAP2,
  S_GAP3,
  S_GAP30,
  S_GAP31,
  S_GAP2_LATE,  /* a gap of 2, then the first packet completes late */
  S_SKIP_LATE,  /* a skip, a packet lost to the other parity, then a late one */
  S_CRC_LATE,   /* a corrupted packet, the next one lost, then a late one */
  S_GAP30_LATE, /* gaps of 30 and 31, then the first packet completes late */
  S_GAP31_LATE,
  S_GAP30_INPASS, /* ... and completes inside the EOPF pass, after the DAINT read */
  S_COUNT
};
static const char *const s_names[S_COUNT] = {
  "late", "late_same", "gap1", "gap2", "gap3", "gap30", "gap31", "gap2_late", "skip_late",
  "crc_late", "gap30_late", "gap31_late", "gap30_inpass"};
/* What this port loses, per NAK model: 0 at reception, 1 at completion,
 * 2 never. With a NAK set late or never, the NAKSTS guard sees no packet
 * in reception, and a miss followed by a late packet, or a late first
 * packet after a gap, loses one packet more than 0.1.0. */
static const unsigned long lost_now[3][S_COUNT] = {
  {0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0},
  {0, 0, 1, 0, 0, 0, 0, 1, 2, 2, 1, 1, 0},
  {0, 0, 1, 0, 0, 0, 0, 1, 2, 2, 1, 1, 0},
};

/* One scenario; returns the packets lost, *bad if the first packet after
 * the gap was stamped wrong */
static unsigned long scenario(int s, bool *bad_out)
{
  open_at(7000);
  for (int k = 0; k < 20; k++)
    step(true);
  unsigned before = app.packets;
  int n = 10;
  long resume;
  switch (s) {
  case S_LATE:
  case S_LATE_SAME:
    n = 1000;
    break;
  case S_GAP1: case S_GAP2: case S_GAP3: case S_GAP30: case S_GAP31:
    for (long k = 0, g = s == S_GAP1 ? 1 : s == S_GAP2 ? 2 : s == S_GAP3 ? 3 : s == S_GAP30 ? 30 : 31;
         k < g; k++)
      step(false);
    break;
  case S_GAP2_LATE:
    step(false);
    step(false);
    break;
  case S_SKIP_LATE:
    step(false);
    step(true);
    break;
  case S_CRC_LATE:
    crc_next = true;
    step(true);
    step(true);
    break;
  default:
    for (int k = 0; k < (s == S_GAP31_LATE ? 31 : 30); k++)
      step(false);
    break;
  }
  resume = core.fn;
  if (s >= S_GAP2_LATE)
    late_next = true;
  if (s == S_GAP30_INPASS)
    in_pass_next = true;
  for (int i = 0; i < n; i++) {
    if ((s == S_LATE || s == S_LATE_SAME) && i % 100 == 50) {
      late_next = true;
      same_pass_next = s == S_LATE_SAME;
    }
    step(true);
  }
  unsigned bad = 0;
  if (s >= S_GAP2 && s != S_SKIP_LATE && s != S_CRC_LATE && s != S_GAP3 && s != S_GAP31 &&
      app.packets > before)
    bad = app.uframe[before] != (u2ps_uframe_t)(resume & 0x3FFF);
  *bad_out = bad != 0u;
  return lost;
}

static void test_models(int idx)
{
  int s = idx % S_COUNT;
  nak_mode = idx / S_COUNT;
  /* 0.1.0's losses: the same scenario with the gap follow off, in a
   * process of its own (its exit status) */
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    bool b;
    follow_off = true;
    unsigned long l = scenario(s, &b);
    _exit((int)(l < 100u ? l : 100u));
  }
  int status = 0;
  CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status));
  unsigned long lost_010 = (unsigned long)WEXITSTATUS(status);
  bool bad;
  scenario(s, &bad);
  printf("nak model %d, %-13s: lost %lu (0.1.0: %lu)%s\n", nak_mode, s_names[s], lost, lost_010,
         bad ? ", first packet stamped wrong" : "");
  CHECK_EQ(lost, lost_now[nak_mode][s]);
  /* With NAK at reception, every packet is stamped right, and nothing is
   * lost that 0.1.0 kept */
  if (nak_mode == 0) {
    CHECK(!bad);
    CHECK(lost <= lost_010);
  }
  /* A late packet's own INCOMPISOOUT: with NAK at reception it finds
   * NAKSTS set (incomp_nak); otherwise it is taken as evidence, which that
   * packet's XFRC clears (out_gap_own) */
  u2ps_dwc2_dcd_diag_t dg;
  u2ps_dwc2_dcd_diag(&dg);
  if (s == S_LATE) {
    CHECK_EQ(dg.incomp_nak, nak_mode == 0 ? 10u : 0u);
    CHECK_EQ(dg.out_gap_own, nak_mode == 0 ? 0u : 10u);
    CHECK_EQ(dg.out_gap_cleared, nak_mode == 0 ? 0u : 10u);
  }
  CHECK_EQ(dg.eonum_mismatch, 0);
}

/* Passes served after the next SOF, early in a microframe for an event of
 * an earlier one. v 0: a follow runs (a gap of 20); the EOPF pass of k is
 * served early in k + 1, and that of k + 1 early in k + 2, where the
 * host's packet comes right after the pass read OTG_DOEPCTLx; v 1: the
 * same with k = 0x3FFE, so that k + 2 is 0x0000. v 2: the INCOMPISOOUT of
 * the gap's first microframe L + 1 is served early in L + 3, and that of
 * L + 3 early in L + 5, where the host's packet comes right after the
 * pass read OTG_DOEPCTLx. v 3: the open's arm reads FNSOF, and the open
 * unmasks EOPF three microframes later; the host's first packet comes in
 * the microframe after that. v 4, 5: as v 0, with no packet then: the
 * follow goes on after the late passes, and the host resumes 5 or 6
 * microframes later: no packet lost, each stamped in its own microframe.
 * v 6: only the INCOMPISOOUT of L + 1 is served early in L + 3, and the
 * host resumes in L + 4. v 0 to 3 and 6 are a known limit of this release
 * (README, "Deviations and known limits"): a packet is lost, counted in
 * out_follow_late in v 0 to 3 and by no counter in v 6. */
static void test_late_passes(int v)
{
  long at = v == 1 ? 0x3FFE - 101 : 9000;
  unsigned before;
  long resume;
  open_at(at);
  if (v == 3) {
    u2ps_dport_stream_close();
    commit();
    irq();
    CHECK(core.closed);
    /* three SOFs at the open's second OTG_DOEPCTLx access, its arm */
    core.sof_addr = A_DOEPCTL;
    core.sof_n = 3;
    core.sof_count = 2;
    reopen_at(core.fn + 5, false);
    before = app.packets;
    resume = core.fn + 1;
    step(false);
    for (int k = 0; k < 10; k++)
      step(true);
    u2ps_dwc2_dcd_diag_t dg;
    u2ps_dwc2_dcd_diag(&dg);
    CHECK_EQ(lost, 1);
    CHECK_EQ(app.packets, before + 9u);
    if (app.packets > before)
      CHECK_EQ(app.uframe[before], (u2ps_uframe_t)((resume + 1) & 0x3FFF));
    CHECK_EQ(dg.out_follow_late, 2); /* the model's count, exact */
    return;
  }
  for (int k = 0; k < 80; k++)
    step(true);
  if (v == 6) {
    core.hold_after_eopf = 2; /* EOPF of L + 1: served at the start of L + 3 */
    step(false);
    step(false);
    step(false);
  } else if (v == 2) {
    core.hold_after_eopf = 2; /* EOPF of L + 1: served at the start of L + 3 */
    step(false);
    step(false);
    core.hold_after_eopf = 2; /* EOPF of L + 3: served at the start of L + 5 */
    step(false);
    step(false);
  } else {
    for (int k = 0; k < 20; k++)
      step(false);
    CHECK((core.gintmsk & GINT_EOPF) != 0u); /* the follow runs */
    if (v == 1)
      CHECK_EQ(core.fn, 0x3FFE);
    core.hold_after_eopf = 1;
    step(false); /* k: its EOPF pass is served early in k + 1 */
    core.hold_after_eopf = 1;
    step(false); /* k + 1: likewise, early in k + 2 */
  }
  if (v >= 4) {
    for (int k = 0; k < (v == 4 ? 5 : v == 5 ? 6 : 0); k++)
      step(false);
    before = app.packets;
    resume = core.fn;
    step(true);
  } else {
    before = app.packets;
    resume = core.fn;
    core.rx_addr = A_DOEPCTL;
    core.rx_count = 1;
    step(false); /* the packet comes inside the pass */
  }
  for (int k = 0; k < 9; k++)
    step(true);
  /* Stamps outside the microframes the host sent in (resume to
   * resume + 9) are wrong; the others must rise */
  unsigned wrong = 0, order = 0;
  long prev = -1;
  for (unsigned i = before; i < app.packets && i < MAXPK; i++) {
    long off = (long)((app.uframe[i] - (u2ps_uframe_t)(resume & 0x3FFF)) & 0x3FFF);
    if (off > 9) {
      wrong++;
      continue;
    }
    order += off <= prev;
    prev = off;
  }
  u2ps_dwc2_dcd_diag_t dg;
  u2ps_dwc2_dcd_diag(&dg);
  CHECK_EQ(order, 0);
  if (v == 4 || v == 5) {
    CHECK_EQ(lost, 0);
    CHECK_EQ(wrong, 0);
  } else if (v == 6) {
    /* the packet of L + 4 lost, and no counter shows it */
    CHECK_EQ(lost, 1);
    CHECK_EQ(wrong, 0);
    CHECK_EQ(dg.out_follow_late, 0);
    CHECK_EQ(dg.incompisoout_other, 0);
  } else {
    /* the packet under the write stamped a microframe early, the next one
     * lost */
    CHECK_EQ(lost, 1);
    CHECK_EQ(wrong, 1);
    if (app.packets > before)
      CHECK_EQ(app.uframe[before], (u2ps_uframe_t)((resume - 1) & 0x3FFF));
    CHECK_EQ(dg.out_follow_late, v == 2 ? 1 : 2); /* the model's counts, exact */
  }
}

/* The core's EONUM changed behind the port's back in a gap: counted in
 * eonum_mismatch at the next INCOMPISOOUT */
static void test_eonum(int v)
{
  (void)v;
  open_at(10000);
  for (int k = 0; k < 20; k++)
    step(true);
  core.eonum = !core.eonum;
  step(false);
  step(false);
  u2ps_dwc2_dcd_diag_t dg;
  u2ps_dwc2_dcd_diag(&dg);
  CHECK(dg.eonum_mismatch >= 1u);
}

/* The data endpoint's follow off: INCOMPISOOUT stays masked, and a gap of
 * 31 loses the host's first packet after it, as in 0.1.0 */
static void test_gap_off(int v)
{
  (void)v;
  follow_off = true;
  open_at(11000);
  CHECK((core.gintmsk & GINT_INCOMPISOOUT) == 0u);
  for (int k = 0; k < 20; k++)
    step(true);
  for (int k = 0; k < 31; k++)
    step(false);
  for (int k = 0; k < 10; k++)
    step(true);
  CHECK_EQ(lost, 1);
  u2ps_dwc2_dcd_diag_t dg;
  u2ps_dwc2_dcd_diag(&dg);
  CHECK_EQ(dg.out_gap_follows, 0);
  CHECK_EQ(dg.incompisoout, 0);
}

/* One case in a child process, with its own counts; returns true if it
 * passed (its failed checks print themselves) */
static bool run(void (*f)(int), int arg, const char *name)
{
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    check_count = check_failures = 0;
    f(arg);
    fflush(stdout);
    _exit(check_failures == 0 ? 0 : 1);
  }
  int status = 0;
  bool ok = pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0;
  printf("%s %d: %s\n", name, arg, ok ? "passed" : "FAILED");
  return ok;
}

int main(void)
{
  /* Streaming: plain, late completions in their own pass and in their
   * XFRC's pass, INCOMPISOOUT at every EOPF, and both */
  for (int v = 0; v < 5; v++)
    CHECK(run(test_streaming, v, "streaming"));
  /* Gaps of 1, 2, 3, 4, 30 and 31 microframes, without and with a
   * feedback endpoint that is never polled */
  for (int g = 0; g < 12; g++)
    CHECK(run(test_gap, g, "gap"));
  /* An SOF inside the pass that starts a gap's follow */
  for (int v = 0; v < 2; v++)
    CHECK(run(test_sof_in_pass, v, "sof_in_pass"));
  /* A bus reset and a close during a gap's follow, then a new stream */
  for (int v = 0; v < 4; v++)
    CHECK(run(test_end_in_follow, v, "end_in_follow"));
  /* Completions inside a pass */
  for (int v = 0; v < 3; v++)
    CHECK(run(test_in_pass, v, "in_pass"));
  /* A miss, then a packet completing late */
  for (int v = 0; v < 4; v++)
    CHECK(run(test_false_gap, v, "false_gap"));
  /* Packets inside a follow's late pass, a follow ended by an XFRC after
   * missed passes, late packets after a follow */
  for (int v = 0; v < 3; v++)
    CHECK(run(test_follow_window, v, "follow_window"));
  /* Passes served after the next SOF, and a late unmask at the open (a
   * known limit) */
  for (int v = 0; v < 7; v++)
    CHECK(run(test_late_passes, v, "late_passes"));
  CHECK(run(test_eonum, 0, "eonum"));
  CHECK(run(test_gap_off, 0, "gap_off"));
  /* The scenarios under each NAK model, against 0.1.0 */
  for (int i = 0; i < 3 * S_COUNT; i++)
    CHECK(run(test_models, i, "models"));
  return check_done();
}
