/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_dcd.c: the DWC2 device port, ARM builds only: every
 * u2ps_dport_* call of u2ps_dport.h and the OTG_HS interrupt handler,
 * in buffer DMA mode. Its decisions for the stream are in u2ps_dwc2_iso.c
 * and its FIFO sizes in u2ps_dwc2_fifo.c; both build on the PC too.
 *
 * Manuals: RM0456 Rev 7 chapter 73 (cited by section and page) and RM0431
 * chapter 32 (cited by section). "Device programming model" is RM0456
 * 73.15.6, p. 3436-3455, and RM0431 32.16.6.
 *
 * Order of events in one pass of the handler:
 *  1. the stream's data endpoint (XFRC: complete, re-arm, then upcalls;
 *     EPDISD of a close), first, because its re-arm has the tightest
 *     deadline; OTG_DOEPINTx is read before OTG_DSTS;
 *  2. USBRST (the port stops the stream itself, then on_reset), ENUMDNE
 *     (on_speed), WKUPINT (on_resume);
 *  3. the feedback endpoint (XFRC, then EPDISD; then EOPF, then IISOIXFR);
 *  4. EP0 IN (data and status completions), then EP0 OUT (completions, the
 *     host's status OUT during a data IN stage, then SETUP), so a
 *     completion pending with a newer SETUP comes before it, in bus order;
 *  5. GONAKEFF of a close, ISOODRP, USBSUSP (on_suspend), OTGINT and MMIS.
 *
 * EP0 in DMA mode: EP0 OUT is kept enabled into the port's 64-byte EP0
 * area, so a SETUP can always land (in DMA mode the endpoint must be
 * enabled to receive one, RM0456 p. 3436, step 3). Its NAK decides
 * whether OUT data is taken: arming an OUT stage (ep0_out, the status OUT)
 * only clears it. The core writes SETUPs into the area at word
 * 4 - 2 * STUPCNT (Figure 942, p. 3441), and OUT data at its start; the
 * port copies OUT data into the stack's buffer, at most 64 bytes per
 * packet. IN stages are sent by DMA straight from the stack's buffer, one
 * packet per arm.
 *
 * The packet path, the OUT pass with everything it calls, the feedback
 * endpoint's passes and the release, is in section .text.u2ps_fast, and
 * the port's state in .bss.u2ps_fast (U2PS_FAST_TEXT, U2PS_FAST_BSS in
 * u2ps_types.h). The port is the only writer of OTG_GINTMSK and
 * OTG_DAINTMSK, so it keeps their values in its state and the handler
 * reads them from there instead of from the core.
 */
#include <stddef.h>
#include <string.h>

#include "u2ps_dport.h"
#include "u2ps_dport_cfg.h"
#include "u2ps_dwc2.h"
#include "u2ps_dwc2_dcd.h"
#include "u2ps_dwc2_fifo.h"
#include "u2ps_dwc2_iso.h"
#include "u2ps_dwc2_regs.h"

#if !defined(U2PS_ISO_OUT_MPS_MAX) || !defined(U2PS_ISO_OUT_COUNT_MAX)
#error "u2ps_dwc2_dcd.c: U2PS_ISO_OUT_MPS_MAX or U2PS_ISO_OUT_COUNT_MAX is missing: link u2ps_device, whose public compile definitions set them"
#endif
#ifndef U2PS_DWC2_OUT_FOLLOW
#define U2PS_DWC2_OUT_FOLLOW 1
#endif
#ifndef U2PS_DWC2_FORCE_FS
#define U2PS_DWC2_FORCE_FS 0
#endif
#ifndef U2PS_TEST_INJECT
#define U2PS_TEST_INJECT 0
#endif

/* A chip whose errata sheet lists OTG_HS faults of IN endpoints the port
 * has no workaround for (u2ps_dwc2_chip.h): ES0596 2.21.1 and 2.21.3 on the
 * STM32H7S3. The configure step stops first (CMakeLists.txt); this stops a
 * build that bypasses it. */
#if U2PS_DWC2_ERRATUM_ISO_IN != 0 || U2PS_DWC2_ERRATUM_ZLP != 0
#error "u2ps_dwc2_dcd.c: the DWC2 device port has no workaround for this chip's errata ES0596 2.21.1 (an isochronous IN endpoint disabled during a packet fetch) and 2.21.3 (a packet instead of a zero-length packet): build the host stack alone (-DU2PS_HOST=ON -DU2PS_DEVICE=OFF)"
#endif

/* Class memory holds what the port programs: each buffer's stride is at
 * least XFRSIZ = ceil4(mps) and a whole number of ALIGN units */
#define STRIDE_OK(mps) \
  (U2PS_DPORT_STRIDE(mps) >= (((mps) + 3u) & ~3u) && U2PS_DPORT_STRIDE(mps) % U2PS_DPORT_ALIGN == 0u)
_Static_assert(STRIDE_OK(1u) && STRIDE_OK(4u) && STRIDE_OK(5u) && STRIDE_OK(776u) &&
                 STRIDE_OK(777u) && STRIDE_OK(1023u) && STRIDE_OK(1024u),
               "U2PS_DPORT_STRIDE below the transfer size or off ALIGN");
_Static_assert(U2PS_DPORT_FB_INTERVAL_MIN == U2PS_DWC2_FB_INTERVAL_MIN &&
                 U2PS_DPORT_FB_INTERVAL_MAX == U2PS_DWC2_FB_INTERVAL_MAX,
               "feedback interval limits differ from the logic's");
_Static_assert(U2PS_EP0_MPS == 64u, "the port programs EP0 for 64-byte packets");
_Static_assert(U2PS_DPORT_ISO_OUT_RESERVE == U2PS_DWC2_OUT_RESERVE &&
                 U2PS_DPORT_ISO_OUT_MIN == U2PS_DPORT_ISO_OUT_RESERVE + 1u,
               "the reserve is the logic's, and the least count lets the stack hold one");
_Static_assert(U2PS_ISO_OUT_COUNT_MAX >= U2PS_DPORT_ISO_OUT_MIN,
               "U2PS_ISO_OUT_COUNT_MAX must be at least U2PS_DPORT_ISO_OUT_MIN (2 on DWC2), or "
               "no stream opens: set -DU2PS_ISO_OUT_COUNT_MAX=2 or more (default 16)");

/* The feedback endpoint's Tx FIFO number, and the largest feedback value:
 * 4 bytes, 16.16 at high speed (USB 2.0 5.12.4.2) */
#define FB_FIFO 1u
#define FB_MAX  4u

/* Turnaround time for an AHB clock of 30 MHz or more (RM0456 73.14.4,
 * Table 771, p. 3325) */
#define TRDT_AHB_30MHZ 9u

/* Port memory, in DMA memory: the EP0 area (three SETUP slots, or one OUT
 * packet) and the feedback word, each on its own 32-byte lines. EP0 OUT
 * cannot be disabled (RM0456 73.14.54, p. 3382), and its DMA address moves
 * on with each back-to-back SETUP past the three programmed (B2BSTUP); a
 * guard of 8 more slots after the area takes those, and a SETUP that finds
 * the address past the guard counts in ep0_overrun. */
#define EP0_AREA  64u
#define EP0_GUARD 64u
static _Alignas(32) uint8_t ep0_area[EP0_AREA + EP0_GUARD] __attribute__((section(U2PS_DMA_SECTION)));
static _Alignas(32) uint32_t fb_word[8] __attribute__((section(U2PS_DMA_SECTION)));

typedef enum { FB_OFF, FB_ARMED, FB_RECOVER, FB_CLOSE } fb_state_t;

static struct {
  bool up;         /* u2ps_dport_init() succeeded */
  u2ps_dwc2_err_t init_err;
  u2ps_dwc2_open_err_t open_err; /* why the last stream open was refused */
  u2ps_dwc2_dcd_mode_t mode;
  bool suspended;
  bool b2b;        /* B2BSTUP seen since the last SETUP was taken */
  uint32_t errors; /* port errors outside the stream logic */
  /* EP0 IN: a data stage or the status IN */
  const uint8_t *in_buf;
  uint16_t in_len, in_off, in_pkt;
  bool in_zlp, in_data, in_status;
  /* EP0 OUT: a data stage or the status OUT */
  uint8_t *out_buf;
  uint16_t out_len, out_off;
  bool out_data, out_status;
  /* The stream */
  bool open, closing, close_out, close_fb;
  uint8_t ep, ep_fb, fb_size;
  uint8_t fb_state;
  bool iiso_on, eopf_on; /* IISOIXFR and EOPF unmasked */
  bool eopf_fb, eopf_out; /* the feedback's or the data endpoint's parity follows at EOPF */
  uint16_t mps, xfrsiz, stride;
  uint8_t *mem;
  u2ps_uframe_t fb_recover_at;
  uint32_t close_t0; /* DWT cycles at the close's SGONAK */
  u2ps_dwc2_out_t out;
  u2ps_dwc2_fb_t fb;
  u2ps_dwc2_fb_start_t fb_start;
  u2ps_dwc2_dcd_diag_t diag;
  u2ps_dport_stats_t stats;
  /* OTG_GINTMSK and OTG_DAINTMSK as last written (gintmsk_set, daintmsk_set) */
  uint32_t gintmsk, daintmsk;
} d U2PS_FAST_BSS;

