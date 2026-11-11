/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_hcd.c: the DWC2 host port, ARM builds only: every u2ps_hport_*
 * call of u2ps_hport.h and the OTG_HS interrupt handler, in buffer DMA
 * mode. Its decisions are in u2ps_dwc2_hctl.c (root port, control
 * channel) and u2ps_dwc2_hiso.c (stream channels, FIFO RAM); both build on
 * the PC too.
 *
 * Manuals: RM0456 Rev 7 chapter 73 (cited by section and page) and RM0431
 * chapter 32 (cited by section), whose host registers are the same.
 * "Host programming model" is RM0456 73.15.5, pp. 3403-3436, and RM0431
 * 32.16.5.
 *
 * Channels: 0 the control channel (endpoint 0, every stage of every
 * request), 1 the stream's isochronous OUT data channel, 2 its isochronous
 * IN feedback channel. In DMA mode the core halts a channel at the end of
 * each channel transfer (CHH, pp. 3426-3433), and the handler reads
 * OTG_HCINTx for the cause at the halt. The port unmasks CHH and AHBERR
 * only and decides only at CHH (ch_halted()). The manual's interrupt
 * routines that disable a channel at STALL, TXERR, BBERR, DTERR and FRMOR
 * (pp. 3404-3405, 3409-3410, 3422-3424) also write the transmit FIFO
 * ("The application is expected to write the data packets into the
 * transmit FIFO", p. 3410): they are slave mode's.
 * CHDIS is never written to the stream's channels: each is armed for the
 * next microframe at most and halts by itself (pp. 3431-3433), and CHENA,
 * of access type rs (p. 3360), would enable again a channel that halted
 * between a read and a write. The control channel is disabled with CHDIS
 * and CHENA (an abort, u2ps_hport_disable), and every channel is flushed
 * with CHDIS alone (CHENA written 0, p. 3404) where the port sends no more
 * tokens: a disconnect, a port the hardware disabled.
 *
 * Order of events in one pass of the handler:
 *  1. DISCINT: the port flushes every channel, without a wait, and drops
 *     what was in flight, then on_disconnect; the channels' halts of this
 *     pass are not served;
 *  2. the data channel's halt (report, re-arm, then upcalls), first,
 *     because its re-arm has the tightest deadline; OTG_HCINTx is read
 *     before OTG_HFNUM;
 *  3. the feedback channel's halt;
 *  4. the control channel's halt;
 *  5. the root port (OTG_HPRT: connect, the enable that ends a reset, a
 *     disable by the hardware), then WKUPINT (on_wakeup);
 *  6. SOF, the feedback's clock while the data channel is idle; what a
 *     call from thread left for the handler (on_closed of a close whose
 *     channels were idle, on_control of a refused transfer, on_enabled of
 *     an enable that came while PRST was still set); IPXFR (counted),
 *     OTGINT and MMIS.
 *
 * The packet path, the handler's pass and the stream channels' halts with
 * everything they call, and the queue call, is in section .text.u2ps_fast,
 * and the port's state in .bss.u2ps_fast (U2PS_FAST_TEXT, U2PS_FAST_BSS in
 * u2ps_types.h). The port is the only writer of OTG_GINTMSK, so it keeps
 * its value in its state.
 */
#include <stddef.h>
#include <string.h>

#include "u2ps_dwc2.h"
#include "u2ps_dwc2_hcd.h"
#include "u2ps_dwc2_hctl.h"
#include "u2ps_dwc2_hiso.h"
#include "u2ps_dwc2_regs.h"
#include "u2ps_hport.h"
#include "u2ps_hport_cfg.h"

#ifndef U2PS_HOST_QUEUE_MAX
#error "u2ps_dwc2_hcd.c: U2PS_HOST_QUEUE_MAX is missing: link u2ps_host, whose public compile definitions set it"
#endif
#ifndef U2PS_TEST_INJECT
#define U2PS_TEST_INJECT 0
#endif

_Static_assert(U2PS_HOST_QUEUE_MAX >= 1u && U2PS_HOST_QUEUE_MAX <= 255u,
               "U2PS_HOST_QUEUE_MAX must be 1..255 for the DWC2 host port's queue: set "
               "-DU2PS_HOST_QUEUE_MAX (default 16)");
_Static_assert((int)U2PS_DWC2_CTL_OK == (int)U2PS_HPORT_CONTROL_OK &&
                 (int)U2PS_DWC2_CTL_STALL == (int)U2PS_HPORT_CONTROL_STALL &&
                 (int)U2PS_DWC2_CTL_ERROR == (int)U2PS_HPORT_CONTROL_ERROR &&
                 (int)U2PS_DWC2_CTL_ABORTED == (int)U2PS_HPORT_CONTROL_ABORTED,
               "u2ps_dwc2_ctl_result_t must follow u2ps_hport_control_t");
_Static_assert(U2PS_HPORT_ALIGN % 4u == 0u, "the DMA makes word accesses (RM0456 73.14.3)");

/* The channels */
#define CH_CTL   0u
#define CH_OUT   1u
#define CH_FB    2u
#define CHANNELS 3u
#define CH_ALL   ((1u << CH_CTL) | (1u << CH_OUT) | (1u << CH_FB))

/* Every bit of OTG_HCINTx (73.14.33, p. 3362) */
#define HCINT_ALL 0x7FFu
/* The interrupts of a channel at work (OTG_HCINTMSKx, 73.14.34, p. 3363):
 * CHH, at which the port decides, and AHBERR, kept for it. The other bits
 * are set at the halt although masked (inferred; not yet checked on a board). */
#define HCINT_RUN (U2PS_DWC2_HCINT_CHH | U2PS_DWC2_HCINT_AHBERR)

/* The bound of each channel halt's wait at u2ps_hport_disable(): a
 * periodic channel halts at its turn in the next microframe, a control
 * channel at its turn in the frame, 1 ms at full speed (inferred bound) */
#define STOP_US 1500u
/* The core's change to host mode: at least 25 ms (73.14.4, p. 3324), then
 * CMOD awaited at most 10 ms more */
#define HOST_MODE_US 25000u
#define CMOD_US      10000u

/* The largest data packet: one transaction per microframe (USB 2.0 5.6.3) */
#define DATA_MPS_MAX 1024u

/* The largest endpoint 0 packet (USB 2.0 9.6.1) */
#define EP0_MPS_MAX 64u

/* Port memory, in DMA memory, each buffer on its own 32-byte lines: the
 * feedback buffers, armed in turn so that a poll does not overwrite the
 * value on_fb() is reading, and the buffer of a status stage, which moves
 * no data: an IN status stage reserves one packet of mps0 (73.14.35, p.
 * 3365), so it holds the largest */
static _Alignas(32) uint8_t fb_buf[2][U2PS_DWC2_HFB_MPS_MAX]
  __attribute__((section(U2PS_DMA_SECTION)));
static _Alignas(32) uint8_t zlp_buf[EP0_MPS_MAX] __attribute__((section(U2PS_DMA_SECTION)));

/* A queued packet */
typedef struct {
  const uint8_t *data;
  uint16_t len;
} packet_t;

