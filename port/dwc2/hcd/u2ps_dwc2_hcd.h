/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_hcd.h: what the DWC2 host port offers the app beyond the host
 * port interface (u2ps_hport.h): its interrupt handler, the reason a start
 * failed, its diagnostic counters, and, in injection builds, the injection
 * hook and the handler's pass profile. ARM builds only.
 *
 * What the port interface leaves to each port:
 * - u2ps_hport_init() maps the reasons of u2ps_dwc2_err_t onto
 *   u2ps_host_err_t: CLOCK to U2PS_HOST_ERR_CLOCK, POWER to
 *   U2PS_HOST_ERR_POWER, TZEN to U2PS_HOST_ERR_SECURITY, PHY to
 *   U2PS_HOST_ERR_PHY, RESET to U2PS_HOST_ERR_RESET, MODE to
 *   U2PS_HOST_ERR_MODE (the core did not enter host mode), FIFO to
 *   U2PS_HOST_ERR_MEMORY, CYCLES (the cycle counter does not count) to
 *   U2PS_HOST_ERR_TIMER (u2ps_dwc2_host_err(), u2ps_dwc2_start.h). The PHY
 *   starts at the first init that reaches it and stays on; a later init
 *   resets the core (CSRST) and sets it up again. Each init zeroes port
 *   memory (the feedback buffers, the status stage's buffer) after the core
 *   reset.
 * - u2ps_hport_dma_check() applies u2ps_dwc2_dma_ok() (u2ps_dwc2.h) to the
 *   host stack's memory it names and to port memory; the host stack then
 *   returns U2PS_HOST_ERR_DMA_MEMORY, and u2ps_dwc2_hcd_init_error() stays
 *   as the last init left it. u2ps_hport_dma_ranges() reports the same
 *   rule as ranges (u2ps_dwc2_dma_ranges(), at most 3, reading the
 *   registers then).
 * - The stream: a data endpoint of 1..1024 bytes, a feedback endpoint of
 *   bInterval 1..13 and wMaxPacketSize 4..U2PS_DWC2_HFB_MPS_MAX (64), at
 *   high speed only (u2ps_dwc2_hstream_fits(), u2ps_dwc2_hiso.h); another
 *   stream is refused, and the app gets U2PS_HOST_STOP_OPEN at its start.
 *   bInterval 14..16 (USB 2.0 allows 1..16) is refused because the port
 *   counts microframes modulo 2^14 (OTG_HFNUM.FRNUM) and takes distances
 *   as signed: a poll period of 2^(bInterval-1) microframes must stay
 *   below 2^13, so 2^12 (bInterval 13, 512 ms) is the longest. A
 *   feedback poll is armed in the microframe right before it
 *   (u2ps_dwc2_hiso.h): at the data channel's halts while packets flow,
 *   and at SOF interrupts, unmasked only while the data channel is idle
 *   (before the first packet, while the queue is dry). That SOF comes with
 *   every microframe at high speed is inferred (RM0456 73.14.6 names full
 *   and low speed only); without it, polls in such a time are skipped and
 *   reported LATE.
 * - u2ps_hport_queue() costs O(1): the packet's cache lines cleaned (F723,
 *   cached memory), then, if the data channel is idle, its arm.
 * - u2ps_hport_disable() halts the channels with bounded waits (at most
 *   3 ms a channel); a channel that does not halt in time is flushed
 *   (CHDIS, RM0456 p. 3404) and counted in timeouts. A disconnect, and a
 *   port the hardware disabled (port babble, an overcurrent), flush every
 *   channel at once, without a wait: the port sends no more tokens. Only
 *   port babble resets the core, as RM0456 asks (p. 3426), inside the
 *   handler: CSRST, host mode forced again and awaited (FHMOD, at least
 *   25 ms, p. 3324), the host set-up; about 26 ms, at most 66 ms. The port
 *   then reports the attached device again (on_connect).
 * - Control transfers: NAKs retried without limit; the third transaction
 *   error in a row ends a transfer (two retries, USB 2.0 10.2.6), and data
 *   moved, a NAK and a data toggle error (DTERR, retried) restart the
 *   count (u2ps_dwc2_hctl.h); u2ps_hport_stats().control_retries counts
 *   the retries.
 */
#ifndef U2PS_DWC2_HCD_H
#define U2PS_DWC2_HCD_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_dwc2.h"