/* ---- Helpers ---------------------------------------------------------------- */

/* The handlers of rare events that the handler's pass calls stay out of
 * line (and out of .text.u2ps_fast): the bus events, EP0, the close and
 * the feedback's recovery */
#define OUT_OF_LINE __attribute__((noinline))

static void vec_mask(bool masked);

/* The interrupts the port unmasks: these at init, and IISOIXFR, EOPF and
 * GONAKEFF while its stream needs them. Never CIDSCHG, which the STM32H7S3
 * sets falsely after every reset (ES0596 2.21.2, p. 34). */
#define GINT_INIT                                                                         \
  (GINT_USBRST | GINT_ENUMDNE | GINT_USBSUSP | GINT_WKUPINT | GINT_OEPINT | GINT_IEPINT | \
   GINT_ISOODRP | GINT_OTGINT | GINT_MMIS)
#define GINT_STREAM (GINT_IISOIXFR | GINT_EOPF | GINT_GONAKEFF)
_Static_assert(((GINT_INIT | GINT_STREAM) & GINT_CIDSCHG) == 0u,
               "no interrupt mask the port writes may unmask CIDSCHG (ES0596 2.21.2)");

/* Every write of OTG_GINTMSK and OTG_DAINTMSK goes through these, which
 * keep the value written (both are read-write, written only by the port:
 * RM0456 73.14.8, p. 3336; 73.14.44, p. 3373) */
U2PS_FAST_TEXT static void gintmsk_set(uint32_t v)
{
  d.gintmsk = v;
  OTG_GINTMSK = v;
}

static void daintmsk_set(uint32_t v)
{
  d.daintmsk = v;
  OTG_DAINTMSK = v;
}

/* PRIMASK around the read of 'now' and the endpoint enable that depends on
 * it, so no interrupt comes between them; a few dozen instructions */
static inline uint32_t irq_save(void)
{
  uint32_t primask;
  __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
  return primask;
}

static inline void irq_restore(uint32_t primask)
{
  __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
}

/* A bounded wait in the handler or at a reset, counted on its timeout */
static bool wait(volatile uint32_t *reg, uint32_t mask, uint32_t value, uint32_t us)
{
  if (u2ps_dwc2_wait_us(reg, mask, value, us))
    return true;
  d.diag.timeouts++;
  return false;
}

/* The bound of each wait while EP0 IN is stopped: the packet in flight, at
 * most 64 bytes, takes about 1 us on the bus (inferred bound) */
#define EP0_STOP_US 100u
/* The bound of each wait of the stream's stop at a bus reset */
#define RESET_STOP_US 1000u

#if U2PS_TEST_INJECT
static uint8_t pass_kind U2PS_FAST_BSS; /* u2ps_dwc2_pass_t of this pass: the lowest seen */
U2PS_FAST_TEXT static void pass_is(u2ps_dwc2_pass_t k)
{
  if (k < pass_kind)
    pass_kind = (uint8_t)k;
}
#define PASS_IS(k) pass_is(k)
#else
#define PASS_IS(k) ((void)0)
#endif

U2PS_FAST_TEXT static uint32_t parity_bits(bool odd)
{
  return odd ? EPCTL_SODDFRM : EPCTL_SEVNFRM;
}

/* The characteristics written with every OTG_DOEPCTLx and OTG_DIEPCTLx
 * write of the stream (endpoint activation, RM0456 p. 3437) */
U2PS_FAST_TEXT static uint32_t out_ctl(void)
{
  return EPCTL_USBAEP | EPCTL_EPTYP_ISO | EPCTL_MPSIZ(d.mps);
}

U2PS_FAST_TEXT static uint32_t fb_ctl(void)
{
  return EPCTL_USBAEP | EPCTL_EPTYP_ISO | EPCTL_TXFNUM(FB_FIFO) | EPCTL_MPSIZ(d.fb_size);
}

/* IISOIXFR unmasked or masked. OTG_GINTSTS records it while masked (RM0456
 * 73.14.8, p. 3336), so what came meanwhile is cleared first. */
U2PS_FAST_TEXT static void iiso_set(bool on)
{
  on = on || !d.mode.iiso_window;
  if (on && !d.iiso_on) {
    OTG_GINTSTS = GINT_IISOIXFR;
    gintmsk_set(d.gintmsk | GINT_IISOIXFR);
  } else if (!on && d.iiso_on) {
    gintmsk_set(d.gintmsk & ~GINT_IISOIXFR);
  }
  d.iiso_on = on;
}

/* EOPF likewise, while a parity follows the bus */
U2PS_FAST_TEXT static void eopf_set(bool on)
{
  if (on && !d.eopf_on) {
    OTG_GINTSTS = GINT_EOPF;
    gintmsk_set(d.gintmsk | GINT_EOPF);
  } else if (!on && d.eopf_on) {
    gintmsk_set(d.gintmsk & ~GINT_EOPF);
  }
  d.eopf_on = on;
}

/* ---- EP0 -------------------------------------------------------------------- */

/* A SETUP the port has not yet delivered: received into the area
 * (STPKTRX, RM0456 73.14.55, p. 3384) or complete (STUP). The port takes
 * one only in the handler, and clears both bits then. */
static bool setup_held(void)
{
  return (OTG_DOEPINT(0) & (DOEPINT_STUP | DOEPINT_STPKTRX)) != 0u;
}

static void lost_ack_watch(bool on)
{
  /* The host's OUT to EP0 during a control read's data stage: EP0 OUT
   * NAKs it, and DOEPINT0.NAK shows it (RM0456 73.14.55, p. 3384). The
   * mask is common to all OUT endpoints, so it is set only while a data
   * IN stage runs. */
  if (on) {
    OTG_DOEPINT(0) = DOEPINT_NAK;
    OTG_DOEPMSK |= DOEPINT_NAK;
  } else {
    OTG_DOEPMSK &= ~DOEPINT_NAK;
  }
}

/* EP0 OUT enabled into the area, with NAK cleared (cnak) or set. The core
 * disables it after each OUT packet and after each SETUP stage (RM0456
 * p. 3384, STPKTRX). Not while a SETUP is held: re-pointing the area then
 * would lose it; the SETUP's handling enables it. */
static void ep0_out_enable(bool cnak)
{
  if (setup_held())
    return;
  if ((OTG_DOEPCTL(0) & EPCTL_EPENA) == 0u) {
    u2ps_dwc2_dcache_clean_invalidate(ep0_area, sizeof ep0_area);
    OTG_DOEPTSIZ(0) = TSIZ_STUPCNT(3u) | TSIZ_PKTCNT(1u) | U2PS_EP0_MPS;
    OTG_DOEPDMA(0) = (uint32_t)(uintptr_t)ep0_area;
    OTG_DOEPCTL(0) = EPCTL_EPENA | (cnak ? EPCTL_CNAK : EPCTL_SNAK);
  } else if (cnak) {
    OTG_DOEPCTL(0) = EPCTL_CNAK;
  }
}

/* After an EP0 stage was armed: if a SETUP arrived meanwhile, the stage
 * belongs to the request it replaced, and is taken back at once (SNAK, and
 * EPDIS while still enabled; RM0456 73.14.48, p. 3377). This narrows the
 * window in which an old stage meets the new request's tokens but cannot
 * close it (u2ps_dport.h, EP0 rules). */
static void ep0_in_undo(void)
{
  if (!setup_held())
    return;
  d.diag.ep0_undone++;
  d.in_data = d.in_status = false;
  lost_ack_watch(false);
  OTG_DIEPCTL(0) = EPCTL_SNAK | EPCTL0_MPSIZ_64;
  if ((OTG_DIEPCTL(0) & EPCTL_EPENA) != 0u)
    OTG_DIEPCTL(0) = EPCTL_EPDIS | EPCTL_SNAK | EPCTL0_MPSIZ_64;
}

static void ep0_out_undo(void)
{
  if (!setup_held())
    return;
  d.diag.ep0_undone++;
  d.out_data = d.out_status = false;
  OTG_DOEPCTL(0) = EPCTL_SNAK;
}

static void ep0_in_packet(void)
{
  uint16_t n = (uint16_t)(d.in_len - d.in_off);
  if (n > U2PS_EP0_MPS)
    n = U2PS_EP0_MPS;
  d.in_pkt = n;
  u2ps_dwc2_dcache_clean(d.in_buf + d.in_off, n);
  OTG_DIEPTSIZ(0) = TSIZ_PKTCNT(1u) | n;
  OTG_DIEPDMA(0) = (uint32_t)(uintptr_t)(d.in_buf + d.in_off);
  OTG_DIEPCTL(0) = EPCTL_EPENA | EPCTL_CNAK | EPCTL0_MPSIZ_64;
  ep0_in_undo();
}

/* EP0 IN disabled with its Tx FIFO flushed ("IN endpoint disable",
 * RM0456 p. 3450): SNAK, INEPNE, EPDIS, EPDISD, flush */