static struct {
  bool up;     /* u2ps_hport_init() succeeded */
  bool phy_up; /* the PHY has started; it stays on */
  u2ps_dwc2_err_t init_err;
  uint8_t port;       /* u2ps_dwc2_port_t */
  u2ps_speed_t speed; /* the speed of the last enable */
  uint32_t errors;    /* port errors outside the logic's counters */
  uint32_t gintmsk;   /* OTG_GINTMSK as last written (gintmsk_set) */
  uint32_t pending[CHANNELS]; /* per channel: OTG_HCINTx bits of an error before its halt */
  /* The control transfer */
  u2ps_dwc2_ctl_t ctl;
  uint8_t ctl_addr;
  const uint8_t *setup;
  uint8_t *data;
  bool ctl_owed;      /* on_control(ERROR, 0) of a transfer the port refused */
  bool ctl_kick;      /* an abort found the channel idle: its halt is checked in the handler */
  bool ctl_flushed;   /* the channel was flushed: a halt that follows is the flush's */
  bool ctl_wait;      /* ctl_next waits for the halt of a channel enabled again (ctl_arm) */
  u2ps_dwc2_ctl_step_t ctl_next;
  bool enable_owed;   /* on_enabled of an enable kept from PRST (u2ps_hport_reset) */
  /* The stream */
  bool open;          /* open: packets are sent */
  bool closing;       /* closed; the channels' halts are awaited */
  bool closed_owed;   /* the close found both channels idle: on_closed in the handler */
  bool sof_on;        /* SOF unmasked: the feedback's clock */
  uint8_t ep_fb;      /* the feedback endpoint; 0: none */
  uint8_t fb_i;       /* the feedback buffer of the poll armed last */
  uint16_t fb_mps;
  uint32_t out_char, fb_char; /* OTG_HCCHARx of the channels, CHENA and ODDFRM 0 */
  packet_t q[U2PS_HOST_QUEUE_MAX];
  uint8_t q_head;     /* the oldest packet: on the channel, or the next to arm */
  uint8_t q_count;
  u2ps_dwc2_hout_t out;
  u2ps_dwc2_hfb_t fb;
  u2ps_dwc2_hcd_diag_t diag;
  u2ps_hport_stats_t stats;
} h U2PS_FAST_BSS;

/* ---- Helpers ---------------------------------------------------------------- */

/* The handlers of rare events stay out of line (and out of
 * .text.u2ps_fast): the port's events, the control channel, the stops */
#define OUT_OF_LINE __attribute__((noinline))

/* The interrupts the port unmasks: these at init, and SOF while the data
 * channel is idle. Never CIDSCHG, which the STM32H7S3 sets falsely after
 * every reset (ES0596 2.21.2, p. 34). */
#define GINT_INIT (GINT_DISCINT | GINT_HCINT | GINT_HPRTINT | GINT_WKUPINT | GINT_OTGINT | GINT_MMIS)
_Static_assert(((GINT_INIT | GINT_SOF) & GINT_CIDSCHG) == 0u,
               "no interrupt mask the port writes may unmask CIDSCHG (ES0596 2.21.2)");

/* Every write of OTG_GINTMSK goes through this, which keeps the value
 * written (read-write, written only by the port: 73.14.8, p. 3337) */
U2PS_FAST_TEXT static void gintmsk_set(uint32_t v)
{
  h.gintmsk = v;
  OTG_GINTMSK = v;
}

/* PRIMASK around the read of 'now' and the channel enable that depends on
 * it, so no interrupt comes between them; a few dozen instructions */
U2PS_FAST_TEXT static inline uint32_t irq_save(void)
{
  uint32_t primask;
  __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
  return primask;
}

U2PS_FAST_TEXT static inline void irq_restore(uint32_t primask)
{
  __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
}

/* A bounded wait, counted on its timeout */
static bool wait(volatile uint32_t *reg, uint32_t mask, uint32_t value, uint32_t us)
{
  if (u2ps_dwc2_wait_us(reg, mask, value, us))
    return true;
  h.diag.timeouts++;
  return false;
}

/* The vector pended (NVIC_ISPR), so that the handler runs once the vector
 * is unmasked: an upcall owed by a call made from thread, which may not
 * make it itself (Armv7-M and Armv8-M Architecture Reference Manuals,
 * "NVIC") */
static void irq_pend(void)
{
  U2PS_DWC2_R32(0xE000E200u + 4u * (U2PS_DWC2_IRQ / 32u)) = 1u << (U2PS_DWC2_IRQ % 32u);
}

/* OTG_HPRT written with the bits of 'set' set and those of 'clear'
 * cleared: the rc_w1 bits as 0 unless named (u2ps_dwc2_hprt_write), and
 * PSUSP, of access type rs, as 0 unless named, so that a suspend the core
 * has ended since the read is not set again (p. 3358) */
static void hprt_set(uint32_t set, uint32_t clear)
{
  OTG_HPRT = u2ps_dwc2_hprt_write(OTG_HPRT, set, clear | (U2PS_DWC2_HPRT_PSUSP & ~set));
}

#if U2PS_TEST_INJECT
static uint8_t pass_kind U2PS_FAST_BSS; /* u2ps_dwc2_hpass_t of this pass: the lowest seen */
U2PS_FAST_TEXT static void pass_is(u2ps_dwc2_hpass_t k)
{
  if (k < pass_kind)
    pass_kind = (uint8_t)k;
}
#define PASS_IS(k) pass_is(k)
#else
#define PASS_IS(k) ((void)0)
#endif

/* The barrier before a channel is enabled: the CPU's writes to what its
 * DMA reads (a packet, the SETUP packet) complete first (DSB, Armv7-M and
 * Armv8-M Architecture Reference Manuals). A cache operation (F723) ends
 * with one already; without one (the U5, the data cache off, DTCM) this is
 * the only one. */
U2PS_FAST_TEXT static inline void dma_barrier(void)
{
  __asm volatile("dsb 0xF" ::: "memory");
}

/* A channel's interrupt: true at its halt, with OTG_HCINTx, read once and
 * cleared, in *hcint, together with the bits of an interrupt before it
 * (AHBERR, the other bit unmasked). Every decision waits for the halt,
 * which in DMA mode ends every channel transfer (pp. 3426-3433); nothing
 * is written to the channel here. */
U2PS_FAST_TEXT static bool ch_halted(uint32_t n, uint32_t *hcint)
{
  uint32_t v = OTG_HCINT(n);
  OTG_HCINT(n) = v;
  if ((v & U2PS_DWC2_HCINT_CHH) == 0u) {
    h.pending[n] |= v;
    h.diag.no_chh++;
    return false;
  }
  *hcint = v | h.pending[n];
  h.pending[n] = 0;
  return true;
}

/* ---- Stops ------------------------------------------------------------------ */

static u2ps_dwc2_err_t host_setup(void);

/* Channel n halted on a port that still sends tokens, with bounded waits:
 * the control channel disabled ("Halting a channel", p. 3404: CHDIS and
 * CHENA, then CHH); a stream channel, armed for the next microframe at
 * most, halts by itself (pp. 3431-3433) and is only awaited. The wait is
 * for CHENA, which the core clears (p. 3360), then for CHH: a halt between
 * the read and the write of the disable makes the write enable the channel
 * again, and its second halt is awaited too. False on a timeout. */
static bool ch_halt(uint32_t n)
{
  uint32_t v = OTG_HCCHAR(n);
  if ((v & HCCHAR_CHENA) == 0u)
    return true;
  if (n == CH_CTL)
    OTG_HCCHAR(n) = v | HCCHAR_CHDIS;
  return wait(&OTG_HCCHAR(n), HCCHAR_CHENA, 0u, STOP_US) &&
         wait(&OTG_HCINT(n), U2PS_DWC2_HCINT_CHH, U2PS_DWC2_HCINT_CHH, STOP_US);
}

