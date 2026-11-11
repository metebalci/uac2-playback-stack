/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_hiso.h: the DWC2 host port's decision logic for its stream:
 * the isochronous OUT data channel, the isochronous IN feedback channel,
 * and the FIFO RAM sizes in host mode. Pure functions on plain state, with
 * no register access: the port's handlers read the registers, call these
 * functions and write what they return. They build on the PC and on ARM.
 *
 * Manual: RM0456 Rev 7 (STM32U5), chapter 73.
 * - OTG_HFNUM.FRNUM, bits 15:0: "increments when a new SOF is transmitted
 *   on the USB, and is cleared to 0 when it reaches 0x3FFF" (73.14.26,
 *   p. 3355). At high speed it is taken to count microframes modulo 2^14,
 *   as u2ps_uframe_t does (inferred; not yet measured on a board). Every
 *   frnum argument below is that field; bits above 13 are ignored.
 * - OTG_HCCHARx.ODDFRM, bit 29: the (micro)frame parity in which the host
 *   performs a periodic transfer (73.14.30, p. 3360). An isochronous OUT
 *   channel fetches its packet when enabled and sends the OUT token "at the
 *   beginning of the next (odd) frame/micro-frame"; an IN channel likewise
 *   sends its IN token (73.15.5, pp. 3431-3433, Figures 939 and 940). The
 *   host runs the periodic transactions first in each (micro)frame (73.7.3,
 *   p. 3304), so a channel armed during microframe M for parity(M + 1)
 *   transfers in M + 1, and one armed during its target microframe is late
 *   (inferred; the behaviour of an arm inside its target microframe is
 *   not yet measured on a board).
 * - OTG_HCINTx (73.14.33, pp. 3362-3363): XFRC, CHH, AHBERR (an AHB error
 *   of the DMA), TXERR (CRC, timeout, bit stuff, false EOP), BBERR, FRMOR
 *   (frame overrun: the transfer did not happen in its (micro)frame; the
 *   manual's isochronous samples disable the channel on it, p. 3423).
 * Here a parity is a bool 'odd': true for an odd microframe (ODDFRM = 1).
 *
 * Microframe distances are taken modulo 2^14 and signed: a microframe more
 * than 8191 ahead counts as behind. The feedback period is therefore
 * limited to 2^12 microframes (bInterval 13).
 */
#ifndef U2PS_DWC2_HISO_H
#define U2PS_DWC2_HISO_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_uframe.h"

/* OTG_HCINTx bits (RM0456 73.14.33) */
#define U2PS_DWC2_HCINT_XFRC   (1u << 0)
#define U2PS_DWC2_HCINT_CHH    (1u << 1)
#define U2PS_DWC2_HCINT_AHBERR (1u << 2)
#define U2PS_DWC2_HCINT_TXERR  (1u << 7)
#define U2PS_DWC2_HCINT_BBERR  (1u << 8)
#define U2PS_DWC2_HCINT_FRMOR  (1u << 9)

/* Feedback bInterval limits of this port: a period of 2^(bInterval-1)
 * microframes within the signed reach of the microframe arithmetic */
#define U2PS_DWC2_HFB_INTERVAL_MAX 13u

/* The largest feedback packet the port takes, the size of its feedback
 * buffer in port memory: a high-speed feedback value has 4 bytes (USB 2.0
 * 5.12.4.2), a wMaxPacketSize up to 64 is taken (choice) */
#define U2PS_DWC2_HFB_MPS_MAX 64u

/* Whether a stream fits the port: data endpoint 1..15 with mps 1..1024 (one
 * transaction per microframe); feedback endpoint 0 (none, then fb_interval
 * and fb_mps are not looked at) or 1..15 with bInterval 1..13 and fb_mps
 * 4..U2PS_DWC2_HFB_MPS_MAX. u2ps_hport_stream_open() refuses another. */
bool u2ps_dwc2_hstream_fits(uint8_t ep_out, uint16_t mps, uint8_t ep_fb, uint8_t fb_interval,
                            uint16_t fb_mps);

/* ---- Microframes ----------------------------------------------------------- */

/* The latest microframe of parity 'odd' at or before FRNUM: fn - ((fn ^
 * odd) & 1), modulo 2^14. Read at a channel's CHH that the handler serves
 * in the microframe of the transfer or the next one, it is the transfer's
 * microframe; served two microframes late it is two after it, and a read
 * taken before the transfer (at the entry of an interrupt that began in
 * the microframe before) two before it. */
u2ps_uframe_t u2ps_dwc2_hframe(uint32_t frnum, bool odd);

/* ---- Isochronous OUT data channel -------------------------------------------
 *
 * One channel, one packet armed at a time (PKTCNT 1, XFRSIZ the packet's
 * length, HCDMA its address), from the port's queue. The handler's
 * sequence, OTG_HCINTx read before OTG_HFNUM:
 *
 * At the data channel's CHH:
 *   1. u2ps_dwc2_hout_chh() with OTG_HCINTx and FRNUM read at that point,
 *      not at the interrupt's entry, and reread false. If it returns false,
 *      the read lies before the armed microframe: read FRNUM once more and
 *      call it again with reread true.
 *   2. If the report's 'again' is set (FRMOR), arm the same packet: step
 *      3. Otherwise, if the queue holds another packet, arm it: step 3; if
 *      not, u2ps_dwc2_hout_dry().
 *   3. u2ps_dwc2_hout_arm() with FRNUM read immediately before the
 *      OTG_HCCHARx write (CHENA and ODDFRM as it returns). Steps 2 and 3
 *      run without preemption.
 *   4. Upcalls, in bus order, if the report's 'sent': on_missed for
 *      no_buffer (NO_BUFFER) and late (LATE), on_missed(1, CORRUPTED) if
 *      corrupted, then on_sent(uframe).
 * At u2ps_hport_queue() with no packet armed (the stream's first packet,
 * or the queue ran dry): u2ps_dwc2_hout_arm() as in step 3, with the
 * vectors masked.
 */

/* Where the data channel stands */
typedef enum {
  U2PS_DWC2_HOUT_CLOSED,    /* no stream */
  U2PS_DWC2_HOUT_OPEN,      /* open, no packet yet */
  U2PS_DWC2_HOUT_ARMED,     /* a packet armed for 'target' */
  U2PS_DWC2_HOUT_COMPLETED, /* its CHH has come; the next arm decides */
  U2PS_DWC2_HOUT_AGAIN,     /* FRMOR: the packet goes again at the next arm */
  U2PS_DWC2_HOUT_DRY        /* the queue was empty at the last CHH */
} u2ps_dwc2_hout_state_t;

typedef struct {
  uint8_t state;         /* u2ps_dwc2_hout_state_t */
  bool started;          /* a packet has been sent since the open */
  bool odd;              /* the armed parity */
  u2ps_uframe_t target;  /* the microframe the armed packet goes out in */
  u2ps_uframe_t last;    /* the microframe of the last packet sent */
  uint16_t no_buffer;    /* NO_BUFFER microframes before the armed packet */
  uint16_t late;         /* LATE microframes before the armed packet */
  /* Counters of u2ps_hport_stats_t */
  uint32_t sent;             /* packets sent, CORRUPTED ones included */
  uint32_t late_completions; /* CHHs whose next arm read a microframe after the packet's */
  uint32_t late_total;       /* LATE microframes */
  uint32_t no_buffer_total;  /* NO_BUFFER microframes */
  uint32_t corrupted;        /* packets the DMA could not fetch (AHBERR) */
  uint32_t errors;           /* AHB errors, and states the hardware cannot produce */
} u2ps_dwc2_hout_t;

/* What a CHH gives the upcalls */
typedef struct {
  bool sent;            /* the packet is done: on_sent(uframe) */
  bool again;           /* not sent (FRMOR): the same packet is armed again */
  bool corrupted;       /* on_missed(1, CORRUPTED) before its on_sent */
  u2ps_uframe_t uframe; /* its microframe */
  uint16_t no_buffer;   /* NO_BUFFER microframes before it */
  uint16_t late;        /* LATE microframes before it */
} u2ps_dwc2_hout_report_t;

/* Opens the stream: nothing armed until the first packet. The counters
 * are kept: zero the struct once, before the first open. */
void u2ps_dwc2_hout_open(u2ps_dwc2_hout_t *o);

/* The stream has ended (close, disable, disconnect); the counters stay. */
void u2ps_dwc2_hout_close(u2ps_dwc2_hout_t *o);

/* The arm decision for the packet at the queue's head, with frnum read
 * immediately before the OTG_HCCHARx write; returns the parity to arm, and
 * the microframe it goes out in is o->target. The target, and what passes
 * without a packet:
 * - the stream's first packet: now + 1; nothing is a hole;
 * - after a CHH (COMPLETED): last + 1 if now is still the last packet's
 *   microframe; otherwise the arm is late (the passed-target check): the
 *   microframes last + 1..now are LATE, and the target is now + 1. A
 *   CHH whose arm reads a later microframe counts a late completion;
 * - after FRMOR (AGAIN): the packet missed its target; the microframes
 *   from that target to now are LATE, and the target is now + 1;
 * - after the queue ran dry (DRY): the microframes last + 1..now are
 *   NO_BUFFER, and the target is now + 1.
 * The holes are kept for the packet's CHH report. In any other state it
 * counts in errors and arms for now + 1. */
bool u2ps_dwc2_hout_arm(u2ps_dwc2_hout_t *o, uint32_t frnum);

/* At a CHH with no packet left to arm (COMPLETED becomes DRY) */
void u2ps_dwc2_hout_dry(u2ps_dwc2_hout_t *o);

/* At the data channel's CHH, with OTG_HCINTx in hcint and frnum read after
 * it. The packet's microframe is its target; FRNUM, with the armed parity,
 * must not lie before it (u2ps_dwc2_hframe): a read before it (taken at
 * the interrupt's entry) returns false and changes nothing if reread is
 * false; with reread true it counts in errors and the target stands. The
 * CHH may be served 0, 1, 2 or more microframes after the target.
 * - XFRC: sent, with the holes recorded at its arm.
 * - AHBERR: the packet did not cross the bus intact: sent, corrupted (on
 *   DWC2 its DMA fetch failed), counted in corrupted and errors.
 * - FRMOR without XFRC: not sent; 'again', and the next arm reports the
 *   LATE microframes.
 * In any state but ARMED it counts in errors and reports nothing. */
bool u2ps_dwc2_hout_chh(u2ps_dwc2_hout_t *o, uint32_t hcint, uint32_t frnum, bool reread,
                        u2ps_dwc2_hout_report_t *r);

/* ---- Isochronous IN feedback channel -----------------------------------------
 *
 * One poll armed at a time, in poll microframes Q, Q + period, ... that the
 * port chooses at the open. An enabled periodic channel sends its token in
 * the next (micro)frame of its ODDFRM parity, whatever the endpoint's
 * interval (73.14.31, p. 3360; 73.15.5, pp. 3431-3433), so a poll is armed
 * only in the microframe right before it: an arm earlier than that would
 * poll in the first microframe of the poll's parity, before the poll. A
 * poll whose microframe has begun before its arm is skipped, not shifted:
 * the next poll stays one period later.
 * The handler's sequence: at the feedback channel's CHH,
 * u2ps_dwc2_hfb_chh() (re-read on false); upcalls on_fb or on_fb_missed
 * for the poll. At every clock of the stream with the feedback channel
 * idle (the data channel's CHH, the feedback channel's CHH, a queue call),
 * u2ps_dwc2_hfb_arm() with FRNUM read immediately before the OTG_HCCHARx
 * write it may make, CHENA and ODDFRM as it returns if it armed (state
 * ARMED); then on_fb_missed(..., LATE) for each poll it skipped. Polls
 * therefore need a clock in the microframe before each of them: the data
 * channel's CHH while packets flow, and the port's SOF interrupt while the
 * data channel is idle (u2ps_dwc2_hcd.c).
 */

typedef enum {
  U2PS_DWC2_HFB_CLOSED, /* no feedback channel */
  U2PS_DWC2_HFB_IDLE,   /* open; the next arm decides */
  U2PS_DWC2_HFB_ARMED   /* a poll armed for 'poll' */
} u2ps_dwc2_hfb_state_t;

typedef struct {
  uint8_t state;      /* u2ps_dwc2_hfb_state_t */
  bool odd;           /* the armed parity */
  uint16_t period;    /* microframes between polls */
  u2ps_uframe_t poll; /* the next (or armed) poll's microframe */
  /* Counters of u2ps_hport_stats_t */
  uint32_t polls;     /* polls made */
  uint32_t missed;    /* polls without a valid packet, or skipped */
  uint32_t errors;    /* babble, and states the hardware cannot produce */
} u2ps_dwc2_hfb_t;

/* What a feedback poll's CHH gives */
typedef enum {
  U2PS_DWC2_HFB_DATA,      /* on_fb(data, len, uframe) */
  U2PS_DWC2_HFB_NO_PACKET, /* on_fb_missed(uframe, NO_PACKET): TXERR or babble */
  U2PS_DWC2_HFB_LATE,      /* on_fb_missed(uframe, LATE): FRMOR, the poll did not happen */
  U2PS_DWC2_HFB_NONE       /* nothing to report */
} u2ps_dwc2_hfb_result_t;

/* Opens the feedback channel with bInterval 'interval' (1..13): the first
 * poll in microframe now + 1, frnum read at the open. False, with nothing
 * open, for another interval. Counters kept, as above. */
bool u2ps_dwc2_hfb_open(u2ps_dwc2_hfb_t *f, uint8_t interval, uint32_t frnum);

void u2ps_dwc2_hfb_close(u2ps_dwc2_hfb_t *f);

/* The arm decision for the next poll, with frnum read immediately before
 * the OTG_HCCHARx write, in IDLE. If now is the microframe right before
 * the poll's, the poll is armed with parity(poll), *odd, the state becomes
 * ARMED, and 0 is returned. If now lies further before it, nothing is
 * armed yet (IDLE, *odd unchanged) and 0 is returned. Otherwise the poll's
 * microframe has begun: the poll is skipped, and so is every later one up
 * to now; the first poll after now is next, armed now if now is right
 * before it; the number skipped is returned, the first of them in
 * *skipped (each skipped poll gives on_fb_missed(LATE), one period apart).
 * In CLOSED or ARMED it returns 0 and arms nothing (*odd unchanged). */
uint16_t u2ps_dwc2_hfb_arm(u2ps_dwc2_hfb_t *f, uint32_t frnum, bool *odd,
                           u2ps_uframe_t *skipped);

/* At the feedback channel's CHH, with OTG_HCINTx in hcint and frnum read
 * after it; the stale-read rule of u2ps_dwc2_hout_chh(). The poll's
 * microframe is *uframe; the next poll is one period later, armed by
 * u2ps_dwc2_hfb_arm(). XFRC gives DATA; TXERR NO_PACKET; BBERR NO_PACKET
 * and counts in errors; FRMOR LATE. In any state but ARMED: NONE, counted
 * in errors. */
bool u2ps_dwc2_hfb_chh(u2ps_dwc2_hfb_t *f, uint32_t hcint, uint32_t frnum, bool reread,
                       u2ps_dwc2_hfb_result_t *result, u2ps_uframe_t *uframe);

/* ---- FIFO RAM in host mode ----------------------------------------------------
 *
 * RM0456 73.11.3, p. 3311, host mode:
 * - Rx FIFO: (largest packet / 4) + 1 for status, twice, as "typically
 *   ... recommended" so that the USB can receive a packet while the
 *   previous one is moved, + 1 for transfer complete; the largest IN
 *   packet is EP0's 64 bytes or the feedback's.
 * - Non-periodic Tx FIFO: the largest non-periodic OUT packet, EP0's 64
 *   bytes.
 * - Periodic Tx FIFO: with an isochronous OUT channel, at least twice its
 *   maximum packet size.
 * Each Tx FIFO at least 16 words (OTG_HNPTXFSIZ, OTG_HPTXFSIZ minimum
 * value, 73.14.15 and 73.14.21); the RAM holds 1024 words (73.11, p. 3307).
 * Quotients round up. The Rx FIFO starts at word 0, the non-periodic Tx
 * FIFO after it, the periodic Tx FIFO after that. */
typedef struct {
  uint16_t rx;         /* OTG_GRXFSIZ.RXFD */
  uint16_t nptx;       /* OTG_HNPTXFSIZ.NPTXFD */
  uint16_t ptx;        /* OTG_HPTXFSIZ.PTXFSIZ */
  uint16_t nptx_start; /* word offset of the non-periodic Tx FIFO */
  uint16_t ptx_start;  /* word offset of the periodic Tx FIFO */
  uint16_t total;      /* words used */
} u2ps_dwc2_hfifo_t;

/* The sizes for an isochronous OUT channel of out_mps bytes (1..1024) and
 * a feedback channel of fb_mps bytes (0 for none, else 4..1024). Returns
 * false if they do not fit; *f is filled in either way. */
bool u2ps_dwc2_hfifo_plan(uint16_t out_mps, uint16_t fb_mps, u2ps_dwc2_hfifo_t *f);

#endif /* U2PS_DWC2_HISO_H */
