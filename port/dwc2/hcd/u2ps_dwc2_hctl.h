/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_hctl.h: the DWC2 host port's decision logic for the root port
 * and the control channel: the values its handlers write to OTG_HPRT for
 * suspend, resume, test mode, disable and the change bits; the port's
 * speed; the event an OTG_HPRT read at a port interrupt gives; OTG_HCCHARx
 * and OTG_HCTSIZx of the control channel at each speed and endpoint 0
 * packet size, and of the stream's channels; the control transfer stage by
 * stage, one channel transfer per stage or part of a stage. Pure functions
 * with no register access, as u2ps_dwc2_hiso.h: they build on the PC and
 * on ARM.
 *
 * Manual: RM0456 Rev 7 (STM32U5), chapter 73; the STM32F723's RM0431 has
 * the same OTG_HPRT fields (32.15.27).
 * - OTG_HPRT (73.14.30, pp. 3358-3360): PCDET, PENA, PENCHNG and POCCHNG
 *   are rc_w1: a 1 written clears them, and a 1 written to PENA disables
 *   the port. Every read-modify-write therefore writes them as 0, unless
 *   the write is meant to clear one (u2ps_dwc2_hprt_write).
 * - PSUSP: "the core only stops sending SOFs"; PRES: the application sets
 *   it and clears it after the resume time, and the core sets it itself
 *   on a remote wakeup (WKUPINT in OTG_GINTSTS, p. 3359).
 * - PTCTL, bits 16:13: 1 Test_J, 2 Test_K, 3 Test_SE0_NAK, 4 Test_Packet,
 *   5 Test_Force_Enable (p. 3358), the selectors of USB 2.0 Table 9-7.
 * - PSPD, bits 18:17: 00 high, 01 full, 10 low speed (p. 3358).
 * - OTG_HCCHARx (73.14.31, pp. 3360-3361): DAD 28:22, MCNT 21:20 (at least
 *   01), EPTYP 19:18 (00 control), LSDEV 17 (a low-speed device), EPDIR 15
 *   (1 IN), EPNUM 14:11, MPSIZ 10:0.
 * - OTG_HCTSIZx (73.14.35, pp. 3364-3365): DPID 30:29 (00 DATA0, 10 DATA1,
 *   11 SETUP for a control channel), PKTCNT 28:19, XFRSIZ 18:0; for an IN
 *   transfer XFRSIZ is the buffer reserved, a multiple of the maximum
 *   packet size (p. 3365), so a zero-length IN status stage reserves one
 *   packet.
 * Frames: at full and low speed the core sends a SOF, or a keep-alive, every
 * 1 ms frame, timed by OTG_HFIR, which the core computes from the PHY clock
 * selected in OTG_HCFG.FSLSPCS when the application leaves it alone
 * (73.14.24, 73.14.25, pp. 3354-3355); the host stack times its own waits
 * in milliseconds at every speed.
 */
#ifndef U2PS_DWC2_HCTL_H
#define U2PS_DWC2_HCTL_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_dwc2_hiso.h"
#include "u2ps_types.h"

/* OTG_HPRT bits (RM0456 73.14.30) */
#define U2PS_DWC2_HPRT_PCSTS     (1u << 0)
#define U2PS_DWC2_HPRT_PCDET     (1u << 1) /* rc_w1 */
#define U2PS_DWC2_HPRT_PENA      (1u << 2) /* rc_w1: a 1 written disables the port */
#define U2PS_DWC2_HPRT_PENCHNG   (1u << 3) /* rc_w1 */
#define U2PS_DWC2_HPRT_POCA      (1u << 4)
#define U2PS_DWC2_HPRT_POCCHNG   (1u << 5) /* rc_w1 */
#define U2PS_DWC2_HPRT_PRES      (1u << 6)
#define U2PS_DWC2_HPRT_PSUSP     (1u << 7)
#define U2PS_DWC2_HPRT_PRST      (1u << 8)
#define U2PS_DWC2_HPRT_PPWR      (1u << 12)
#define U2PS_DWC2_HPRT_PTCTL_MSK (0xFu << 13)
#define U2PS_DWC2_HPRT_PSPD_MSK  (0x3u << 17)
/* The rc_w1 bits */
#define U2PS_DWC2_HPRT_W1C                                                                \
  (U2PS_DWC2_HPRT_PCDET | U2PS_DWC2_HPRT_PENA | U2PS_DWC2_HPRT_PENCHNG |               \
   U2PS_DWC2_HPRT_POCCHNG)