/* Channel n flushed, without a wait: CHDIS written with CHENA 0, which
 * "automatically clears the CHENA bit" (p. 3404); a 0 written to CHENA (rs)
 * leaves it, so the write cannot enable a halted channel again. Whether a
 * CHH follows is not stated: one that does is the flush's. */
static void ch_flush(uint32_t n)
{
  uint32_t v = OTG_HCCHAR(n);
  if ((v & HCCHAR_CHENA) == 0u)
    return;
  OTG_HCCHAR(n) = (v & ~HCCHAR_CHENA) | HCCHAR_CHDIS;
  if (n == CH_CTL)
    h.ctl_flushed = true;
}

static void sof_set(bool on);

/* The stream ends without a report: its packets are the app's again */
static void stream_drop(void)
{
  sof_set(false);
  u2ps_dwc2_hout_close(&h.out);
  u2ps_dwc2_hfb_close(&h.fb);
  h.open = h.closing = h.closed_owed = false;
  h.q_head = h.q_count = 0;
  h.ep_fb = 0;
}

/* Host mode, whatever the ID pin says: FHMOD set, FDMOD cleared, then
 * the change, which takes at least 25 ms (73.14.4, p. 3324), awaited until
 * CMOD reads 1 (73.14.6). False if it does not. */
static bool force_host(void)
{
  OTG_GUSBCFG = (OTG_GUSBCFG & ~GUSBCFG_FDMOD) | GUSBCFG_FHMOD;
  u2ps_dwc2_delay_us(HOST_MODE_US);
  return wait(&OTG_GINTSTS, GINT_CMOD, GINT_CMOD, CMOD_US);
}

/* The core soft reset that port babble asks for (73.15.5, "Handling
 * babble conditions", p. 3426), inside the handler, with bounded waits:
 * the core reset of u2ps_hport_init() (u2ps_dwc2_core_reset), host mode
 * again, then the host set-up. CSRST ends every AHB and USB transaction;
 * it keeps FHMOD on the U5 (73.14.5, p. 3327) but not on the F723 (RM0431
 * 32.15.5 does not list it), and host registers written while the core is
 * in device mode are ignored (MMIS, 73.14.6, p. 3332), so FHMOD is set
 * again and CMOD awaited first (force_host): about 26 ms in the handler,
 * at most 66 ms with every wait at its bound, at a fault. The port is idle
 * after it, and the core detects an attached device again (PCDET). */
static void soft_reset(void)
{
  h.diag.soft_resets++;
  h.port = U2PS_DWC2_PORT_IDLE;
  if (!u2ps_dwc2_core_reset())
    h.diag.timeouts++;
  if (!force_host() || host_setup() != U2PS_DWC2_OK)
    h.errors++;
}

/* Every channel stopped and what was in flight dropped, without a report:
 * the control transfer, the stream (DISCINT, "the application is expected
 * to disable all enabled channels", p. 3405; u2ps_hport_disable; a port
 * the hardware disabled). With 'halt', for a port that still sends tokens,
 * each enabled channel is halted with a bounded wait (ch_halt), and one
 * that does not halt in time is flushed and counted in timeouts; without,
 * each is flushed at once (ch_flush): a disconnected or disabled port
 * sends no token (p. 3426), so no IN channel can write memory (inferred),
 * and the data channel's DMA only reads. No soft reset (port babble's
 * only). */
static void stop_all(bool halt)
{
  for (uint32_t n = 0; n < CHANNELS; n++) {
    if (!halt || !ch_halt(n))
      ch_flush(n);
    OTG_HCINT(n) = HCINT_ALL;
    h.pending[n] = 0;
  }
  u2ps_dwc2_ctl_drop(&h.ctl);
  h.ctl_owed = h.ctl_kick = h.ctl_wait = h.enable_owed = false;
  stream_drop();
}

/* ---- Root port ---------------------------------------------------------------- */

void u2ps_hport_reset(bool on)
{
  if (!h.up)
    return;
  /* "Host initialization" steps 5-8 (73.15.2, p. 3402): PRST set, timed by
   * the host stack, cleared; PENCHNG then gives the speed, unless it came
   * while PRST was set: that enable is reported from the handler now */
  if (on) {
    h.port = U2PS_DWC2_PORT_RESET;
    hprt_set(U2PS_DWC2_HPRT_PRST, 0);
  } else {
    bool now;
    h.port = u2ps_dwc2_hprt_reset_end(h.port, &now);
    hprt_set(0, U2PS_DWC2_HPRT_PRST);
    if (now) {
      h.enable_owed = true;
      irq_pend();
    }
  }
}

/* PENA written 1 disables the port: no SOFs (p. 3360). PRES and PRST
 * written 0 end a resume K or a reset; a suspend ends at the next reset,
 * which clears PSUSP (p. 3359). */
void u2ps_hport_disable(void)
{
  if (!h.up)
    return;
  stop_all(true);
  hprt_set(U2PS_DWC2_HPRT_PENA, U2PS_DWC2_HPRT_PRES | U2PS_DWC2_HPRT_PRST);
  h.port = U2PS_DWC2_PORT_IDLE;
}

/* OTG_HPRT.PTCTL (p. 3358); the port is disabled (u2ps_hport.h) */
void u2ps_hport_test_mode(uint8_t selector)
{
  uint32_t ptctl = u2ps_dwc2_hprt_ptctl(selector);
  if (!h.up || ptctl == 0u) {
    h.errors++;
    return;
  }
  h.port = U2PS_DWC2_PORT_TEST;
  hprt_set(ptctl, U2PS_DWC2_HPRT_PTCTL_MSK);
}

/* PSUSP: "The core only stops sending SOFs" (p. 3359). WKUPINT, unmasked
 * from the init on, is cleared first: only a wakeup during this suspend
 * counts. */
void u2ps_hport_suspend(void)
{
  if (!h.up || h.port != U2PS_DWC2_PORT_ENABLED) {
    h.errors++;
    return;
  }
  OTG_GINTSTS = GINT_WKUPINT;
  hprt_set(U2PS_DWC2_HPRT_PSUSP, 0);
  h.port = U2PS_DWC2_PORT_SUSPENDED;
}

/* PRES: the application sets it, times the resume and clears it; the core
 * sets it itself at a remote wakeup (73.7.1, p. 3302; p. 3359) */
bool u2ps_hport_resume(bool on)
{
  if (!h.up)
    return false;
  if (on) {
    if (h.port == U2PS_DWC2_PORT_SUSPENDED) {
      hprt_set(U2PS_DWC2_HPRT_PRES, 0);
      h.port = U2PS_DWC2_PORT_RESUMING;
    } else {
      h.errors++;
    }
    return false;
  }
  if ((OTG_HPRT & U2PS_DWC2_HPRT_PRES) != 0u)
    hprt_set(0, U2PS_DWC2_HPRT_PRES);
  bool out = u2ps_dwc2_hprt_resumed(OTG_HPRT);
  if (out && h.port == U2PS_DWC2_PORT_RESUMING)
    h.port = U2PS_DWC2_PORT_ENABLED;
  return out;
}

/* A disconnect (DISCINT, 73.7.1, p. 3301): the core has cleared PENA (p.
 * 3360), so the channels are flushed, not awaited */
