/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_dcd.h: what the DWC2 device port offers the app beyond the
 * port interface (u2ps_dport.h): its interrupt handler, the reason a
 * start failed, its diagnostic counters and switches for bench
 * comparisons, and, in injection builds, the injection hook. ARM builds
 * only.
 *
 * What the port interface leaves to each port:
 * - u2ps_dport_init() maps the reasons of u2ps_dwc2_err_t onto
 *   u2ps_dev_err_t: CLOCK to U2PS_DEV_ERR_CLOCK, POWER to
 *   U2PS_DEV_ERR_POWER, TZEN to U2PS_DEV_ERR_SECURITY, PHY to
 *   U2PS_DEV_ERR_PHY, RESET to U2PS_DEV_ERR_RESET, MODE to
 *   U2PS_DEV_ERR_MODE, FIFO to U2PS_DEV_ERR_MEMORY, CYCLES (the cycle
 *   counter does not count) to U2PS_DEV_ERR_TIMER (u2ps_dwc2_dev_err(),
 *   u2ps_dwc2_start.h). It zeroes port memory (the EP0 area, the feedback
 *   word) after the core reset.
 * - u2ps_dport_dma_check() applies u2ps_dwc2_dma_ok() (u2ps_dwc2.h) to the
 *   device stack's control buffer and to port memory; the device stack
 *   then returns U2PS_DEV_ERR_DMA_MEMORY, and u2ps_dwc2_dcd_init_error()
 *   stays U2PS_DWC2_OK, since the port did not start.
 * - fb_recoveries counts the feedback endpoint's recoveries only if a data
 *   packet came in the last feedback period and a poll came after the
 *   recovery (u2ps_dwc2_iso.h); the others are fb_idle_recoveries in the
 *   diagnostic counters. A recovery is counted at the next poll.
 * - fallback_over is always 0: the microframe numbers are exact.
 * - A detach and u2ps_dport_reinit() stop a stream with the bounded
 *   waits of a bus reset (each at most 1 ms), in thread with the vector
 *   masked.
 */
#ifndef U2PS_DWC2_DCD_H
#define U2PS_DWC2_DCD_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_dwc2.h"
#include "u2ps_dwc2_iso.h"

/* The port's one handler: the app's OTG_HS interrupt handler calls it,
 * the vector function its startup file places at IRQ number U2PS_DWC2_IRQ
 * (u2ps_dwc2.h; OTG_HS_IRQHandler in ST's startup file for the STM32U5).
 * The app sets that IRQ's NVIC priority and enables it after a successful
 * u2ps_dev_init(); no other interrupt that runs port or stack code may
 * share or exceed that priority. */
void u2ps_dwc2_dcd_irq(void);

/* Why u2ps_dport_init() failed, in the port's own terms;
 * U2PS_DWC2_OK if it did not. Any context. */
u2ps_dwc2_err_t u2ps_dwc2_dcd_init_error(void);

/* Why the last u2ps_dport_stream_open() refused the stream, the cause of
 * the device stack's U2PS_FAULT_OPEN: which rule of u2ps_stream_cfg_t the
 * app's class memory broke (u2ps_dwc2_open_err_t, u2ps_dwc2_iso.h, with
 * each fix); U2PS_DWC2_OPEN_OK after an open that succeeded. Any context;
 * read it in u2ps_app_on_fault(U2PS_FAULT_OPEN) or after it. */
u2ps_dwc2_open_err_t u2ps_dwc2_dcd_open_error(void);

/* How the feedback endpoint's first poll after an open was met: one record
 * per open, kept until the next */
typedef struct {
  uint32_t opens;         /* stream opens with a feedback endpoint */
  uint16_t open_uframe;   /* FNSOF read at the open's arm */
  bool open_odd;          /* the parity armed at the open */
  uint16_t flips;         /* EOPF parity writes before the first XFRC */
  uint16_t switches;      /* switches before the first XFRC */
  bool polled;            /* a feedback XFRC came since the open */
  uint16_t first_q;       /* the poll microframe Q of the first XFRC */
  bool first_odd;         /* the parity armed at the first XFRC */
  uint16_t first_after;   /* microframes from the open to the first XFRC */
} u2ps_dwc2_fb_start_t;