/* The OTG_HPRT value to write after reading 'hprt': the bits of 'set' set,
 * those of 'clear' cleared, every other bit as read, and the rc_w1 bits
 * written 0 unless 'set' names them: set PENA to disable the port, a
 * change bit to clear it. Suspend: set PSUSP; resume: set PRES, then clear
 * PRES; test mode s: clear PTCTL, set U2PS_DWC2_HPRT_PTCTL(s). */
uint32_t u2ps_dwc2_hprt_write(uint32_t hprt, uint32_t set, uint32_t clear);

/* PTCTL for test selector s (1..5, USB 2.0 Table 9-7); 0 for another */
uint32_t u2ps_dwc2_hprt_ptctl(uint8_t selector);

/* The speed the port enabled at, from PSPD: U2PS_SPEED_HIGH, FULL or LOW;
 * U2PS_SPEED_NONE for the reserved code 11 */
u2ps_speed_t u2ps_dwc2_hprt_speed(uint32_t hprt);

/* The port is out of resume: PRES reads 0 (resume(false) returns this) */
bool u2ps_dwc2_hprt_resumed(uint32_t hprt);

/* OTG_HCCHARx of the control channel to endpoint 0 of device 'addr' at
 * 'speed' with packet size 'mps0', for an IN (in) or OUT stage: EPTYP
 * control, EPNUM 0, MCNT 01, LSDEV at low speed only; CHENA, CHDIS and
 * ODDFRM 0 */
uint32_t u2ps_dwc2_hcchar_ep0(uint8_t addr, uint8_t mps0, bool in, u2ps_speed_t speed);

/* The stages of a control transfer, each its own channel transfer */
typedef enum {
  U2PS_DWC2_STAGE_SETUP,    /* 8 bytes, DPID SETUP, OUT */
  U2PS_DWC2_STAGE_DATA_IN,  /* len bytes in, DPID DATA1 */
  U2PS_DWC2_STAGE_DATA_OUT, /* len bytes out, DPID DATA1 */
  U2PS_DWC2_STAGE_STATUS    /* zero bytes, DPID DATA1, opposite to the data stage (or IN
                               without one: u2ps_dwc2_status_in) */
} u2ps_dwc2_stage_t;

/* OTG_HCTSIZx of a stage of len bytes (the SETUP stage: 8; the status
 * stage: 0), IN (in) or OUT, with packet size mps0 (8, 16, 32 or 64):
 * PKTCNT the packets of len, at least 1; XFRSIZ len for an OUT stage, and
 * for an IN stage PKTCNT times mps0, which the buffer must hold: a
 * zero-length IN status stage reserves mps0 bytes (73.14.35, p. 3365) */
uint32_t u2ps_dwc2_hctsiz_ctl(u2ps_dwc2_stage_t stage, bool in, uint16_t len, uint8_t mps0);

/* The status stage of a request is IN: a request without a data stage, or
 * one whose data stage is OUT (USB 2.0 8.5.3) */
bool u2ps_dwc2_status_in(const uint8_t setup[8]);