OUT_OF_LINE static void disconnect(void)
{
  PASS_IS(U2PS_DWC2_HPASS_PORT);
  stop_all(false);
  /* A reset or a resume in progress ends with the device */
  hprt_set(0, U2PS_DWC2_HPRT_PRST | U2PS_DWC2_HPRT_PRES);
  if (h.port != U2PS_DWC2_PORT_TEST)
    h.port = U2PS_DWC2_PORT_IDLE;
  u2ps_host_on_disconnect();
}

/* A host port interrupt (HPRTINT): the event of OTG_HPRT
 * (u2ps_dwc2_hctl.h) */
OUT_OF_LINE static void port_int(void)
{
  PASS_IS(U2PS_DWC2_HPASS_PORT);
  uint32_t hprt = OTG_HPRT;
  u2ps_dwc2_hprt_event_t e;
  u2ps_dwc2_hprt_event(hprt, h.port, &e);
  OTG_HPRT = e.ack;
  h.diag.hprt = hprt;
  if ((hprt & U2PS_DWC2_HPRT_POCCHNG) != 0u)
    h.diag.overcurrent++;
  h.port = e.state;
  if (e.state == U2PS_DWC2_PORT_RESET_ENABLED) {
    /* Enabled while PRST is still set: kept for the reset's end */
    h.speed = e.speed;
    h.diag.speed = (uint8_t)e.speed;
    h.diag.enable_prst++;
  }
  if (e.connect)
    u2ps_host_on_connect();
  if (e.disabled) {
    /* Port babble, or an overcurrent: the port sends no more tokens; the
     * stream and the control transfer end without a report; after port
     * babble a soft reset (p. 3426) */
    stop_all(false);
    h.errors++;
    if (e.babble) {
      h.diag.babble++;
      soft_reset();
    }
    h.port = U2PS_DWC2_PORT_IDLE;
    u2ps_host_on_disabled();
  }
  if (e.enabled) {
    h.speed = e.speed;
    h.diag.speed = (uint8_t)e.speed;
    u2ps_host_on_enabled(e.speed);
  }
}

/* ---- The control transfer ------------------------------------------------------ */

/* A stage, or a part of the data stage, on the control channel (channel
 * initialization, 73.15.5, pp. 3403-3404): OTG_HCDMAx, OTG_HCTSIZx, then
 * OTG_HCCHARx with CHENA. The SETUP packet and OUT data are cleaned from
 * the data cache before the DMA reads them, IN data cleaned and
 * invalidated before the DMA writes it. */
static void ctl_arm(const u2ps_dwc2_ctl_step_t *s)
{
  /* An abort's CHDIS that met the channel's own halt enabled it again
   * (CHENA is rs, p. 3360): its halt comes next, and the arm waits for it
   * (ctl_chh) */
  if ((OTG_HCCHAR(CH_CTL) & HCCHAR_CHENA) != 0u) {
    h.ctl_next = *s;
    h.ctl_wait = true;
    h.diag.ctl_held++;
    return;
  }
  h.ctl_flushed = false;
  const uint8_t *addr;
  if (s->stage == U2PS_DWC2_STAGE_SETUP) {
    addr = h.setup;
    u2ps_dwc2_dcache_clean(h.setup, 8u);
  } else if (s->stage == U2PS_DWC2_STAGE_STATUS) {
    addr = zlp_buf;
  } else {
    uint8_t *p = h.data + s->offset;
    if (s->in)
      u2ps_dwc2_dcache_clean_invalidate(p, h.ctl.armed);
    else
      u2ps_dwc2_dcache_clean(p, h.ctl.armed);
    addr = p;
  }
  OTG_HCINT(CH_CTL) = HCINT_ALL;
  h.pending[CH_CTL] = 0;
  OTG_HCDMA(CH_CTL) = (uint32_t)(uintptr_t)addr;
  OTG_HCTSIZ(CH_CTL) = s->hctsiz;
  uint32_t hcchar = u2ps_dwc2_hcchar_ep0(h.ctl_addr, h.ctl.mps0, s->in, h.speed);
  dma_barrier();
  OTG_HCCHAR(CH_CTL) = hcchar | HCCHAR_CHENA;
}

/* What the control logic decided; an end reported now if in the handler,
 * else owed to it */
static void ctl_act(const u2ps_dwc2_ctl_step_t *s, bool handler)
{
  if (s->what == U2PS_DWC2_CTL_ARM) {
    ctl_arm(s);
  } else if (s->what == U2PS_DWC2_CTL_DONE) {
    if (!handler) {
      /* Only a refused data stage ends in a call: ERROR, 0 bytes */
      h.ctl_owed = true;
      irq_pend();
      return;
    }
    if (h.ctl.data_in && s->len > 0u && h.data != NULL)
      u2ps_dwc2_dcache_invalidate(h.data, s->len);
    u2ps_host_on_control((u2ps_hport_control_t)s->result, s->len);
  }
}

static void ctl_begin(uint8_t addr, uint8_t mps0, const uint8_t setup[8], void *data,
                      bool setup_only)
{
  if (!h.up || h.port != U2PS_DWC2_PORT_ENABLED) {
    /* Not while the port is not enabled, suspended or resuming: refused */
    h.errors++;
    h.ctl_owed = true;
    irq_pend();
    return;
  }
  u2ps_dwc2_ctl_step_t s;
  h.ctl_addr = addr;
  h.setup = setup;
  h.data = data;
  u2ps_dwc2_ctl_start(&h.ctl, setup, mps0, setup_only, &s);
  ctl_act(&s, false);
}

void u2ps_hport_control(uint8_t addr, uint8_t mps0, const uint8_t setup[8], void *data)
{
  ctl_begin(addr, mps0, setup, data, false);
}

void u2ps_hport_control_setup(uint8_t addr, uint8_t mps0, const uint8_t setup[8])
{
  ctl_begin(addr, mps0, setup, NULL, true);
}

void u2ps_hport_control_data(void *data)
{
  u2ps_dwc2_ctl_step_t s;
  h.data = data;
  u2ps_dwc2_ctl_data(&h.ctl, &s);
  ctl_act(&s, false);
}

/* CHDIS and CHENA on the enabled channel, written from one read; its CHH
 * ends the transfer (p. 3404). A halt between that read and the write
 * makes the write enable the channel again, and a second halt follows,
 * which ctl_arm() waits for. A channel found idle has its halt pending, or
 * lost it: the handler checks, and ends the transfer itself in the second
 * case, so that exactly one on_control follows. */
void u2ps_hport_control_abort(void)
{
  if (!u2ps_dwc2_ctl_abort(&h.ctl))
    return;
  uint32_t v = OTG_HCCHAR(CH_CTL);
  if ((v & HCCHAR_CHENA) != 0u) {
    OTG_HCCHAR(CH_CTL) = v | HCCHAR_CHDIS;
  } else {
    h.ctl_kick = true;
    irq_pend();
  }
}