static void ep0_in_stop(void)
{
  if ((OTG_DIEPCTL(0) & EPCTL_EPENA) != 0u) {
    OTG_DIEPCTL(0) = EPCTL_SNAK | EPCTL0_MPSIZ_64;
    (void)wait(&OTG_DIEPINT(0), DIEPINT_INEPNE, DIEPINT_INEPNE, EP0_STOP_US);
    OTG_DIEPCTL(0) = EPCTL_EPDIS | EPCTL_SNAK | EPCTL0_MPSIZ_64;
    (void)wait(&OTG_DIEPINT(0), DIEPINT_EPDISD, DIEPINT_EPDISD, EP0_STOP_US);
    if (!u2ps_dwc2_flush_tx(0u))
      d.diag.timeouts++;
  }
  OTG_DIEPINT(0) = 0xFFFFFFFFu;
}

/* A SETUP aborts every armed stage (u2ps_dport.h, EP0 rules); the core
 * has set NAK on EP0 IN and OUT already (RM0456 p. 3440, internal data
 * flow 1) */
static void ep0_abort(void)
{
  d.in_data = d.in_status = d.out_data = d.out_status = false;
  lost_ack_watch(false);
  ep0_in_stop();
}

void u2ps_dport_ep0_in(const void *data, uint16_t len, bool zlp)
{
  if (setup_held()) {
    d.diag.ep0_dropped++;
    return;
  }
  d.in_buf = data;
  d.in_len = len;
  d.in_off = 0;
  d.in_zlp = zlp;
  d.in_data = true;
  d.in_status = false;
  lost_ack_watch(true);
  ep0_in_packet();
}

void u2ps_dport_ep0_out(void *data, uint16_t len)
{
  if (setup_held()) {
    d.diag.ep0_dropped++;
    return;
  }
  d.out_buf = data;
  d.out_len = len;
  d.out_off = 0;
  d.out_data = true;
  d.out_status = false;
  ep0_out_enable(true);
  ep0_out_undo();
}

void u2ps_dport_ep0_status(bool in)
{
  if (setup_held()) {
    d.diag.ep0_dropped++;
    return;
  }
  if (in) {
    d.in_status = true;
    OTG_DIEPTSIZ(0) = TSIZ_PKTCNT(1u);
    OTG_DIEPCTL(0) = EPCTL_EPENA | EPCTL_CNAK | EPCTL0_MPSIZ_64;
    ep0_in_undo();
  } else {
    d.out_status = true;
    ep0_out_enable(true);
    ep0_out_undo();
  }
}

void u2ps_dport_ep0_stall(void)
{
  if (setup_held()) {
    d.diag.ep0_dropped++;
    return;
  }
  d.in_data = d.in_status = d.out_data = d.out_status = false;
  lost_ack_watch(false);
  /* Both directions, until the core clears STALL at the next SETUP
   * (RM0456 73.14.54, p. 3382; RM0431 32.15.50) */
  OTG_DIEPCTL(0) = EPCTL_STALL | EPCTL0_MPSIZ_64;
  ep0_out_enable(false);
  OTG_DOEPCTL(0) = EPCTL_STALL;
}

void u2ps_dport_set_address(uint8_t addr)
{
  /* DCFG.DAD now, then the status IN; the core answers it at the old
   * address ("Endpoint initialization on SetAddress command", RM0456
   * p. 3437) */
  OTG_DCFG = (OTG_DCFG & ~DCFG_DAD_MSK) | DCFG_DAD(addr & 0x7Fu);
}

void u2ps_dport_test_mode(uint8_t selector)
{
  /* DCTL.TCTL takes the selectors of USB 2.0 Table 9-7 (RM0456 73.14.39,
   * p. 3368) */
  if (selector >= 1u && selector <= 4u)
    OTG_DCTL = (OTG_DCTL & ~DCTL_TCTL_MSK) | DCTL_TCTL(selector);
}

#if U2PS_TEST_INJECT
static uint32_t lost_ack_every, lost_ack_count;

void u2ps_dwc2_inject_lost_ack(uint32_t every)
{
  vec_mask(true);
  lost_ack_every = every;
  lost_ack_count = 0;
  vec_mask(false);
}

/* The final XFRC of a control read is not taken: the stage waits for an
 * ACK the host has already given, and its status OUT ends it */
static bool lost_ack_injected(void)
{
  if (lost_ack_every == 0u || ++lost_ack_count < lost_ack_every)
    return false;
  lost_ack_count = 0;
  return true;
}
#endif

OUT_OF_LINE static void ep0_in_int(void)
{
  uint32_t i = OTG_DIEPINT(0);
  OTG_DIEPINT(0) = i & (DIEPINT_XFRC | DIEPINT_TOC | DIEPINT_AHBERR | DIEPINT_EPDISD);
  if ((i & DIEPINT_AHBERR) != 0u)
    d.diag.ahberr++;
  if ((i & DIEPINT_XFRC) == 0u)
    return;
  if (d.in_data) {
    d.in_off = (uint16_t)(d.in_off + d.in_pkt);
    if (d.in_off < d.in_len) {
      ep0_in_packet();
    } else if (d.in_zlp) {
      /* A whole number of packets shorter than wLength ends with a
       * zero-length packet (USB 2.0 8.5.3.2) */
      d.in_zlp = false;
      ep0_in_packet();
    } else {
#if U2PS_TEST_INJECT
      if (lost_ack_injected())
        return;
#endif
      d.in_data = false;
      lost_ack_watch(false);
      u2ps_dev_on_ep0_data(d.in_len);
    }
  } else if (d.in_status) {
    d.in_status = false;
    u2ps_dev_on_ep0_status();
  }
}

OUT_OF_LINE static void ep0_out_int(void)
{
  uint32_t i = OTG_DOEPINT(0);
  uint32_t mask = OTG_DOEPMSK;

  /* STSPHSRX is cleared too, and at once (it is unmasked for that alone):
   * left set after a control write's status stage, it held back the next
   * SETUP's STUP on the U5, measured in buffer DMA mode. The core wrote the
   * SETUP to memory (XFRC, STPKTRX) but never ended its setup stage, so
   * EP0 stalled until the next bus reset. RM0456 73.14.55 (p. 3385)
   * describes the bit without this effect. Every other bit but those of
   * the SETUP, which its handling below clears, is cleared alike. */
  OTG_DOEPINT(0) = i & ~(DOEPINT_STUP | DOEPINT_STPKTRX | DOEPINT_B2BSTUP);
  if ((i & DOEPINT_AHBERR) != 0u)
    d.diag.ahberr++;
  if ((i & DOEPINT_XFRC) != 0u) {
    uint32_t left = OTG_DOEPTSIZ(0) & TSIZ0_XFRSIZ_MSK;
    uint16_t n = left <= U2PS_EP0_MPS ? (uint16_t)(U2PS_EP0_MPS - left) : 0u;
    u2ps_dwc2_dcache_invalidate(ep0_area, sizeof ep0_area);
    if ((i & DOEPINT_STPKTRX) != 0u) {
      /* The 8 bytes of a SETUP, never an OUT stage's data, also while one
       * is armed: each SETUP gives XFRC with STPKTRX before its STUP
       * (measured on the U5), and the STUP aborts the stage. For a
       * device-to-host SETUP, whether an IN token met an empty EP0 FIFO
       * before this point (ITTXFE, RM0456 73.14.49, p. 3379): then the
       * check for a held SETUP had a window. */
      uintptr_t at = (uintptr_t)OTG_DOEPDMA(0);
      if (at >= (uintptr_t)ep0_area + 8u && at <= (uintptr_t)ep0_area + sizeof ep0_area &&
          (ep0_area[at - (uintptr_t)ep0_area - 8u] & 0x80u) != 0u) {
        d.diag.in_setups++;
        if ((OTG_DIEPINT(0) & DIEPINT_ITTXFE) != 0u)
          d.diag.in_setup_ittxfe++;
      }
      OTG_DIEPINT(0) = DIEPINT_ITTXFE;
    } else if (d.out_data) {
      uint16_t room = (uint16_t)(d.out_len - d.out_off);
      uint16_t k = n < room ? n : room;
      memcpy(d.out_buf + d.out_off, ep0_area, k);
      d.out_off = (uint16_t)(d.out_off + k);
      if (n < U2PS_EP0_MPS || d.out_off >= d.out_len) {
        d.out_data = false;
        ep0_out_enable(false);
        u2ps_dev_on_ep0_data(d.out_off);
      } else {
        ep0_out_enable(true);
      }
    } else if (d.out_status) {
      d.out_status = false;
      ep0_out_enable(false);
      u2ps_dev_on_ep0_status();
    } else {
      d.errors++; /* OUT data with no stage armed: NAK was set */
      ep0_out_enable(false);
    }
  }
  /* An IN completion that came after this pass read OTG_DAINT goes first:
   * then the host's OUT is the status stage, not a lost ACK */
  if ((i & mask & DOEPINT_NAK) != 0u && d.in_data && (OTG_DIEPINT(0) & DIEPINT_XFRC) != 0u)
    ep0_in_int();
  if ((i & mask & DOEPINT_NAK) != 0u && d.in_data) {
    /* Lost ACK (USB 2.0 8.5.3.3): the host has moved on to the status
     * stage, so it has the data. The stage ends here; the status OUT gets
     * NAK until the stack arms it inside on_ep0_data. */
    d.diag.lost_ack++;
    d.in_data = false;
    lost_ack_watch(false);
    PASS_IS(U2PS_DWC2_PASS_EP0_STOP);
    ep0_in_stop();
    u2ps_dev_on_ep0_data(d.in_len);
  }
  if ((i & DOEPINT_B2BSTUP) != 0u) {
    OTG_DOEPINT(0) = DOEPINT_B2BSTUP;
    d.b2b = true;
    d.diag.b2bstup++;
  }
  if ((i & DOEPINT_STUP) != 0u) {
    /* The last SETUP sits at word 4 - 2 * STUPCNT of the area, STUPCNT
     * being what is left of the 3 programmed (Figure 942, p. 3441) */
    uint32_t rem = (OTG_DOEPTSIZ(0) >> TSIZ_STUPCNT_POS) & 0x3u;
    uint8_t setup[8];
    bool ok = !d.b2b && rem <= 2u;
    if ((uintptr_t)OTG_DOEPDMA(0) > (uintptr_t)ep0_area + sizeof ep0_area)
      d.diag.ep0_overrun++;
    PASS_IS(U2PS_DWC2_PASS_SETUP);
    if ((OTG_DIEPCTL(0) & EPCTL_EPENA) != 0u)
      PASS_IS(U2PS_DWC2_PASS_EP0_STOP);
    u2ps_dwc2_dcache_invalidate(ep0_area, sizeof ep0_area);
    if (ok)
      memcpy(setup, ep0_area + 8u * (2u - rem), sizeof setup);
    OTG_DOEPINT(0) = DOEPINT_STUP | DOEPINT_STPKTRX | DOEPINT_B2BSTUP;
    d.b2b = false;
    ep0_abort();
    ep0_out_enable(false);
    /* More than three back-to-back SETUPs: the slots cannot be told
     * apart; the host repeats the request after its timeout (inferred) */
    if (ok)
      u2ps_dev_on_setup(setup);
    else
      d.errors++;
  }
}