/* Counters for bench checks; they never reset */
typedef struct {
  uint32_t iisoixfr;      /* IISOIXFR interrupts while the feedback endpoint was open */
  uint32_t iiso_hist[8];  /* those with a poll expected, by (FNSOF - expected poll) mod 8 */
  uint32_t eopf;          /* EOPF interrupts (a parity following the bus) */
  uint32_t out_follows;   /* the data endpoint's parity writes before its first packet */
  uint32_t out_follow_late; /* of those, written once their microframe had begun: a first
                               packet in it may have been lost */
  uint32_t fb_forced;     /* recoveries ended without their EPDISD (inferred fallback) */
  uint32_t fb_idle_recoveries; /* recoveries and switches not in fb_recoveries: no data flowed,
                                  or no poll followed */
  uint32_t fb_arm_busy;   /* feedback arms skipped because the endpoint was still enabled */
  uint32_t isoodrp;       /* ISOODRP: isochronous OUT packets dropped for Rx FIFO space */
  uint32_t pid_drops;     /* data packets dropped for RXDPID other than DATA0 */
  uint32_t close_drops;   /* data packets that landed during a close, dropped */
  uint32_t close_polled;  /* close steps taken from a disabled endpoint without its EPDISD */
  uint32_t close_cycles;  /* DWT cycles from the last close's SGONAK to its on_closed */
  uint32_t close_max;     /* the longest of those */
  uint32_t ahberr;        /* AHBERR on any endpoint */
  uint32_t b2bstup;       /* more than three back-to-back SETUP packets */
  /* SETUPs that found the EP0 area and its guard full. A known limit:
   * nothing bounds the EP0 OUT DMA while SETUPs come back to back. RM0456
   * 73.14.55 (p. 3384) allows any number of them before the IN or OUT
   * token that ends the setup stage, and what the core's buffer DMA does
   * past the third is not documented and not measured. If it advances 8
   * bytes for each, a host that sends 17 or more without such a token
   * writes past the 64-byte area and its 64-byte guard into the DMA memory
   * after them before this counts it. Only a faulty or hostile host does
   * that. */
  uint32_t ep0_overrun;
  uint32_t ep0_dropped;   /* EP0 calls dropped because a newer SETUP was held */
  uint32_t ep0_undone;    /* EP0 IN or OUT stages taken back: a SETUP arrived during the arm */
  uint32_t lost_ack;      /* control-read data stages ended by the host's status OUT */
  uint32_t in_setups;     /* device-to-host SETUPs (XFRC with STPKTRX) */
  uint32_t in_setup_ittxfe; /* of those, with DIEPINT0.ITTXFE already set */
  uint32_t timeouts;      /* register waits that timed out */
} u2ps_dwc2_dcd_diag_t;

/* App, thread: a copy of the counters, taken with the vector masked */
void u2ps_dwc2_dcd_diag(u2ps_dwc2_dcd_diag_t *diag);

/* App, thread: the feedback start record of the last open */
void u2ps_dwc2_dcd_fb_start(u2ps_dwc2_fb_start_t *rec);

/* Switches for bench comparisons; all true after u2ps_dport_init(), but
 * out_follow false in a build with U2PS_DWC2_OUT_FOLLOW=OFF. They take
 * effect at the next stream open.
 * - fb_follow: the feedback parity follows the bus from the open to the
 *   first poll (u2ps_dwc2_iso.h); false: the parity guessed at the open
 *   stays until the switch rule changes it.
 * - out_follow: the data endpoint's parity follows the bus from the open
 *   to the first packet (u2ps_dwc2_out_eopf); false: the parity guessed at
 *   the open stays, and a first packet in a microframe of the other
 *   parity is lost.
 * - iiso_window: IISOIXFR unmasked only around an expected poll; false:
 *   unmasked for the whole stream. */
typedef struct {
  bool fb_follow;
  bool iiso_window;
  bool out_follow;
} u2ps_dwc2_dcd_mode_t;

void u2ps_dwc2_dcd_mode(const u2ps_dwc2_dcd_mode_t *mode);
void u2ps_dwc2_dcd_mode_get(u2ps_dwc2_dcd_mode_t *mode);

#if defined(U2PS_TEST_INJECT) && U2PS_TEST_INJECT
/* The injection build (CMake option U2PS_TEST_INJECT, never in a
 * release build): late re-arms of the stream's data endpoint on purpose.
 * Every 'every'th re-arm after a completion that would be in time (made in
 * the packet's own microframe P) waits until P + 1 starts, then 'us'
 * microseconds more, then decides and arms. every = 0 turns it off. The
 * counters restart at each call. */
typedef struct {
  uint32_t every, us;  /* the setting */
  uint32_t injected;   /* re-arms held */
  uint32_t skipped;    /* chosen re-arms that were late already; the next one is taken */
  uint32_t timeouts;   /* holds that saw no new microframe within two */
  uint32_t d_min, d_max; /* DWT cycles from the start of P + 1 to the end of the arm */
} u2ps_dwc2_inject_t;

void u2ps_dwc2_inject(uint32_t every, uint32_t us);
void u2ps_dwc2_inject_get(u2ps_dwc2_inject_t *st);

/* Lost-ACK injection: the final XFRC of every 'every'th control-read data
 * stage is not taken, so the host's status OUT finds the stage still
 * waiting for its ACK (USB 2.0 8.5.3.3). 0 turns it off. */
void u2ps_dwc2_inject_lost_ack(uint32_t every);

/* Handler time by kind of pass, in DWT cycles: the kind is the first of
 * the list that the pass served */
typedef enum {
  U2PS_DWC2_PASS_RESET,     /* bus reset */
  U2PS_DWC2_PASS_EP0_STOP,  /* an EP0 IN stage stopped (SETUP over an armed stage, lost ACK) */
  U2PS_DWC2_PASS_FB_FLUSH,  /* a feedback recovery's flush and re-arm */
  U2PS_DWC2_PASS_SETUP,     /* a SETUP */
  U2PS_DWC2_PASS_OUT,       /* a data packet */
  U2PS_DWC2_PASS_IISO,      /* IISOIXFR without any of the above */
  U2PS_DWC2_PASS_EOPF,      /* EOPF without any of the above */
  U2PS_DWC2_PASS_OTHER,
  U2PS_DWC2_PASS_KINDS
} u2ps_dwc2_pass_t;

typedef struct {
  uint32_t n[U2PS_DWC2_PASS_KINDS];
  uint32_t max[U2PS_DWC2_PASS_KINDS];
  uint64_t sum[U2PS_DWC2_PASS_KINDS];
} u2ps_dwc2_passes_t;

/* A copy of the pass profile; clear = true restarts it */
void u2ps_dwc2_passes_get(u2ps_dwc2_passes_t *p, bool clear);
#endif

#endif /* U2PS_DWC2_DCD_H */