/* The abort's check: still busy, idle, and no halt pending */
OUT_OF_LINE static void ctl_kick(void)
{
  h.ctl_kick = false;
  if (!h.ctl.busy || (OTG_HCCHAR(CH_CTL) & HCCHAR_CHENA) != 0u ||
      (OTG_HCINT(CH_CTL) & U2PS_DWC2_HCINT_CHH) != 0u)
    return;
  h.errors++;
  uint32_t hcint = U2PS_DWC2_HCINT_CHH | h.pending[CH_CTL];
  h.pending[CH_CTL] = 0;
  OTG_HCINT(CH_CTL) = HCINT_ALL;
  u2ps_dwc2_ctl_step_t s;
  u2ps_dwc2_ctl_chh(&h.ctl, hcint, OTG_HCTSIZ(CH_CTL), &s);
  ctl_act(&s, true);
}

OUT_OF_LINE static void ctl_chh(void)
{
  uint32_t hcint;
  if (!ch_halted(CH_CTL, &hcint))
    return;
  PASS_IS(U2PS_DWC2_HPASS_CONTROL);
  if (h.ctl_flushed) {
    /* The halt of a flush (ch_flush): nothing was on the channel since */
    h.ctl_flushed = false;
    return;
  }
  if (h.ctl_wait) {
    /* The second halt after an abort's disable met the channel's own:
     * the arm it held */
    h.ctl_wait = false;
    ctl_arm(&h.ctl_next);
    return;
  }
  /* The device's NAKs, which the core retried in DMA mode (p. 3427): the
   * bit is taken to be set although masked (73.14.34; inferred, not yet
   * checked on a board) */
  h.diag.ctl_naks += (hcint & U2PS_DWC2_HCINT_NAK) != 0u;
  h.diag.ctl_txerr += (hcint & U2PS_DWC2_HCINT_TXERR) != 0u;
  h.diag.ctl_bberr += (hcint & U2PS_DWC2_HCINT_BBERR) != 0u;
  h.diag.ctl_dterr += (hcint & U2PS_DWC2_HCINT_DTERR) != 0u;
  h.diag.ahberr += (hcint & U2PS_DWC2_HCINT_AHBERR) != 0u;
  u2ps_dwc2_ctl_step_t s;
  u2ps_dwc2_ctl_chh(&h.ctl, hcint, OTG_HCTSIZ(CH_CTL), &s);
  ctl_act(&s, true);
}

/* ---- The stream: isochronous OUT data channel ------------------------------------- */

/* The packet at the queue's head on the data channel: OTG_HCDMAx and
 * OTG_HCTSIZx, then the arm decision with FRNUM read immediately before
 * the OTG_HCCHARx write (u2ps_dwc2_hiso.h); isochronous OUT in DMA mode,
 * the core fetches the packet once the channel is enabled and sends it in
 * the next microframe of the ODDFRM parity (pp. 3431-3432) */
U2PS_FAST_TEXT static void out_arm(void)
{
  const packet_t *p = &h.q[h.q_head];
  OTG_HCDMA(CH_OUT) = (uint32_t)(uintptr_t)p->data;
  OTG_HCTSIZ(CH_OUT) = u2ps_dwc2_hctsiz_iso(p->len);
  dma_barrier();
  uint32_t primask = irq_save();
  bool odd = u2ps_dwc2_hout_arm(&h.out, u2ps_dwc2_hfnum());
  OTG_HCCHAR(CH_OUT) = h.out_char | HCCHAR_CHENA | (odd ? HCCHAR_ODDFRM : 0u);
  irq_restore(primask);
}

/* The next feedback poll, if it is due (u2ps_dwc2_hfb_arm(): from the
 * microframe right before it), into the feedback buffer not read last.
 * From the handler ('handler'), polls whose microframe has passed are
 * skipped and returned, the first in *skipped; from a call, which may not
 * report them, nothing is armed then: the handler skips them. */
U2PS_FAST_TEXT static uint16_t fb_arm(bool handler, u2ps_uframe_t *skipped)
{
  uint16_t n = 0;
  if (h.ep_fb == 0u || h.fb.state != U2PS_DWC2_HFB_IDLE ||
      u2ps_uframe_dist((u2ps_uframe_t)(u2ps_dwc2_hfnum() & U2PS_UFRAME_MASK), h.fb.poll) > 1)
    return 0; /* not due: nothing to arm or to skip (u2ps_dwc2_hfb_arm) */
  uint8_t i = (uint8_t)(h.fb_i ^ 1u);
  /* No dirty line of the buffer may be written back over what the DMA
   * writes (F723): cleaned and invalidated before the poll, invalidated
   * again before it is read (fb_chh) */
  u2ps_dwc2_dcache_clean_invalidate(fb_buf[i], U2PS_DWC2_HFB_MPS_MAX);
  OTG_HCDMA(CH_FB) = (uint32_t)(uintptr_t)fb_buf[i];
  OTG_HCTSIZ(CH_FB) = u2ps_dwc2_hctsiz_iso(h.fb_mps);
  dma_barrier();
  uint32_t primask = irq_save();
  uint32_t fn = u2ps_dwc2_hfnum();
  if (handler || u2ps_uframe_dist((u2ps_uframe_t)(fn & U2PS_UFRAME_MASK), h.fb.poll) > 0) {
    bool odd = false;
    n = u2ps_dwc2_hfb_arm(&h.fb, fn, &odd, skipped);
    if (h.fb.state == U2PS_DWC2_HFB_ARMED) {
      OTG_HCCHAR(CH_FB) = h.fb_char | HCCHAR_CHENA | (odd ? HCCHAR_ODDFRM : 0u);
      h.fb_i = i;
    }
  }
  irq_restore(primask);
  return n;
}

/* SOF unmasked (on) or masked, as the feedback's clock while the data
 * channel is idle: before the stream's first packet and while its queue
 * is dry. SOF is set "when an SOF (FS), or Keep-Alive (LS) is
 * transmitted" (73.14.6, p. 3331); at high speed it is taken to come with
 * every microframe's SOF (inferred; not yet measured on a board). Cleared before it is
 * unmasked: OTG_GINTSTS records it while masked (73.14.8, p. 3337). */
U2PS_FAST_TEXT static void sof_set(bool on)
{
  on = on && h.ep_fb != 0u;
  if (on && !h.sof_on) {
    OTG_GINTSTS = GINT_SOF;
    gintmsk_set(h.gintmsk | GINT_SOF);
  } else if (!on && h.sof_on) {
    gintmsk_set(h.gintmsk & ~GINT_SOF);
  }
  h.sof_on = on;
}

/* on_fb_missed(LATE) for each of n polls skipped, one period apart */
U2PS_FAST_TEXT static void fb_skipped(uint16_t n, u2ps_uframe_t first)
{
  for (uint16_t k = 0; k < n; k++)
    u2ps_host_on_fb_missed(u2ps_uframe_add(first, (uint16_t)(k * h.fb.period)), U2PS_MISS_LATE);
}

#if U2PS_TEST_INJECT
/* The injection build: every inj.every-th re-arm after a halt that would
 * be in time (made in the packet's own microframe P) waits until P + 1
 * starts, then inj.wait cycles more, and only then decides and arms. The
 * cycles come from the DWT cycle counter (u2ps_dwc2.h). */
static struct {
  volatile uint32_t every; /* 0: off */
  uint32_t wait, limit;    /* cycles: the hold after P + 1 starts; two microframes */
  uint32_t rearms;         /* re-arms since the last injection */
  u2ps_dwc2_hinject_t st;
} inj U2PS_FAST_BSS;

U2PS_FAST_TEXT static uint32_t frnum14(void)
{
  return u2ps_dwc2_hfnum() & U2PS_UFRAME_MASK;
}