/* ---- The stream: isochronous OUT ------------------------------------------- */

/* The buffer to arm next: address and transfer size; the endpoint enable
 * follows in out_enable() */
U2PS_FAST_TEXT static void out_buffer(uint16_t b)
{
  uint8_t *buf = d.mem + (uint32_t)b * d.stride;
  u2ps_dwc2_dcache_invalidate(buf, d.stride);
  OTG_DOEPDMA(d.ep) = (uint32_t)(uintptr_t)buf;
  OTG_DOEPTSIZ(d.ep) = TSIZ_PKTCNT(1u) | d.xfrsiz;
}

/* The arm decision with 'now' read immediately before the OTG_DOEPCTLx
 * write; returns the LATE microframes to report after the packet, and
 * 'now' in *fn */
U2PS_FAST_TEXT static uint16_t out_enable(uint32_t *fn)
{
  u2ps_dwc2_bnext_t a;
  uint32_t primask = irq_save();
  uint32_t now = u2ps_dwc2_fnsof();
  if (u2ps_dwc2_out_arm(&d.out, now, &a))
    OTG_DOEPCTL(d.ep) = out_ctl() | EPCTL_EPENA | EPCTL_CNAK | parity_bits(a.odd);
  irq_restore(primask);
  *fn = now;
  return a.late;
}

#if U2PS_TEST_INJECT
/* The injection build: every inj.every-th re-arm after a completion that
 * would be in time (made in the packet's own microframe P) waits until
 * P + 1 starts, then inj.wait cycles more, and only then decides and arms.
 * The cycles come from the DWT cycle counter (DWT_CYCCNT,
 * u2ps_dwc2_regs.h). */

static struct {
  volatile uint32_t every; /* 0: off */
  uint32_t wait, limit;    /* cycles: the hold after P + 1 starts; two microframes */
  uint32_t rearms;         /* re-arms since the last injection */
  u2ps_dwc2_inject_t st;
} inj U2PS_FAST_BSS;

/* Before the arm decision of a completed packet of microframe p: returns
 * true, with the cycle count at which P + 1 was seen in *t1, if this
 * re-arm is held */
U2PS_FAST_TEXT static bool inject_hold(u2ps_uframe_t p, uint32_t *t1)
{
  if (inj.every == 0u || ++inj.rearms < inj.every)
    return false;
  uint32_t fn = u2ps_dwc2_fnsof() & U2PS_UFRAME_MASK;
  if (fn != p) {
    inj.st.skipped++; /* late already: the next one is taken */
    return false;
  }
  inj.rearms = 0;
  uint32_t t0 = DWT_CYCCNT;
  while ((u2ps_dwc2_fnsof() & U2PS_UFRAME_MASK) == fn && DWT_CYCCNT - t0 < inj.limit) {
  }
  if ((u2ps_dwc2_fnsof() & U2PS_UFRAME_MASK) == fn)
    inj.st.timeouts++;
  *t1 = DWT_CYCCNT;
  while (DWT_CYCCNT - *t1 < inj.wait) {
  }
  return true;
}

/* After the held re-arm: the time from the start of P + 1 to the end of
 * the arm */
U2PS_FAST_TEXT static void inject_done(uint32_t t1)
{
  uint32_t dt = DWT_CYCCNT - t1;
  if (inj.st.injected == 0u || dt < inj.st.d_min)
    inj.st.d_min = dt;
  if (dt > inj.st.d_max)
    inj.st.d_max = dt;
  inj.st.injected++;
}

void u2ps_dwc2_inject(uint32_t every, uint32_t us)
{
  vec_mask(true);
  inj.every = 0;
  inj.wait = us * U2PS_DWC2_CYCLES_PER_US;
  inj.limit = 250u * U2PS_DWC2_CYCLES_PER_US;
  inj.rearms = 0;
  inj.st = (u2ps_dwc2_inject_t){.every = every, .us = us};
  inj.every = every;
  vec_mask(false);
}

void u2ps_dwc2_inject_get(u2ps_dwc2_inject_t *st)
{
  vec_mask(true);
  *st = inj.st;
  vec_mask(false);
}

static u2ps_dwc2_passes_t passes U2PS_FAST_BSS;

void u2ps_dwc2_passes_get(u2ps_dwc2_passes_t *p, bool clear)
{
  vec_mask(true);
  *p = passes;
  if (clear)
    passes = (u2ps_dwc2_passes_t){0};
  vec_mask(false);
}
#endif

/* n microframes of a hole for 'why', in on_missed calls of at most 65,535
 * each */
U2PS_FAST_TEXT __attribute__((noinline)) static void out_missed(uint32_t n, u2ps_miss_t why)
{
  for (; n > 0xFFFFu; n -= 0xFFFFu)
    u2ps_dev_on_missed(0xFFFFu, why);
  if (n != 0u)
    u2ps_dev_on_missed((uint16_t)n, why);
}

U2PS_FAST_TEXT static void out_xfrc(void)
{
  uint32_t tsiz = OTG_DOEPTSIZ(d.ep);
  u2ps_dwc2_out_packet_t r;

  PASS_IS(U2PS_DWC2_PASS_OUT);
  /* Valid only with RXDPID DATA0 and one packet received (RM0456 p. 3446,
   * application step 6) */
  bool valid = ((tsiz >> TSIZ_RXDPID_POS) & 0x3u) == 0u && (tsiz & TSIZ_PKTCNT_MSK) == 0u;
  if (!valid) {
    if (d.out.state == U2PS_DWC2_OUT_ARMED)
      d.diag.pid_drops++;
    r.buf = U2PS_DWC2_NO_BUF;
    r.late = r.no_buffer = r.no_packet = 0;
    r.arm_buf = u2ps_dwc2_out_drop(&d.out, u2ps_dwc2_fnsof());
  } else if (!u2ps_dwc2_out_complete(&d.out, u2ps_dwc2_fnsof(), false, &r)) {
    (void)u2ps_dwc2_out_complete(&d.out, u2ps_dwc2_fnsof(), true, &r);
  }
  if (r.arm_buf != U2PS_DWC2_NO_BUF)
    out_buffer(r.arm_buf);
#if U2PS_TEST_INJECT
  uint32_t t1 = 0;
  bool held = r.buf != U2PS_DWC2_NO_BUF && r.arm_buf != U2PS_DWC2_NO_BUF &&
              inject_hold(r.uframe, &t1);
#endif
  uint32_t fn;
  uint16_t late = out_enable(&fn);
#if U2PS_TEST_INJECT
  if (held)
    inject_done(t1);
#endif
  /* The feedback endpoint's IISOIXFR, unmasked in the microframes around
   * an expected poll; the data endpoint's completions are the clock */
  if (d.fb_state == FB_ARMED && !d.iiso_on && u2ps_dwc2_fb_watch(&d.fb, fn))
    iiso_set(true);
  if (r.buf == U2PS_DWC2_NO_BUF) {
    /* A reserve packet: no upcall, and its data unread (no invalidate);
     * data flowed for the feedback's rule */
    if (valid && r.arm_buf != U2PS_DWC2_NO_BUF && d.ep_fb != 0u)
      u2ps_dwc2_fb_data(&d.fb, r.uframe);
    return;
  }
  /* The feedback counts its recoveries while data flows */
  if (d.ep_fb != 0u)
    u2ps_dwc2_fb_data(&d.fb, r.uframe);
  /* Upcalls only after the re-arm, in bus order */
  uint8_t *data = d.mem + (uint32_t)r.buf * d.stride;
  uint16_t len = u2ps_dwc2_out_len(&d.out, d.xfrsiz, tsiz & TSIZ_XFRSIZ_MSK);
  u2ps_dwc2_dcache_invalidate(data, d.stride);
  if (r.late != 0u)
    out_missed(r.late, U2PS_MISS_LATE);
  if (r.no_buffer != 0u)
    out_missed(r.no_buffer, U2PS_MISS_NO_BUFFER);
  if (r.no_packet != 0u)
    out_missed(r.no_packet, U2PS_MISS_NO_PACKET);
  u2ps_dev_on_packet(data, len, r.uframe);
  if (late != 0u)
    u2ps_dev_on_missed(late, U2PS_MISS_LATE);
}