/* ---- Port events ----------------------------------------------------------------
 *
 * OTG_HPRT at a host port interrupt (HPRTINT, RM0456 73.14.6, p. 3329): the
 * change bits name the event, and the port's own state tells an enable
 * that a reset ended from a disable the hardware made.
 * - PCDET: "a device connection is detected" (p. 3360).
 * - PENCHNG with PENA 1: the reset sequence has ended and the port is
 *   enabled at PSPD (73.7.1, p. 3302; 73.15.2 steps 7-9, p. 3402). The
 *   host initialization orders it after the application clears PRST (steps
 *   7-8), but PRST is cleared "after the reset sequence is complete" (p.
 *   3358), so an enable while PRST is still set is kept, with its speed,
 *   and reported when the port ends the reset (u2ps_dwc2_hprt_reset_end).
 * - PENCHNG with PENA 0: the port was disabled "by an overcurrent condition,
 *   a disconnect condition, or by the application clearing this bit"
 *   (p. 3360). A disconnect also raises DISCINT (73.7.1, p. 3301), which
 *   the port serves as such; the application's own disable is no event;
 *   what remains, with the device still attached (PCSTS 1), is port babble
 *   or an overcurrent. Port babble "flushes the Rx FIFO and disables the
 *   port"; the application checks POCA and then performs a soft reset
 *   (73.15.5, "Handling babble conditions", p. 3426).
 * - PSUSP is of access type rs (p. 3358): a 0 written changes nothing and a
 *   1 suspends, so the acknowledging write writes it 0; PRST and PRES are rw
 *   and are written as read. */

/* Where the root port stands, as the port's handlers and calls track it */
typedef enum {
  U2PS_DWC2_PORT_IDLE,      /* not enabled: no reset yet, or disabled */
  U2PS_DWC2_PORT_RESET,     /* PRST set */
  U2PS_DWC2_PORT_RESET_END, /* PRST cleared; the enable is awaited */
  U2PS_DWC2_PORT_ENABLED,   /* enabled, sending SOFs */
  U2PS_DWC2_PORT_SUSPENDED, /* PSUSP set */
  U2PS_DWC2_PORT_RESUMING,  /* PRES set, by the port or by the core at a remote wakeup */
  U2PS_DWC2_PORT_TEST,      /* PTCTL set: only u2ps_hport_init() ends it */
  U2PS_DWC2_PORT_RESET_ENABLED /* PRST set, and the core has enabled the port already */
} u2ps_dwc2_port_t;

/* What an OTG_HPRT read at a host port interrupt gives */
typedef struct {
  uint32_t ack;       /* the OTG_HPRT value to write: clears the change bits read */
  bool connect;       /* on_connect: PCDET with PCSTS 1 */
  bool enabled;       /* on_enabled(speed): the reset the port ended has enabled it */
  u2ps_speed_t speed; /* the speed it enabled at (PSPD) */
  bool disabled;      /* on_disabled: the hardware disabled the enabled port */
  bool babble;        /* ... with POCA 0: port babble, which needs a soft reset */
  uint8_t state;      /* u2ps_dwc2_port_t after the event */
} u2ps_dwc2_hprt_event_t;

/* The event in 'hprt', read at the interrupt, with the port in 'state'
 * (u2ps_dwc2_port_t). An enable is reported (enabled) only in RESET_END;
 * in RESET it is kept: the state becomes RESET_ENABLED with its speed in
 * e->speed, for u2ps_dwc2_hprt_reset_end(). A disable counts only in
 * ENABLED, SUSPENDED, RESUMING or RESET_ENABLED with PCSTS 1. Each makes
 * the port's new state. */
void u2ps_dwc2_hprt_event(uint32_t hprt, uint8_t state, u2ps_dwc2_hprt_event_t *e);

/* The port ends the reset (u2ps_hport_reset(false)) in 'state': RESET
 * becomes RESET_END, whose enable the next PENCHNG reports;
 * RESET_ENABLED becomes ENABLED, and *now is true: the enable kept from
 * PRST is reported now (on_enabled with the speed kept). Any other state
 * stays, *now false. */
uint8_t u2ps_dwc2_hprt_reset_end(uint8_t state, bool *now);

/* ---- The stream's channels ---------------------------------------------------- */

/* OTG_HCCHARx of an isochronous channel to endpoint 'ep' of device 'addr'
 * with packet size 'mps', IN (in) or OUT: EPTYP 01, MCNT 01 (one
 * transaction per microframe), LSDEV 0 (high speed only); CHENA, CHDIS
 * and ODDFRM 0 (73.14.31) */