/* Before the arm decision after the packet of microframe p: returns true,
 * with the cycle count at which P + 1 was seen in *t1, if this re-arm is
 * held */
U2PS_FAST_TEXT static bool inject_hold(u2ps_uframe_t p, uint32_t *t1)
{
  if (inj.every == 0u || ++inj.rearms < inj.every)
    return false;
  uint32_t fn = frnum14();
  if (fn != p) {
    inj.st.skipped++; /* late already: the next one is taken */
    return false;
  }
  inj.rearms = 0;
  uint32_t t0 = u2ps_dwc2_cycles();
  while (frnum14() == fn && u2ps_dwc2_cycles() - t0 < inj.limit) {
  }
  if (frnum14() == fn)
    inj.st.timeouts++;
  *t1 = u2ps_dwc2_cycles();
  while (u2ps_dwc2_cycles() - *t1 < inj.wait) {
  }
  return true;
}

U2PS_FAST_TEXT static void inject_done(uint32_t t1)
{
  uint32_t dt = u2ps_dwc2_cycles() - t1;
  if (inj.st.injected == 0u || dt < inj.st.d_min)
    inj.st.d_min = dt;
  if (dt > inj.st.d_max)
    inj.st.d_max = dt;
  inj.st.injected++;
}

void u2ps_dwc2_hinject(uint32_t every, uint32_t us)
{
  u2ps_hport_irq_mask(true);
  inj.every = 0;
  inj.wait = us * U2PS_DWC2_CYCLES_PER_US;
  inj.limit = 250u * U2PS_DWC2_CYCLES_PER_US;
  inj.rearms = 0;
  inj.st = (u2ps_dwc2_hinject_t){.every = every, .us = us};
  inj.every = every;
  u2ps_hport_irq_mask(false);
}

void u2ps_dwc2_hinject_get(u2ps_dwc2_hinject_t *st)
{
  u2ps_hport_irq_mask(true);
  *st = inj.st;
  u2ps_hport_irq_mask(false);
}

static u2ps_dwc2_hpasses_t passes U2PS_FAST_BSS;

void u2ps_dwc2_hpasses_get(u2ps_dwc2_hpasses_t *p, bool clear)
{
  u2ps_hport_irq_mask(true);
  *p = passes;
  if (clear)
    passes = (u2ps_dwc2_hpasses_t){0};
  u2ps_hport_irq_mask(false);
}
#endif

static void close_check(void);

/* The data channel's halt (u2ps_dwc2_hiso.h, the handler's sequence):
 * report, re-arm, the next feedback poll, then the upcalls in bus order */
U2PS_FAST_TEXT static void out_chh(void)
{
  uint32_t hcint;
  if (!ch_halted(CH_OUT, &hcint))
    return;
  if (!h.open) {
    if (h.closing)
      close_check();
    return;
  }
  PASS_IS(U2PS_DWC2_HPASS_DATA);
  u2ps_dwc2_hout_report_t r;
  if (!u2ps_dwc2_hout_chh(&h.out, hcint, u2ps_dwc2_hfnum(), false, &r)) {
    h.diag.rereads++;
    (void)u2ps_dwc2_hout_chh(&h.out, hcint, u2ps_dwc2_hfnum(), true, &r);
  }
  h.diag.ahberr += (hcint & U2PS_DWC2_HCINT_AHBERR) != 0u;
  if (r.sent) {
    h.q_head = (uint8_t)((h.q_head + 1u) % U2PS_HOST_QUEUE_MAX);
    h.q_count--;
  }
  if (r.again || (r.sent && h.q_count > 0u)) {
#if U2PS_TEST_INJECT
    uint32_t t1 = 0;
    bool held = r.sent && inject_hold(r.uframe, &t1);
    out_arm();
    if (held)
      inject_done(t1);
#else
    out_arm();
#endif
  } else if (r.sent) {
    u2ps_dwc2_hout_dry(&h.out);
  }
  u2ps_uframe_t first = 0;
  uint16_t skipped = fb_arm(true, &first);
  if (r.sent) {
    if (r.no_buffer != 0u)
      u2ps_host_on_missed(r.no_buffer, U2PS_MISS_NO_BUFFER);
    if (r.late != 0u)
      u2ps_host_on_missed(r.late, U2PS_MISS_LATE);
    if (r.corrupted)
      u2ps_host_on_missed(1u, U2PS_MISS_CORRUPTED);
    u2ps_host_on_sent(r.uframe);
  }
  fb_skipped(skipped, first);
  /* Dry still after the upcalls, which may have queued: the feedback's
   * clock is SOF until the next packet */
  if (h.open && h.out.state == U2PS_DWC2_HOUT_DRY)
    sof_set(true);
}

/* The queue, in order; a packet arms the channel when it is idle (the
 * stream's first packet, or the queue ran dry). Its lines are cleaned
 * first (u2ps_hport.h). */
U2PS_FAST_TEXT void u2ps_hport_queue(const uint8_t *data, uint16_t len)
{
  if (!h.open || h.q_count >= U2PS_HOST_QUEUE_MAX) {
    h.errors++; /* no stream, or more than U2PS_HOST_QUEUE_MAX */
    return;
  }
  u2ps_dwc2_dcache_clean(data, len);
  uint8_t i = (uint8_t)((h.q_head + h.q_count) % U2PS_HOST_QUEUE_MAX);
  h.q[i].data = data;
  h.q[i].len = len;
  h.q_count++;
  if (h.out.state == U2PS_DWC2_HOUT_OPEN || h.out.state == U2PS_DWC2_HOUT_DRY) {
    out_arm();
    sof_set(false);
  }
  u2ps_uframe_t first;
  (void)fb_arm(false, &first);
}

/* ---- The stream: isochronous IN feedback channel ---------------------------------- */

U2PS_FAST_TEXT static void fb_chh(void)
{
  uint32_t hcint;
  if (!ch_halted(CH_FB, &hcint))
    return;
  if (!h.open) {
    if (h.closing)
      close_check();
    return;
  }
  PASS_IS(U2PS_DWC2_HPASS_FB);
  u2ps_dwc2_hfb_result_t res;
  u2ps_uframe_t uframe = 0;
  if (!u2ps_dwc2_hfb_chh(&h.fb, hcint, u2ps_dwc2_hfnum(), false, &res, &uframe)) {
    h.diag.rereads++;
    (void)u2ps_dwc2_hfb_chh(&h.fb, hcint, u2ps_dwc2_hfnum(), true, &res, &uframe);
  }
  h.diag.ahberr += (hcint & U2PS_DWC2_HCINT_AHBERR) != 0u;
  uint8_t *data = fb_buf[h.fb_i];
  uint16_t len = 0;
  if (res == U2PS_DWC2_HFB_DATA)
    len = u2ps_dwc2_hctsiz_in_len(h.fb_mps, OTG_HCTSIZ(CH_FB), h.fb_mps);
  /* The next poll first: at period 1 it is due now */
  u2ps_uframe_t first = 0;
  uint16_t skipped = fb_arm(true, &first);
  if (res == U2PS_DWC2_HFB_DATA) {
    u2ps_dwc2_dcache_invalidate(data, U2PS_DWC2_HFB_MPS_MAX);
    u2ps_host_on_fb(data, len, uframe);
  } else if (res == U2PS_DWC2_HFB_NO_PACKET) {
    u2ps_host_on_fb_missed(uframe, U2PS_MISS_NO_PACKET);
  } else if (res == U2PS_DWC2_HFB_LATE) {
    u2ps_host_on_fb_missed(uframe, U2PS_MISS_LATE);
  }
  fb_skipped(skipped, first);
}