/* Bookkeeping only: a buffer is always armed, and a freed one is armed at
 * the next completion, so no register is written */
U2PS_FAST_TEXT void u2ps_dport_release(uint16_t n)
{
  if (!d.open && !d.closing) {
    d.errors++; /* no stream */
    return;
  }
  u2ps_dwc2_out_release(&d.out, n);
}

/* ---- The stream: feedback IN ------------------------------------------------ */

/* MCNT = PKTCNT = 1, the value's bytes from the feedback word (RM0456
 * p. 3453); the DMA fetches it when the endpoint is enabled (inferred), so
 * a new value goes out at the next arm. 'recovery' makes this the
 * recovery's arm, which comes after the endpoint was disabled. Any other
 * arm finds the endpoint disabled by its XFRC, unless that XFRC raced a
 * recovery: then nothing is written to the enabled endpoint (RM0456
 * 73.14.48, p. 3377: EPENA set by the application, cleared by the core). */
U2PS_FAST_TEXT static void fb_arm(bool recovery)
{
  if (!recovery && (OTG_DIEPCTL(d.ep_fb) & EPCTL_EPENA) != 0u) {
    d.diag.fb_arm_busy++;
    return;
  }
  u2ps_dwc2_dcache_clean(fb_word, sizeof fb_word);
  OTG_DIEPDMA(d.ep_fb) = (uint32_t)(uintptr_t)fb_word;
  OTG_DIEPTSIZ(d.ep_fb) = TSIZ_MCNT(1u) | TSIZ_PKTCNT(1u) | d.fb_size;
  uint32_t primask = irq_save();
  uint32_t fn = u2ps_dwc2_fnsof();
  if (recovery)
    u2ps_dwc2_fb_rearm(&d.fb, fn);
  OTG_DIEPCTL(d.ep_fb) = fb_ctl() | EPCTL_EPENA | EPCTL_CNAK | parity_bits(d.fb.odd);
  irq_restore(primask);
  if (recovery)
    iiso_set(u2ps_dwc2_fb_watch(&d.fb, fn));
}

/* The recovery's second step, at EPDISD: flush, then a new arm
 * ("Incomplete isochronous IN data transfers", RM0456 p. 3455) */
OUT_OF_LINE static void fb_recover_end(void)
{
  PASS_IS(U2PS_DWC2_PASS_FB_FLUSH);
  if (!u2ps_dwc2_flush_tx(FB_FIFO))
    d.diag.timeouts++;
  OTG_DIEPINT(d.ep_fb) = 0xFFFFFFFFu;
  if (d.closing) {
    d.fb_state = FB_OFF; /* nothing is armed during a close */
    return;
  }
  d.fb_state = FB_ARMED;
  fb_arm(true);
}

U2PS_FAST_TEXT static void fb_iisoixfr(void)
{
  d.diag.iisoixfr++;
  PASS_IS(U2PS_DWC2_PASS_IISO);
  if (d.fb_state == FB_RECOVER) {
    /* An XFRC that raced the disable leaves the endpoint idle; if no
     * EPDISD has come two periods on, end the recovery here (inferred) */
    uint16_t since = u2ps_uframe_sub((u2ps_uframe_t)u2ps_dwc2_fnsof(), d.fb_recover_at);
    if (since > 2u * d.fb.period &&
        (OTG_DIEPCTL(d.ep_fb) & (EPCTL_EPENA | EPCTL_EPDIS)) == 0u) {
      d.diag.fb_forced++;
      fb_recover_end();
    }
    return;
  }
  if (d.fb_state != FB_ARMED)
    return;
  uint32_t fn = u2ps_dwc2_fnsof();
  if (d.fb.expect_known && !d.fb.recovering)
    d.diag.iiso_hist[u2ps_uframe_sub((u2ps_uframe_t)(fn & U2PS_UFRAME_MASK), d.fb.expect) & 7u]++;
  u2ps_dwc2_fb_action_t act = u2ps_dwc2_fb_iisoixfr(&d.fb, fn);
  if (act == U2PS_DWC2_FB_SWITCH && !d.fb_start.polled)
    d.fb_start.switches++;
  if (act == U2PS_DWC2_FB_NONE) {
    /* Masked until the next expected poll, unless no poll is known */
    iiso_set(u2ps_dwc2_fb_watch(&d.fb, fn));
    return;
  }
  /* It stays unmasked during the recovery, for the fallback above */
  d.fb_state = FB_RECOVER;
  d.fb_recover_at = (u2ps_uframe_t)u2ps_dwc2_fnsof();
  if ((OTG_DIEPCTL(d.ep_fb) & EPCTL_EPENA) != 0u)
    OTG_DIEPCTL(d.ep_fb) = fb_ctl() | EPCTL_EPDIS | EPCTL_SNAK;
  else
    fb_recover_end();
}

static void close_check(void);

/* The start record at the first poll after an open */
U2PS_FAST_TEXT static void fb_first_poll(void)
{
  if (d.fb_start.polled)
    return;
  u2ps_uframe_t q = u2ps_uframe_sub(d.fb.expect, d.fb.period);
  d.fb_start.polled = true;
  d.fb_start.first_q = q;
  d.fb_start.first_odd = d.fb.armed_odd;
  d.fb_start.first_after = u2ps_uframe_sub(q, d.fb_start.open_uframe);
}

U2PS_FAST_TEXT static void fb_int(void)
{
  uint32_t i = OTG_DIEPINT(d.ep_fb);
  OTG_DIEPINT(d.ep_fb) = i;
  if ((i & DIEPINT_AHBERR) != 0u)
    d.diag.ahberr++;
  if ((i & DIEPINT_XFRC) != 0u) {
    if (d.fb_state == FB_ARMED) {
      /* The poll came: the follow is over, and IISOIXFR is not needed
       * until the microframe before the next one */
      bool rearm = u2ps_dwc2_fb_xfrc(&d.fb, u2ps_dwc2_fnsof());
      fb_first_poll();
      d.eopf_fb = false;
      eopf_set(d.eopf_out);
      iiso_set(false);
      if (rearm && !d.closing)
        fb_arm(false);
    } else if (d.fb_state == FB_RECOVER) {
      (void)u2ps_dwc2_fb_xfrc(&d.fb, u2ps_dwc2_fnsof()); /* confirms; no re-arm */
      fb_first_poll();
      d.eopf_fb = false;
      eopf_set(d.eopf_out);
    }
  }
  if ((i & DIEPINT_EPDISD) != 0u) {
    if (d.fb_state == FB_RECOVER) {
      fb_recover_end();
    } else if (d.fb_state == FB_CLOSE) {
      if (!u2ps_dwc2_flush_tx(FB_FIFO))
        d.diag.timeouts++;
      d.close_fb = false;
      close_check();
    }
  }
}

/* EOPF: while a parity follows the bus, the parity of the next
 * microframe goes to the enabled endpoint: the feedback endpoint's until
 * its first poll, the data endpoint's until its first packet, however late
 * they come (u2ps_dwc2_iso.h): one pass per microframe until then. Only
 * SODDFRM or SEVNFRM acts: EPENA and EPDIS are of access type rs and SNAK
 * and CNAK of type w, so the zeros written to them change nothing (RM0456
 * 1.2, p. 127; 73.14.48, p. 3377, OTG_DIEPCTLx; 73.14.59, p. 3387,
 * OTG_DOEPCTLx of an isochronous endpoint); the characteristics are written
 * as they are. A data endpoint write that lands once its microframe has
 * begun is counted (out_follow_late). */
U2PS_FAST_TEXT static void stream_eopf(void)
{
  d.diag.eopf++;
  PASS_IS(U2PS_DWC2_PASS_EOPF);
  uint32_t fn = u2ps_dwc2_fnsof();
  bool odd;
  if (d.eopf_out) {
    if (u2ps_dwc2_out_eopf(&d.out, fn, &odd)) {
      OTG_DOEPCTL(d.ep) = out_ctl() | parity_bits(odd);
      u2ps_dwc2_out_eopf_after(&d.out, u2ps_dwc2_fnsof());
    } else {
      d.eopf_out = false;
    }
  }
  if (d.eopf_fb) {
    if (d.fb_state == FB_ARMED && u2ps_dwc2_fb_eopf(&d.fb, fn)) {
      OTG_DIEPCTL(d.ep_fb) = fb_ctl() | parity_bits(d.fb.odd);
      d.fb_start.flips++;
    } else {
      d.eopf_fb = false;
    }
  }
  eopf_set(d.eopf_fb || d.eopf_out);
}

U2PS_FAST_TEXT void u2ps_dport_fb_set(uint32_t value)
{
  *(volatile uint32_t *)&fb_word[0] = value; /* one store; the DMA reads it */
}