/* The port's one handler: the app's OTG_HS interrupt handler calls it,
 * the vector function its startup file places at IRQ number U2PS_DWC2_IRQ
 * (u2ps_dwc2.h; OTG_HS_IRQHandler in ST's startup files). The app sets
 * that IRQ's NVIC priority and enables it after a successful
 * u2ps_host_init(); no other interrupt that runs port or stack code may
 * share or exceed that priority. At port babble one pass holds the
 * handler for the core soft reset: about 26 ms, at most 66 ms (above). */
void u2ps_dwc2_hcd_irq(void);

/* Why u2ps_hport_init() failed, in the port's own terms; U2PS_DWC2_OK if
 * it did not. Any context. */
u2ps_dwc2_err_t u2ps_dwc2_hcd_init_error(void);

/* Counters for bench checks; they restart at each u2ps_host_init() */
typedef struct {
  uint32_t ctl_naks;    /* control channel halts with NAK set: the device NAKed, the core retried */
  uint32_t ctl_txerr;   /* control channel halts with TXERR */
  uint32_t ctl_bberr;   /* control channel halts with BBERR */
  uint32_t ctl_dterr;   /* control channel halts with DTERR */
  uint32_t ahberr;      /* AHBERR on any channel */
  uint32_t rereads;     /* stream channel halts whose first FRNUM read lay before the transfer */
  uint32_t ipxfr;       /* passes that found IPXFR set: a periodic transfer left incomplete */
  uint32_t babble;      /* port babbles; each ends with a core soft reset */
  uint32_t overcurrent; /* POCCHNG */
  uint32_t soft_resets; /* core soft resets after port babble */
  uint32_t wakeups;     /* remote wakeups reported (on_wakeup) */
  uint32_t timeouts;    /* register waits that timed out */
  uint32_t no_chh;      /* channel interrupts without CHH (AHBERR before the halt) */
  uint32_t ctl_held;    /* control arms held for the second halt after an abort met a halt */
  uint32_t enable_prst; /* enables (PENCHNG with PENA) that came while PRST was still set */
  uint32_t hprt;        /* OTG_HPRT at the last port interrupt */
  uint8_t speed;        /* u2ps_speed_t of the last enable (PSPD) */
} u2ps_dwc2_hcd_diag_t;

/* App, thread: a copy of the counters, taken with the vector masked */
void u2ps_dwc2_hcd_diag(u2ps_dwc2_hcd_diag_t *diag);

#if defined(U2PS_TEST_INJECT) && U2PS_TEST_INJECT
/* The injection build (CMake option U2PS_TEST_INJECT, never in a release
 * build): late re-arms of the stream's data channel on purpose. Every
 * 'every'th re-arm after a halt that would be in time (made in the
 * packet's own microframe P) waits until P + 1 starts, then 'us'
 * microseconds more, then decides and arms. every = 0 turns it off. The
 * counters restart at each call. */
typedef struct {
  uint32_t every, us;    /* the setting */
  uint32_t injected;     /* re-arms held */
  uint32_t skipped;      /* chosen re-arms that were late already; the next one is taken */
  uint32_t timeouts;     /* holds that saw no new microframe within two */
  uint32_t d_min, d_max; /* DWT cycles from the start of P + 1 to the end of the arm */
} u2ps_dwc2_hinject_t;

void u2ps_dwc2_hinject(uint32_t every, uint32_t us);
void u2ps_dwc2_hinject_get(u2ps_dwc2_hinject_t *st);

/* Handler time by kind of pass, in DWT cycles: the kind is the first of
 * the list that the pass served */
typedef enum {
  U2PS_DWC2_HPASS_PORT,    /* a disconnect or a port event */
  U2PS_DWC2_HPASS_DATA,    /* the data channel's halt */
  U2PS_DWC2_HPASS_FB,      /* the feedback channel's halt */
  U2PS_DWC2_HPASS_CONTROL, /* the control channel's halt */
  U2PS_DWC2_HPASS_OTHER,
  U2PS_DWC2_HPASS_KINDS
} u2ps_dwc2_hpass_t;

typedef struct {
  uint32_t n[U2PS_DWC2_HPASS_KINDS];
  uint32_t max[U2PS_DWC2_HPASS_KINDS];
  uint64_t sum[U2PS_DWC2_HPASS_KINDS];
} u2ps_dwc2_hpasses_t;

/* A copy of the pass profile; clear = true restarts it */
void u2ps_dwc2_hpasses_get(u2ps_dwc2_hpasses_t *p, bool clear);
#endif

#endif /* U2PS_DWC2_HCD_H */