/* ---- The stream: open and close -------------------------------------------------- */

/* Channel initialization (73.15.5, pp. 3403-3404): the characteristics of
 * both channels; the data channel is armed by the first packet, the
 * feedback channel in the microframe before its first poll, now + 1 */
bool u2ps_hport_stream_open(const u2ps_hstream_t *s)
{
  if (!h.up || h.open || h.closing || h.port != U2PS_DWC2_PORT_ENABLED ||
      h.speed != U2PS_SPEED_HIGH)
    return false;
  if (!u2ps_dwc2_hstream_fits(s->ep_out, s->mps, s->ep_fb, s->fb_interval, s->fb_mps))
    return false;
  h.out_char = u2ps_dwc2_hcchar_iso(s->addr, s->ep_out, s->mps, false);
  h.q_head = h.q_count = 0;
  OTG_HCINT(CH_OUT) = HCINT_ALL;
  u2ps_dwc2_hout_open(&h.out);
  h.open = true;
  if (s->ep_fb != 0u) {
    h.ep_fb = s->ep_fb;
    h.fb_mps = s->fb_mps;
    h.fb_char = u2ps_dwc2_hcchar_iso(s->addr, s->ep_fb, s->fb_mps, true);
    OTG_HCINT(CH_FB) = HCINT_ALL;
    (void)u2ps_dwc2_hfb_open(&h.fb, s->fb_interval, u2ps_dwc2_hfnum());
    u2ps_uframe_t first;
    (void)fb_arm(false, &first);
    sof_set(true); /* until the first packet */
  }
  return true;
}

/* The close's end: both channels idle */
OUT_OF_LINE static void close_check(void)
{
  if (!h.closing || ((OTG_HCCHAR(CH_OUT) | OTG_HCCHAR(CH_FB)) & HCCHAR_CHENA) != 0u)
    return;
  OTG_HCINT(CH_OUT) = HCINT_ALL;
  OTG_HCINT(CH_FB) = HCINT_ALL;
  stream_drop();
  u2ps_host_on_closed();
}

/* Asynchronous: the channels are not disabled; each, armed for the next
 * microframe at most, halts by itself (pp. 3431-3433) and is not armed
 * again, and on_closed comes at the last halt (close_check); with both
 * idle, from the handler at once (irq_pend) */
void u2ps_hport_stream_close(void)
{
  if (!h.open)
    return;
  h.open = false;
  h.closing = true;
  sof_set(false);
  if (((OTG_HCCHAR(CH_OUT) | OTG_HCCHAR(CH_FB)) & HCCHAR_CHENA) == 0u) {
    h.closed_owed = true;
    irq_pend();
  }
}

/* ---- The handler ------------------------------------------------------------------ */

U2PS_FAST_TEXT static void irq_pass(void)
{
  uint32_t raw = OTG_GINTSTS;
  uint32_t gint = raw & h.gintmsk;
  uint32_t haint = 0;

  /* 1. A disconnect: every channel stopped, so their halts are not served */
  if ((gint & GINT_DISCINT) != 0u) {
    OTG_GINTSTS = GINT_DISCINT;
    disconnect();
  } else if ((gint & GINT_HCINT) != 0u) {
    haint = OTG_HAINT & CH_ALL;
  }
  /* 2-4. The channels */
  if ((haint & (1u << CH_OUT)) != 0u)
    out_chh();
  if ((haint & (1u << CH_FB)) != 0u)
    fb_chh();
  if ((haint & (1u << CH_CTL)) != 0u)
    ctl_chh();
  /* 5. The root port; OTG_HPRT's change bits clear HPRTINT */
  if ((gint & GINT_HPRTINT) != 0u)
    port_int();
  if ((gint & GINT_WKUPINT) != 0u) {
    OTG_GINTSTS = GINT_WKUPINT;
    /* A remote wakeup: PSUSP cleared, PRES set and the resume K driven by
     * the core (p. 3302) */
    if (h.port == U2PS_DWC2_PORT_SUSPENDED) {
      PASS_IS(U2PS_DWC2_HPASS_PORT);
      h.port = U2PS_DWC2_PORT_RESUMING;
      h.diag.wakeups++;
      u2ps_host_on_wakeup();
    }
  }
  /* 6. The feedback's clock, what calls left, and the rest */
  if ((gint & GINT_SOF) != 0u) {
    OTG_GINTSTS = GINT_SOF;
    if (h.open) {
      u2ps_uframe_t first = 0;
      fb_skipped(fb_arm(true, &first), first);
    } else {
      sof_set(false);
    }
  }
  if (h.closed_owed) {
    h.closed_owed = false;
    close_check();
  }
  if (h.ctl_owed) {
    h.ctl_owed = false;
    u2ps_host_on_control(U2PS_HPORT_CONTROL_ERROR, 0);
  }
  if (h.enable_owed) {
    h.enable_owed = false;
    if (h.port == U2PS_DWC2_PORT_ENABLED) {
      PASS_IS(U2PS_DWC2_HPASS_PORT);
      u2ps_host_on_enabled(h.speed);
    }
  }
  if (h.ctl_kick)
    ctl_kick();
  if ((raw & GINT_IPXFR) != 0u) {
    OTG_GINTSTS = GINT_IPXFR;
    h.diag.ipxfr++;
  }
  if ((gint & GINT_OTGINT) != 0u)
    OTG_GOTGINT = OTG_GOTGINT;
  if ((gint & GINT_MMIS) != 0u)
    OTG_GINTSTS = GINT_MMIS;
}

U2PS_FAST_TEXT void u2ps_dwc2_hcd_irq(void)
{
#if U2PS_TEST_INJECT
  /* The pass profile of the injection build */
  uint32_t t0 = u2ps_dwc2_cycles();
  pass_kind = U2PS_DWC2_HPASS_OTHER;
  irq_pass();
  uint32_t dt = u2ps_dwc2_cycles() - t0;
  passes.n[pass_kind]++;
  passes.sum[pass_kind] += dt;
  if (dt > passes.max[pass_kind])
    passes.max[pass_kind] = dt;
#else
  irq_pass();
#endif
}

/* ---- Start-up --------------------------------------------------------------------- */

/* The core and host set-up after a core reset ("Core initialization" and
 * "Host initialization", 73.15.1-2, p. 3402); OTG_HCFG and OTG_HFIR are
 * left at their reset values: FSLSS (r) 0, the core enumerates at high
 * speed; FSLSPCS 00, no value for the internal UTMI PHY being documented
 * (73.14.24, p. 3354; inferred, not yet checked with a full-speed device);
 * FRIVL, which the core then computes from the PHY clock itself
 * (73.14.25, p. 3355) */