/* ---- The stream: open and close --------------------------------------------- */

bool u2ps_dport_stream_open(const u2ps_stream_t *s)
{
  /* The device stack's fields first; then the app's class memory, whose
   * cause u2ps_dwc2_dcd_open_error() tells */
  d.open_err = U2PS_DWC2_OPEN_STREAM;
  if (d.open || d.closing)
    return false;
  if (s->ep_out < 1u || s->ep_out > U2PS_DPORT_EP_MAX || s->ep_fb > U2PS_DPORT_EP_MAX)
    return false;
  if (s->mps < 1u || s->mps > U2PS_ISO_OUT_MPS_MAX)
    return false;
  if (s->ep_fb != 0u &&
      (s->fb_size < 1u || s->fb_size > FB_MAX || s->fb_interval < U2PS_DWC2_FB_INTERVAL_MIN ||
       s->fb_interval > U2PS_DWC2_FB_INTERVAL_MAX))
    return false;
  d.open_err = u2ps_dwc2_mem_check((uintptr_t)s->mem, s->stride, s->count, s->mps,
                                   U2PS_DPORT_ISO_OUT_MIN, U2PS_ISO_OUT_COUNT_MAX,
                                   U2PS_DPORT_ALIGN);
  if (d.open_err == U2PS_DWC2_OPEN_OK &&
      !u2ps_dwc2_dma_ok(s->mem, (uint32_t)s->count * s->stride))
    d.open_err = U2PS_DWC2_OPEN_NOT_DMA;
  if (d.open_err != U2PS_DWC2_OPEN_OK)
    return false;

  d.ep = s->ep_out;
  d.ep_fb = s->ep_fb;
  d.fb_size = s->fb_size;
  d.mps = s->mps;
  d.xfrsiz = u2ps_dwc2_out_xfrsiz(s->mps);
  d.stride = s->stride;
  d.mem = s->mem;
  d.close_out = d.close_fb = false;

  /* Data endpoint: activate, unmask, arm buffer 0 for parity(now + 1);
   * the parity follows the bus at EOPF until the first packet, unless the
   * follow is off (a bench comparison) */
  OTG_DOEPCTL(d.ep) = out_ctl() | EPCTL_SNAK;
  OTG_DOEPINT(d.ep) = 0xFFFFFFFFu;
  daintmsk_set(d.daintmsk | DAINT_OEP(d.ep));
  out_buffer(0);
  bool odd = false;
  uint32_t primask = irq_save();
  (void)u2ps_dwc2_out_open(&d.out, s->count, u2ps_dwc2_fnsof(), &odd);
  OTG_DOEPCTL(d.ep) = out_ctl() | EPCTL_EPENA | EPCTL_CNAK | parity_bits(odd);
  irq_restore(primask);
  d.out.following = d.mode.out_follow;
  d.eopf_out = d.mode.out_follow;
  d.eopf_fb = false;

  if (d.ep_fb != 0u) {
    fb_word[0] = s->fb_value;
    OTG_DIEPCTL(d.ep_fb) = fb_ctl() | EPCTL_SNAK;
    OTG_DIEPINT(d.ep_fb) = 0xFFFFFFFFu;
    if (!u2ps_dwc2_flush_tx(FB_FIFO))
      d.diag.timeouts++;
    daintmsk_set(d.daintmsk | DAINT_IEP(d.ep_fb));
    u2ps_dwc2_dcache_clean(fb_word, sizeof fb_word);
    OTG_DIEPDMA(d.ep_fb) = (uint32_t)(uintptr_t)fb_word;
    OTG_DIEPTSIZ(d.ep_fb) = TSIZ_MCNT(1u) | TSIZ_PKTCNT(1u) | d.fb_size;
    primask = irq_save();
    uint32_t fn = u2ps_dwc2_fnsof();
    (void)u2ps_dwc2_fb_open(&d.fb, s->fb_interval, fn);
    OTG_DIEPCTL(d.ep_fb) = fb_ctl() | EPCTL_EPENA | EPCTL_CNAK | parity_bits(d.fb.odd);
    irq_restore(primask);
    d.fb_state = FB_ARMED;
    d.fb_start = (u2ps_dwc2_fb_start_t){
      .opens = d.fb_start.opens + 1u,
      .open_uframe = (uint16_t)(fn & U2PS_UFRAME_MASK),
      .open_odd = d.fb.armed_odd,
    };
    /* The parity follows the bus until the first poll, and IISOIXFR is not
     * needed before it. Without the follow (a bench comparison), the
     * guessed parity stays and IISOIXFR drives the switch rule. */
    if (d.mode.fb_follow) {
      d.eopf_fb = true;
      iiso_set(false);
    } else {
      u2ps_dwc2_fb_follow_off(&d.fb);
      iiso_set(true);
    }
  }
  eopf_set(d.eopf_fb || d.eopf_out);
  d.open = true;
  return true;
}

/* The holes recorded after the last delivered packet, up to the last
 * packet received, taken from the logic and reported */
static void out_tail_report(void)
{
  u2ps_dwc2_out_packet_t t;
  u2ps_dwc2_out_tail(&d.out, &t);
  if (t.late != 0u)
    out_missed(t.late, U2PS_MISS_LATE);
  if (t.no_buffer != 0u)
    out_missed(t.no_buffer, U2PS_MISS_NO_BUFFER);
  if (t.no_packet != 0u)
    out_missed(t.no_packet, U2PS_MISS_NO_PACKET);
}

/* At a tick in a long gap, in thread with the vectors masked: what the
 * logic holds back for the next packet record, now; no register is read
 * or written */
void u2ps_dport_report_holes(void)
{
  if (d.open) /* not during a close */
    out_tail_report();
}

/* Endpoints deactivated and masked (endpoint deactivation, RM0456 p. 3437).
 * First the stream's tail: the holes after its last delivered packet, up
 * to the last packet received, before on_closed or on_reset, or inside
 * connect(false) (u2ps_dport.h). */
static void stream_end(void)
{
  out_tail_report();
  u2ps_dwc2_out_close(&d.out);
  u2ps_dwc2_fb_close(&d.fb);
  gintmsk_set(d.gintmsk & ~(GINT_IISOIXFR | GINT_EOPF | GINT_GONAKEFF));
  d.iiso_on = d.eopf_on = d.eopf_fb = d.eopf_out = false;
  OTG_DOEPCTL(d.ep) = EPCTL_SNAK;
  OTG_DOEPINT(d.ep) = 0xFFFFFFFFu;
  daintmsk_set(d.daintmsk & ~DAINT_OEP(d.ep));
  if (d.ep_fb != 0u) {
    OTG_DIEPCTL(d.ep_fb) = EPCTL_SNAK;
    OTG_DIEPINT(d.ep_fb) = 0xFFFFFFFFu;
    daintmsk_set(d.daintmsk & ~DAINT_IEP(d.ep_fb));
  }
  d.fb_state = FB_OFF;
  d.open = d.closing = false;
  d.close_out = d.close_fb = false;
  d.ep = d.ep_fb = 0;
}

/* "Disabling an OUT endpoint" (RM0456 p. 3442; RM0431 32.16.6), each step
 * on its own interrupt: SGONAK here; at GONAKEFF EPDIS and SNAK on the data
 * and feedback endpoints; at their EPDISD the feedback Tx FIFO flush; then
 * CGONAK and on_closed. Packets that land before GONAKEFF are dropped. */
void u2ps_dport_stream_close(void)
{
  if (!d.open)
    return;
  d.open = false;
  d.closing = true;
  u2ps_dwc2_out_closing(&d.out);
  gintmsk_set(d.gintmsk & ~(GINT_IISOIXFR | GINT_EOPF));
  d.iiso_on = d.eopf_on = d.eopf_fb = d.eopf_out = false;
  d.close_t0 = u2ps_dwc2_cycles();
  /* SGONAK only with GONAKEFF clear (RM0456 73.14.39, p. 3367) */
  if ((OTG_GINTSTS & GINT_GONAKEFF) == 0u)
    OTG_DCTL |= DCTL_SGONAK;
  gintmsk_set(d.gintmsk | GINT_GONAKEFF);
}

OUT_OF_LINE static void close_gonakeff(void)
{
  /* GONAKEFF stays set until CGONAK: masked while the endpoints disable */
  gintmsk_set(d.gintmsk & ~GINT_GONAKEFF);
  if (!d.closing) {
    OTG_DCTL |= DCTL_CGONAK;
    return;
  }
  if ((OTG_DOEPCTL(d.ep) & EPCTL_EPENA) != 0u) {
    OTG_DOEPCTL(d.ep) = out_ctl() | EPCTL_EPDIS | EPCTL_SNAK;
    d.close_out = true;
  }
  if (d.ep_fb != 0u) {
    uint32_t ctl = OTG_DIEPCTL(d.ep_fb);
    if ((ctl & EPCTL_EPDIS) != 0u) {
      d.close_fb = true; /* a recovery's disable runs; its EPDISD comes */
    } else if ((ctl & EPCTL_EPENA) != 0u) {
      OTG_DIEPCTL(d.ep_fb) = fb_ctl() | EPCTL_EPDIS | EPCTL_SNAK;
      d.close_fb = true;
    } else if (!u2ps_dwc2_flush_tx(FB_FIFO)) {
      d.diag.timeouts++;
    }
    d.fb_state = FB_CLOSE;
  }
  close_check();
}