uint32_t u2ps_dwc2_hcchar_iso(uint8_t addr, uint8_t ep, uint16_t mps, bool in);

/* OTG_HCTSIZx of one isochronous packet of len bytes (an IN channel: the
 * bytes reserved): DPID DATA0, the only PID of a high-speed endpoint with
 * one transaction per microframe (USB 2.0 8.5.5, p. 229), PKTCNT 1,
 * XFRSIZ len */
uint32_t u2ps_dwc2_hctsiz_iso(uint16_t len);

/* The bytes an IN channel transfer received, from OTG_HCTSIZx read at its
 * halt: XFRSIZ, armed with 'reserved' bytes, counts down by the bytes
 * received (inferred: 73.14.35 states only what the application programs;
 * not yet measured on a board), at most 'cap' */
uint16_t u2ps_dwc2_hctsiz_in_len(uint32_t reserved, uint32_t hctsiz, uint16_t cap);

/* ---- The control transfer --------------------------------------------------------
 *
 * One stage at a time, each its own channel transfer on the control
 * channel (73.15.5, "Control transactions", pp. 3413-3414), in DMA mode,
 * where the core halts the channel at the end of each one and reports its
 * cause with CHH (pp. 3426-3428). The handler reads OTG_HCINTx and
 * OTG_HCTSIZx at the CHH and calls u2ps_dwc2_ctl_chh(), which says what to
 * arm next or how the transfer ended.
 * - NAK and NYET of an OUT stage at high speed: the core pings and retries
 *   by itself (p. 3427). A halt with NAK alone re-arms what is left (the
 *   port retries NAKs, u2ps_hport.h), uncounted as an error.
 * - TXERR, BBERR or FRMOR, or a halt without a cause: a transaction error.
 *   What the stage moved counts; the rest is re-armed, and the
 *   U2PS_DWC2_CTL_STRIKES-th error in a row ends the transfer: USB 2.0
 *   4.5.2 (p. 19, "up to three times") and 10.2.6 (p. 282, "If the error
 *   count ... reaches three, the host retires the transfer"); the ISR
 *   samples de-allocate the channel at Error_count == 3 (pp. 3409-3410).
 *   The count restarts at a transaction without a transmission error
 *   (10.2.6): a halt that moved data, as an ACK resets the samples' count;
 *   a halt with NAK alone; and DTERR, a data toggle error, which the
 *   sample resets the count at (p. 3411) and which is re-armed and counted
 *   in retries.
 * - STALL ends the transfer; so does AHBERR, an error of the DMA.
 * - A data stage longer than U2PS_DWC2_PKTCNT_MAX packets is armed in
 *   parts. The data PID of each part follows from the packets moved before
 *   it: DATA1 first (USB 2.0 8.5.3, p. 226), toggled with each packet.
 * - Bytes moved: an OUT part moves whole packets, counted by PKTCNT; an IN
 *   part's bytes come from XFRSIZ (u2ps_dwc2_hctsiz_in_len()). An IN part
 *   ends early at a short packet, and so does the data stage. */

/* OTG_HCINTx bits beyond those of u2ps_dwc2_hiso.h (73.14.33) */
#define U2PS_DWC2_HCINT_STALL (1u << 3)
#define U2PS_DWC2_HCINT_NAK   (1u << 4)
#define U2PS_DWC2_HCINT_DTERR (1u << 10)

/* Transaction errors in a row that end the transfer (USB 2.0 10.2.6):
 * the first U2PS_DWC2_CTL_STRIKES - 1 are re-armed */
#define U2PS_DWC2_CTL_STRIKES 3u

/* The most packets of one channel transfer: OTG_HCTSIZx.PKTCNT has 10 bits */
#define U2PS_DWC2_PKTCNT_MAX 1023u

/* How a control transfer ended; the order of u2ps_hport_control_t */
typedef enum {
  U2PS_DWC2_CTL_OK,
  U2PS_DWC2_CTL_STALL,
  U2PS_DWC2_CTL_ERROR,
  U2PS_DWC2_CTL_ABORTED
} u2ps_dwc2_ctl_result_t;