static u2ps_dwc2_err_t host_setup(void)
{
  /* DMA mode with INCR4 bursts (as the device port); the interrupt line
   * masked until the end */
  OTG_GAHBCFG = (OTG_GAHBCFG & ~(GAHBCFG_HBSTLEN_MSK | GAHBCFG_GINTMSK)) | GAHBCFG_DMAEN |
                GAHBCFG_HBSTLEN_INCR4;
  /* The embedded host state machine, not the OTG A-device's (EHEN,
   * 73.14.1, p. 3320): the port is a host only, with no HNP or A-device
   * timeout (ADTOCHG, p. 3321). VBUS is the app's, and the core does not
   * sense it ("In Host mode, the VBUS sensing pin does not need to be
   * connected", 73.7.1, p. 3301): the A-session and VBUS valid by override
   * (73.14.1, pp. 3320-3321; choice, inferred: the manual leaves open what
   * the core does with them in host mode). The ID pin is not used: on
   * the U5 and the H7S3 (U2PS_DWC2_GCCFG_OVR) the host pull-downs forced
   * (73.7, p. 3300; 73.14.18, p. 3347; RM0477 62.14.18, pp. 3103-3104,
   * VBDEN 0 since PM8, OTG_HS_VBUS, is the NUCLEO-H7S3L8's TCPP03 FLGn). */
  OTG_GOTGCTL |= GOTGCTL_EHEN | GOTGCTL_AVALOEN | GOTGCTL_AVALOVAL | GOTGCTL_VBVALOEN |
                 GOTGCTL_VBVALOVAL;
#if U2PS_DWC2_GCCFG_OVR == 1
  OTG_GCCFG = (OTG_GCCFG & ~GCCFG_VBDEN) | GCCFG_FORCEHOSTPD;
#elif U2PS_DWC2_GCCFG_OVR != 0
#error "u2ps_dwc2_hcd.c: U2PS_DWC2_GCCFG_OVR must be 0 or 1 (u2ps_dwc2_chip.h)"
#endif
  /* Steps 12-14: FIFO RAM for the largest stream (u2ps_dwc2_hiso.h), then
   * every FIFO flushed */
  u2ps_dwc2_hfifo_t f;
  bool ok = u2ps_dwc2_hfifo_plan(DATA_MPS_MAX, U2PS_DWC2_HFB_MPS_MAX, &f);
  OTG_GRXFSIZ = f.rx;
  OTG_HNPTXFSIZ = (uint32_t)f.nptx << 16 | f.nptx_start;
  OTG_HPTXFSIZ = (uint32_t)f.ptx << 16 | f.ptx_start;
  ok = u2ps_dwc2_flush_tx(0x10u) && ok;
  ok = u2ps_dwc2_flush_rx() && ok;
  /* Channel interrupts (73.15.5, channel initialization steps 3-4) */
  for (uint32_t n = 0; n < CHANNELS; n++) {
    OTG_HCINTMSK(n) = HCINT_RUN;
    OTG_HCINT(n) = HCINT_ALL;
    h.pending[n] = 0;
  }
  OTG_HAINTMSK = CH_ALL;
  /* Cleared before they are unmasked (73.14.6, p. 3328); RXFLVL, NPTXFE
   * and PTXFE stay masked in DMA mode; IPXFR is read, not unmasked */
  OTG_GINTSTS = 0xFFFFFFFFu;
  gintmsk_set(GINT_INIT);
  /* Step 3: the port power bit, which drives no VBUS (p. 3358); a device
   * attached then gives PCDET */
  hprt_set(U2PS_DWC2_HPRT_PPWR, 0);
  OTG_GAHBCFG |= GAHBCFG_GINTMSK;
  return ok ? U2PS_DWC2_OK : U2PS_DWC2_ERR_FIFO;
}

static u2ps_host_err_t init_fail(u2ps_dwc2_err_t e)
{
  h.init_err = e;
  return u2ps_dwc2_host_err(e);
}

/* The SETUP packet, the control buffer and port memory against the rule
 * of u2ps_dwc2_dma_ok(), with the PWR clock on first (U5) so that the
 * SRAMs' power-down bits can be read; the port's start enables it anyway */
bool u2ps_hport_dma_check(const void *mem, uint32_t len)
{
  u2ps_dwc2_pwr_clock();
  return u2ps_dwc2_dma_ok(mem, len) && u2ps_dwc2_dma_ok(fb_buf, sizeof fb_buf) &&
         u2ps_dwc2_dma_ok(zlp_buf, sizeof zlp_buf);
}

/* The same rule as ranges (u2ps_dwc2_dma_ranges), for the host stack's
 * check of each queued packet in a build with the contract checks */
_Static_assert(U2PS_DWC2_RANGES_MAX <= U2PS_HPORT_RANGES_MAX,
               "the port's ranges fit the host port interface's");

uint32_t u2ps_hport_dma_ranges(u2ps_hport_range_t *ranges)
{
  u2ps_dwc2_range_t r[U2PS_DWC2_RANGES_MAX];
  unsigned n = u2ps_dwc2_dma_ranges(r);
  for (unsigned i = 0; i < n; i++)
    ranges[i] = (u2ps_hport_range_t){r[i].start, r[i].end};
  return n;
}

/* The first call starts the PHY, a later one only resets the core: the
 * vectors are masked or not yet enabled, and CSRST ends whatever the
 * channels were doing (p. 3327) */
u2ps_host_err_t u2ps_hport_init(void)
{
  bool phy_up = h.phy_up;
  memset(&h, 0, sizeof h);
  h.phy_up = phy_up;
  if (!h.phy_up) {
    u2ps_dwc2_err_t e = u2ps_dwc2_phy_start();
    if (e != U2PS_DWC2_OK)
      return init_fail(e);
    h.phy_up = true;
  }
  if (!u2ps_dwc2_core_reset())
    return init_fail(U2PS_DWC2_ERR_RESET);
  /* Port memory, now that the reset has stopped the channels' DMA (p.
   * 3327): zeroed, so its section needs no zeroing at startup, and
   * cleaned, so that no dirty line is written back over what the DMA
   * writes later (F723) */
  memset(fb_buf, 0, sizeof fb_buf);
  memset(zlp_buf, 0, sizeof zlp_buf);
  u2ps_dwc2_dcache_clean(fb_buf, sizeof fb_buf);
  u2ps_dwc2_dcache_clean(zlp_buf, sizeof zlp_buf);
  /* Host only, whatever the ID pin says */
  if (!force_host())
    return init_fail(U2PS_DWC2_ERR_MODE);
  u2ps_dwc2_err_t e = host_setup();
  if (e != U2PS_DWC2_OK)
    return init_fail(e);
  h.up = true;
  return U2PS_HOST_OK;
}

u2ps_dwc2_err_t u2ps_dwc2_hcd_init_error(void)
{
  return h.init_err;
}

/* The vector is enabled only once the port has started: a mask call after
 * a failed start leaves it off */
void u2ps_hport_irq_mask(bool masked)
{
  if (masked || h.up)
    u2ps_dwc2_irq_mask(masked);
}

const u2ps_hport_stats_t *u2ps_hport_stats(void)
{
  h.stats.sent = h.out.sent;
  h.stats.late_completions = h.out.late_completions;
  h.stats.late = h.out.late_total;
  h.stats.no_buffer = h.out.no_buffer_total;
  h.stats.corrupted = h.out.corrupted;
  h.stats.fb_polls = h.fb.polls;
  h.stats.fb_missed = h.fb.missed;
  h.stats.control_retries = h.ctl.retries;
  h.stats.errors = h.out.errors + h.fb.errors + h.ctl.faults + h.errors;
  return &h.stats;
}

void u2ps_dwc2_hcd_diag(u2ps_dwc2_hcd_diag_t *diag)
{
  u2ps_hport_irq_mask(true);
  *diag = h.diag;
  u2ps_hport_irq_mask(false);
}