OUT_OF_LINE static void close_check(void)
{
  if (!d.closing || d.close_out || d.close_fb || (d.gintmsk & GINT_GONAKEFF) != 0u)
    return;
  OTG_DCTL |= DCTL_CGONAK;
  stream_end();
  d.diag.close_cycles = u2ps_dwc2_cycles() - d.close_t0;
  if (d.diag.close_cycles > d.diag.close_max)
    d.diag.close_max = d.diag.close_cycles;
  u2ps_dev_on_closed();
}

/* On every pass of a close: an endpoint found disabled counts as its
 * EPDISD, so Global OUT NAK cannot stay set when an EPDISD never comes
 * (it NAKs EP0's OUT data and status stages, RM0456 73.14.39, p. 3368). An
 * endpoint is disabled once the core has cleared both EPENA and EPDIS
 * (73.14.48, p. 3377). */
OUT_OF_LINE static void close_poll(void)
{
  if (d.close_fb && (OTG_DIEPCTL(d.ep_fb) & (EPCTL_EPENA | EPCTL_EPDIS)) == 0u) {
    d.diag.close_polled++;
    if (!u2ps_dwc2_flush_tx(FB_FIFO))
      d.diag.timeouts++;
    d.close_fb = false;
    close_check();
  }
  if (d.closing && d.close_out && (OTG_DOEPCTL(d.ep) & (EPCTL_EPENA | EPCTL_EPDIS)) == 0u) {
    d.diag.close_polled++;
    d.close_out = false;
    close_check();
  }
}

/* At a bus reset: the stream stops at once, with bounded waits ("Transfer
 * stop programming", RM0456 p. 3442 and p. 3450). The manual first enables
 * every OUT endpoint so that all can be disabled alike; here only enabled
 * endpoints are disabled, so no DMA address is armed again (choice). */
static void stream_stop_now(void)
{
  if (!d.open && !d.closing)
    return;
  if ((OTG_DOEPCTL(d.ep) & EPCTL_EPENA) != 0u) {
    if ((OTG_GINTSTS & GINT_GONAKEFF) == 0u) {
      if ((OTG_DCTL & DCTL_GONSTS) == 0u)
        OTG_DCTL |= DCTL_SGONAK;
      (void)wait(&OTG_GINTSTS, GINT_GONAKEFF, GINT_GONAKEFF, RESET_STOP_US);
    }
    OTG_DOEPCTL(d.ep) = out_ctl() | EPCTL_EPDIS | EPCTL_SNAK;
    (void)wait(&OTG_DOEPINT(d.ep), DOEPINT_EPDISD, DOEPINT_EPDISD, RESET_STOP_US);
  }
  if (d.ep_fb != 0u) {
    uint32_t ctl = OTG_DIEPCTL(d.ep_fb);
    if ((ctl & (EPCTL_EPENA | EPCTL_EPDIS)) != 0u) {
      if ((ctl & EPCTL_EPDIS) == 0u)
        OTG_DIEPCTL(d.ep_fb) = fb_ctl() | EPCTL_EPDIS | EPCTL_SNAK;
      (void)wait(&OTG_DIEPINT(d.ep_fb), DIEPINT_EPDISD, DIEPINT_EPDISD, RESET_STOP_US);
    }
    if (!u2ps_dwc2_flush_tx(FB_FIFO))
      d.diag.timeouts++;
  }
  if ((OTG_DCTL & DCTL_GONSTS) != 0u)
    OTG_DCTL |= DCTL_CGONAK;
  stream_end();
}

OUT_OF_LINE static void out_epdisd(void)
{
  if (d.closing && d.close_out) {
    d.close_out = false;
    close_check();
  }
}

/* ---- Bus events ------------------------------------------------------------- */

OUT_OF_LINE static void bus_reset(void)
{
  stream_stop_now();
  ep0_abort();
  d.b2b = false;
  d.suspended = false;
  OTG_DCFG &= ~DCFG_DAD_MSK;
  /* "Endpoint initialization on USB reset" (RM0456 p. 3436) */
  for (uint32_t n = 0; n <= U2PS_DPORT_EP_MAX; n++) {
    OTG_DOEPCTL(n) |= EPCTL_SNAK;
    OTG_DOEPINT(n) = 0xFFFFFFFFu;
    OTG_DIEPINT(n) = 0xFFFFFFFFu;
  }
  daintmsk_set(DAINT_IEP(0) | DAINT_OEP(0));
  OTG_DOEPMSK = DOEPINT_STUP | DOEPINT_XFRC | DOEPINT_EPDISD | DOEPINT_B2BSTUP | DOEPINT_AHBERR |
                DOEPINT_STSPHSRX;
  OTG_DIEPMSK = DIEPINT_XFRC | DIEPINT_TOC | DIEPINT_EPDISD | DIEPINT_AHBERR;
  /* FIFO RAM at every reset, sized for the largest stream (u2ps_dwc2_fifo.h) */
  if (!u2ps_dwc2_fifo_setup(U2PS_ISO_OUT_MPS_MAX, FB_MAX))
    d.errors++;
  /* STUPCNT = 3 and a valid DMA address before a SETUP can come (steps 4
   * and 5); the endpoint is enabled at ENUMDNE. An endpoint still enabled
   * from before the reset already points at the area, and its registers
   * may not be changed while it is enabled (RM0456 73.14.56, p. 3386). */
  if ((OTG_DOEPCTL(0) & EPCTL_EPENA) == 0u) {
    OTG_DOEPTSIZ(0) = TSIZ_STUPCNT(3u) | TSIZ_PKTCNT(1u) | U2PS_EP0_MPS;
    OTG_DOEPDMA(0) = (uint32_t)(uintptr_t)ep0_area;
  }
  u2ps_dev_on_reset();
}

OUT_OF_LINE static void enum_done(void)
{
  /* "Endpoint initialization on enumeration completion" (RM0456 p. 3436):
   * the speed, EP0's maximum packet size (64 at both speeds, as the
   * device stack serves it), EP0 OUT enabled for SETUPs */
  u2ps_speed_t speed =
    (OTG_DSTS & DSTS_ENUMSPD_MSK) == 0u ? U2PS_SPEED_HIGH : U2PS_SPEED_FULL;
  OTG_DIEPCTL(0) = EPCTL0_MPSIZ_64;
  ep0_out_enable(false);
  u2ps_dev_on_speed(speed);
}

U2PS_FAST_TEXT static void irq_pass(void)
{
  uint32_t gint = OTG_GINTSTS & d.gintmsk;
  uint32_t daint = OTG_DAINT & d.daintmsk;

  /* 1. The data endpoint */
  if (d.ep != 0u && (daint & DAINT_OEP(d.ep)) != 0u) {
    uint32_t i = OTG_DOEPINT(d.ep);
    OTG_DOEPINT(d.ep) = i;
    if ((i & DOEPINT_XFRC) != 0u)
      out_xfrc();
    if ((i & DOEPINT_AHBERR) != 0u)
      d.diag.ahberr++;
    if ((i & DOEPINT_EPDISD) != 0u)
      out_epdisd();
  }
  /* 2. Bus events */
  if ((gint & GINT_USBRST) != 0u) {
    OTG_GINTSTS = GINT_USBRST;
    PASS_IS(U2PS_DWC2_PASS_RESET);
    bus_reset();
    daint = 0; /* every endpoint interrupt was cleared */
  }
  if ((gint & GINT_ENUMDNE) != 0u) {
    OTG_GINTSTS = GINT_ENUMDNE;
    enum_done();
  }
  if ((gint & GINT_WKUPINT) != 0u) {
    OTG_GINTSTS = GINT_WKUPINT;
    if (d.suspended) {
      d.suspended = false;
      u2ps_dev_on_resume();
    }
  }
  /* 3. The feedback endpoint: XFRC before EOPF and IISOIXFR */
  if (d.ep_fb != 0u && (daint & DAINT_IEP(d.ep_fb)) != 0u)
    fb_int();
  if ((gint & GINT_EOPF) != 0u) {
    OTG_GINTSTS = GINT_EOPF;
    if (d.open)
      stream_eopf();
    else
      eopf_set(false);
  }
  if ((gint & GINT_IISOIXFR) != 0u) {
    OTG_GINTSTS = GINT_IISOIXFR;
    if (d.ep_fb != 0u)
      fb_iisoixfr();
  }
  /* 4. EP0 */
  if ((daint & DAINT_IEP(0)) != 0u)
    ep0_in_int();
  if ((daint & DAINT_OEP(0)) != 0u)
    ep0_out_int();
  /* 5. The rest */
  if ((gint & GINT_GONAKEFF) != 0u)
    close_gonakeff();
  if ((gint & GINT_ISOODRP) != 0u) {
    OTG_GINTSTS = GINT_ISOODRP;
    d.diag.isoodrp++;
  }
  if ((gint & GINT_USBSUSP) != 0u) {
    OTG_GINTSTS = GINT_USBSUSP;
    if (!d.suspended) {
      d.suspended = true;
      u2ps_dev_on_suspend();
    }
  }
  if ((gint & GINT_OTGINT) != 0u)
    OTG_GOTGINT = OTG_GOTGINT;
  if ((gint & GINT_MMIS) != 0u)
    OTG_GINTSTS = GINT_MMIS;
  if (d.closing)
    close_poll();
}