/* What the handler does next */
typedef enum {
  U2PS_DWC2_CTL_NONE, /* nothing */
  U2PS_DWC2_CTL_ARM,  /* arm a channel transfer */
  U2PS_DWC2_CTL_DONE  /* the transfer ended: on_control(result, len) */
} u2ps_dwc2_ctl_do_t;

typedef struct {
  uint8_t what;    /* u2ps_dwc2_ctl_do_t */
  uint8_t stage;   /* ARM: u2ps_dwc2_stage_t */
  bool in;         /* ARM: an IN channel transfer (OTG_HCCHARx.EPDIR) */
  uint32_t hctsiz; /* ARM: OTG_HCTSIZx */
  uint16_t offset; /* ARM, data stage: the part's start in the data (OTG_HCDMAx) */
  uint8_t result;  /* DONE: u2ps_dwc2_ctl_result_t */
  uint16_t len;    /* DONE: bytes moved in the data stage */
} u2ps_dwc2_ctl_step_t;

typedef struct {
  bool busy;       /* a channel transfer is on the channel */
  bool setup_only; /* the transfer ends after its SETUP stage */
  bool held;       /* that SETUP stage completed: u2ps_dwc2_ctl_data() goes on */
  bool aborting;   /* u2ps_dwc2_ctl_abort() came during the transfer */
  bool data_in;    /* the data stage is IN (bmRequestType D7) */
  uint8_t stage;   /* u2ps_dwc2_stage_t on the channel */
  uint8_t mps0;
  uint8_t errors;  /* transaction errors in a row */
  uint16_t length; /* wLength */
  uint16_t offset; /* bytes of the data stage moved before the part on the channel */
  uint16_t armed;  /* bytes of the part on the channel */
  /* Counters; they never reset */
  uint32_t retries; /* channel transfers re-armed after a transaction error or DTERR */
  uint32_t naks;    /* channel transfers re-armed after a halt with NAK alone */
  uint32_t faults;  /* AHB errors, and calls the hardware cannot cause */
} u2ps_dwc2_ctl_t;

/* A control transfer whose SETUP packet is 'setup' (wLength and the
 * direction are taken from it), with endpoint 0 packet size mps0 (8, 16,
 * 32 or 64); setup_only: the transfer ends after its SETUP stage
 * (u2ps_hport_control_setup). *s: arm the SETUP stage. With a channel
 * transfer still on the channel it counts in faults and does nothing. */
void u2ps_dwc2_ctl_start(u2ps_dwc2_ctl_t *c, const uint8_t setup[8], uint8_t mps0, bool setup_only,
                         u2ps_dwc2_ctl_step_t *s);

/* The data and status stages after a SETUP stage alone
 * (u2ps_hport_control_data). Without one held: DONE with ERROR, counted
 * in faults. */
void u2ps_dwc2_ctl_data(u2ps_dwc2_ctl_t *c, u2ps_dwc2_ctl_step_t *s);

/* u2ps_hport_control_abort(): true if a channel transfer is on the
 * channel, which the port then disables (CHDIS, p. 3404); its CHH ends the
 * transfer ABORTED, unless the transfer ended first (the status stage, or
 * the SETUP stage alone, completed; a STALL). False with nothing on the
 * channel: nothing follows. */
bool u2ps_dwc2_ctl_abort(u2ps_dwc2_ctl_t *c);

/* At the control channel's CHH: OTG_HCINTx and OTG_HCTSIZx read then. *s:
 * the next part or stage to arm, or the end. With nothing on the channel:
 * NONE, counted in faults. */
void u2ps_dwc2_ctl_chh(u2ps_dwc2_ctl_t *c, uint32_t hcint, uint32_t hctsiz,
                       u2ps_dwc2_ctl_step_t *s);

/* The transfer is dropped without an end (disable, disconnect); the
 * counters stay */
void u2ps_dwc2_ctl_drop(u2ps_dwc2_ctl_t *c);

#endif /* U2PS_DWC2_HCTL_H */