U2PS_FAST_TEXT void u2ps_dwc2_dcd_irq(void)
{
#if U2PS_TEST_INJECT
  /* The pass profile of the injection build */
  uint32_t t0 = u2ps_dwc2_cycles();
  pass_kind = U2PS_DWC2_PASS_OTHER;
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

/* ---- Start-up --------------------------------------------------------------- */

static u2ps_dev_err_t init_fail(u2ps_dwc2_err_t e)
{
  d.init_err = e;
  return u2ps_dwc2_dev_err(e);
}

/* The control buffer and port memory against the rule of
 * u2ps_dwc2_dma_ok(), with the PWR clock on first (U5) so that the SRAMs'
 * power-down bits can be read; the port's start enables it anyway */
bool u2ps_dport_dma_check(const void *mem, uint32_t len)
{
  u2ps_dwc2_pwr_clock();
  return u2ps_dwc2_dma_ok(mem, len) && u2ps_dwc2_dma_ok(ep0_area, sizeof ep0_area) &&
         u2ps_dwc2_dma_ok(fb_word, sizeof fb_word);
}

u2ps_dev_err_t u2ps_dport_init(void)
{
  memset(&d, 0, sizeof d);
  d.mode = (u2ps_dwc2_dcd_mode_t){
    .fb_follow = true, .iiso_window = true, .out_follow = U2PS_DWC2_OUT_FOLLOW != 0};
  u2ps_dwc2_err_t e = u2ps_dwc2_phy_start();
  if (e != U2PS_DWC2_OK)
    return init_fail(e);
  if (!u2ps_dwc2_core_reset())
    return init_fail(U2PS_DWC2_ERR_RESET);
  /* Port memory, now that the reset has stopped the core's DMA: zeroed, so
   * its section needs no zeroing at startup, and cleaned, so that no dirty
   * line is written back over what the DMA writes later (F723) */
  memset(ep0_area, 0, sizeof ep0_area);
  memset(fb_word, 0, sizeof fb_word);
  u2ps_dwc2_dcache_clean(ep0_area, sizeof ep0_area);
  u2ps_dwc2_dcache_clean(fb_word, sizeof fb_word);

  /* Device only, whatever the ID pin says; the change takes at least 25 ms
   * (RM0456 73.14.4, p. 3324) */
  OTG_GUSBCFG = (OTG_GUSBCFG & ~GUSBCFG_FHMOD) | GUSBCFG_FDMOD;
  u2ps_dwc2_delay_us(25000u);
  if (!u2ps_dwc2_wait_us(&OTG_GINTSTS, GINT_CMOD, 0u, 10000u))
    return init_fail(U2PS_DWC2_ERR_MODE);

  /* Core initialization (RM0456 73.15.1, p. 3402): DMA mode with INCR4
   * bursts (choice), the interrupt line masked until the end; the
   * turnaround time */
  OTG_GAHBCFG = (OTG_GAHBCFG & ~(GAHBCFG_HBSTLEN_MSK | GAHBCFG_GINTMSK)) | GAHBCFG_DMAEN |
                GAHBCFG_HBSTLEN_INCR4;
  OTG_GUSBCFG = (OTG_GUSBCFG & ~GUSBCFG_TRDT_MSK) | GUSBCFG_TRDT(TRDT_AHB_30MHZ);

  /* No VBUS sensing: the B-session is valid by override (RM0456 73.14.1,
   * p. 3320; on the U5 also 73.14.18, p. 3347). The boards do not wire
   * VBUS to the core (choice: both overrides), so SDIS alone decides the
   * pull-up, and the device stack keeps it off without VBUS (USB 2.0
   * 7.1.5.1, p. 141): in VBUS mode app the app reports VBUS, in mode bus
   * the board powers the device from it (u2ps_dev.h). */
  OTG_GOTGCTL |= GOTGCTL_BVALOEN | GOTGCTL_BVALOVAL;
#if U2PS_DWC2_GCCFG_OVR == 1
  OTG_GCCFG = (OTG_GCCFG & ~GCCFG_VBDEN) | GCCFG_VBVALOVEN | GCCFG_VBVALOVAL;
#elif U2PS_DWC2_GCCFG_OVR != 0
#error "u2ps_dwc2_dcd.c: U2PS_DWC2_GCCFG_OVR must be 0 or 1 (u2ps_dwc2_chip.h)"
#endif

  /* Device initialization (RM0456 73.15.3, p. 3403): disconnected until
   * u2ps_dport_connect(); speed; status OUT handshake as the endpoint's
   * NAK and STALL say; the periodic frame interval at 95 %, so IISOIXFR
   * comes after a late feedback poll; address 0 */
  OTG_DCTL |= DCTL_SDIS;
  OTG_DCFG = (OTG_DCFG & ~(DCFG_PFIVL_MSK | DCFG_DAD_MSK | DCFG_NZLSOHSK | DCFG_DSPD_MSK)) |
             DCFG_PFIVL_95 | (U2PS_DWC2_FORCE_FS ? DCFG_DSPD_FS : DCFG_DSPD_HS);
  if (!u2ps_dwc2_fifo_setup(U2PS_ISO_OUT_MPS_MAX, FB_MAX))
    return init_fail(U2PS_DWC2_ERR_FIFO);

  /* Interrupts: clear, then unmask. RXFLVL and NPTXFE stay masked in DMA
   * mode (RM0456 p. 3437), INCOMPISOOUT too: the stream's misses come from
   * the microframe numbers. EOPF and IISOIXFR are unmasked by the stream
   * when its feedback endpoint needs them. */
  OTG_GINTSTS = 0xFFFFFFFFu;
  gintmsk_set(GINT_INIT);
  d.daintmsk = OTG_DAINTMSK; /* its reset value, until the first bus reset writes it */
  OTG_GAHBCFG |= GAHBCFG_GINTMSK;
  d.up = true;
  return U2PS_DEV_OK;
}

/* The device stack has detached (u2ps_dport_connect), so no stream runs
 * and EP0 is idle; a bus reset after the next attach sets the rest up */
void u2ps_dport_reinit(void)
{
  if (!d.up)
    return;
  stream_stop_now();
  ep0_abort();
  d.b2b = false;
  d.suspended = false;
  OTG_DCFG &= ~DCFG_DAD_MSK;
}

u2ps_dwc2_err_t u2ps_dwc2_dcd_init_error(void)
{
  return d.init_err;
}

u2ps_dwc2_open_err_t u2ps_dwc2_dcd_open_error(void)
{
  return d.open_err;
}

/* A detach stops the stream as a bus reset does, then removes the pull-up
 * (SDIS, RM0456 73.14.39, p. 3368); the vectors are masked meanwhile.
 * It also ends test mode (TCTL 000, "Test mode disabled", the same
 * register; RM0431 32.15.36), so that the transceiver drives nothing once
 * the pull-up is off, also at a VBUS loss. Address 0 again, so the device
 * answers at 0 after the next attach. */
void u2ps_dport_connect(bool on)
{
  if (!d.up)
    return;
  if (on) {
    OTG_DCTL &= ~DCTL_SDIS;
    return;
  }
  stream_stop_now();
  ep0_abort();
  d.b2b = false;
  d.suspended = false;
  OTG_DCTL = (OTG_DCTL & ~DCTL_TCTL_MSK) | DCTL_SDIS;
  OTG_DCFG &= ~DCFG_DAD_MSK;
}

/* The vector is enabled only once the port has started: a mask call after
 * a failed start leaves it off */
static void vec_mask(bool masked)
{
  if (masked || d.up)
    u2ps_dwc2_irq_mask(masked);
}

void u2ps_dport_irq_mask(bool masked)
{
  vec_mask(masked);
}

const u2ps_dport_stats_t *u2ps_dport_stats(void)
{
  d.stats.late_completions = d.out.late_completions;
  d.stats.late = d.out.late_total;
  d.stats.no_buffer = d.out.no_buffer_total;
  d.stats.corrupted = 0; /* the core drops CRC-error packets unseen (RM0456 p. 3446) */
  d.stats.fb_recoveries = d.fb.recoveries;
  d.stats.errors = d.out.errors + d.errors;
  d.stats.fallback_over = 0; /* microframe numbers are exact: no estimate */
  return &d.stats;
}

void u2ps_dwc2_dcd_diag(u2ps_dwc2_dcd_diag_t *diag)
{
  vec_mask(true);
  *diag = d.diag;
  diag->close_drops = d.out.close_drops;
  diag->out_follows = d.out.follows;
  diag->out_follow_late = d.out.follow_late;
  diag->fb_idle_recoveries = d.fb.idle_recoveries;
  vec_mask(false);
}

void u2ps_dwc2_dcd_fb_start(u2ps_dwc2_fb_start_t *rec)
{
  vec_mask(true);
  *rec = d.fb_start;
  vec_mask(false);
}

void u2ps_dwc2_dcd_mode(const u2ps_dwc2_dcd_mode_t *mode)
{
  vec_mask(true);
  d.mode = *mode;
  vec_mask(false);
}

void u2ps_dwc2_dcd_mode_get(u2ps_dwc2_dcd_mode_t *mode)
{
  vec_mask(true);
  *mode = d.mode;
  vec_mask(false);
}
