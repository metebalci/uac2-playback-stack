/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_iso.h: the DWC2 device port's decision logic for its stream,
 * the isochronous OUT data endpoint and its isochronous IN feedback
 * endpoint. Pure functions on plain state, with no register access: the
 * port's handlers read the registers, call these functions and write what
 * they return. They build on the PC and on ARM.
 *
 * Manuals: RM0431 (STM32F72x/F73x; HTML copy, probably Rev 4, without page
 * numbers, cited by section) and RM0456 Rev 7 (STM32U5), chapter 73.
 * - OTG_DSTS.FNSOF[13:0], bits 21:8: the frame number of the received SOF
 *   (RM0431 32.15.37; RM0456 73.14.40, p. 3369). At high speed it is taken
 *   to count microframes as u2ps_uframe_t does, frame << 3 | microframe;
 *   neither manual states that layout (inferred; the measured rule works
 *   with it). Every fnsof argument below is that field, shifted down, not
 *   the raw OTG_DSTS value.
 * - EONUM, bit 16 of OTG_DOEPCTLx and OTG_DIEPCTLx, set with SODDFRM and
 *   SEVNFRM: the (micro)frame parity in which the core receives or sends
 *   isochronous data (RM0431 32.15.61 and 32.15.51; RM0456 73.14.59, p. 3391,
 *   and 73.14.48, p. 3378). An isochronous OUT packet is received only if
 *   EONUM = FNSOF[0] (RM0431 32.16.6, "Generic isochronous OUT data
 *   transfer", internal data flow 2; RM0456 73.15.6, p. 3445). EONUM only
 *   holds what the application programmed, so the logic keeps the parity it
 *   armed instead of reading it back.
 * Here a parity is a bool 'odd': true for odd (SODDFRM), false for even
 * (SEVNFRM).
 *
 * Microframe distances are taken modulo 2^14. A microframe counts as
 * behind another only if it lies 0, 1 or 2 microframes before or on it
 * ((x - ref) mod 2^14 is 0, 0x3FFE or 0x3FFF); every other distance is a
 * forward gap. So a host pause of up to 2.048 s minus three microframes is
 * counted as a gap; a longer one aliases to its length modulo 2.048 s. A
 * hold of buffers by the device stack does not alias: every packet the
 * host sends meanwhile is a reserve packet (below), whose microframe is
 * the base of the next distance.
 *
 * The functions of the packet path are in section .text.u2ps_fast
 * (U2PS_FAST_TEXT, u2ps_types.h); the small ones it calls for every packet
 * or every release are static inline here.
 */
#ifndef U2PS_DWC2_ISO_H
#define U2PS_DWC2_ISO_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_types.h"
#include "u2ps_uframe.h"

/* No buffer: returned where a buffer index is expected */
#define U2PS_DWC2_NO_BUF 0xFFFFu

/* The buffers of the stream the port keeps armed, its
 * U2PS_DPORT_ISO_OUT_RESERVE: one, the buffer armed. After a completion
 * the core sets the endpoint's NAK, and while it is set isochronous OUT
 * packets "are ignored and not written to the receive FIFO" (RM0456
 * 73.15.6, p. 3444, internal data flow 3; RM0431 32.16.6 the same); the
 * core drops them without an interrupt (RM0456 73.6.2, p. 3300). Only an
 * armed buffer sees the host's packets, so the port never leaves the
 * endpoint without one, and the device stack holds at most count - 1. */
#define U2PS_DWC2_OUT_RESERVE 1u

/* Feedback bInterval limits of this port. The core sends isochronous IN
 * data in every microframe of the armed parity and has no interval
 * setting, so only a period of 2 or more microframes keeps every poll on
 * one parity (RM0431 32.15.51; RM0456 73.14.48, p. 3378). The upper limit
 * keeps the period, 2^(bInterval-1) microframes, within the signed reach of
 * the 14-bit microframe arithmetic (4096 < 8192). */
#define U2PS_DWC2_FB_INTERVAL_MIN 2u
#define U2PS_DWC2_FB_INTERVAL_MAX 13u

/* ---- Packet sizes ---------------------------------------------------------- */

/* The transfer size (OTG_DOEPTSIZx.XFRSIZ) for one isochronous OUT packet
 * of at most mps bytes: mps rounded up to a whole word. The manuals require
 * the OUT transfer size to be "adjusted to the word boundary" (RM0431
 * 32.16.6, "Generic non-isochronous OUT data transfers", application
 * requirement 2, which applies to isochronous OUT as well; RM0456 73.15.6,
 * p. 3443). Their formula, MPSIZ + 4 - (MPSIZ mod 4), equals this for an
 * mps that is not a multiple of 4 and gives mps + 4 otherwise. With mps + 4
 * a full packet would leave XFRSIZ 4 and PKTCNT 0, which meets neither
 * completion condition of RM0456 p. 3444 (internal data flow 6); ceil4(mps)
 * leaves 0 and 0 (inferred). Class memory strides hold it
 * (U2PS_DPORT_STRIDE). */
uint16_t u2ps_dwc2_out_xfrsiz(uint16_t mps);

/* ---- Class memory ---------------------------------------------------------- */

/* Why u2ps_dport_stream_open() refused a stream: the cause behind the
 * device stack's U2PS_FAULT_OPEN on this port (u2ps_dwc2_dcd_open_error),
 * with the fix in u2ps_stream_cfg_t (u2ps_dev.h) */
typedef enum {
  U2PS_DWC2_OPEN_OK = 0,
  /* count outside U2PS_DPORT_ISO_OUT_MIN..U2PS_ISO_OUT_COUNT_MAX: lend at
   * least 2 buffers, and no more than the library is built for */
  U2PS_DWC2_OPEN_COUNT,
  U2PS_DWC2_OPEN_MEM_NULL,     /* mem is NULL: give the class memory */
  /* stride below the transfer size, max_packet_size rounded up to a word:
   * use at least U2PS_DPORT_STRIDE(info->max_packet_size) */
  U2PS_DWC2_OPEN_STRIDE_SHORT,
  /* stride not a multiple of U2PS_DPORT_ALIGN (32 on the STM32F723, 4 on
   * the STM32U5): round it up to one */
  U2PS_DWC2_OPEN_STRIDE_ALIGN,
  /* mem not on U2PS_DPORT_ALIGN: declare the class memory
   * _Alignas(U2PS_DPORT_ALIGN) */
  U2PS_DWC2_OPEN_MEM_ALIGN,
  /* count * stride bytes from mem do not all lie in memory the OTG_HS DMA
   * reaches, or that memory's clock is off or it is powered down
   * (u2ps_dwc2_dma_ok, u2ps_dwc2.h): place the class memory in SRAM1,
   * SRAM2, SRAM3, SRAM5 or SRAM6 at 0x2000_0000 on the STM32U5 (not SRAM4),
   * with the SRAM's RCC clock enable set and PWR_CR1.SRAMxPD clear, as at
   * reset; in DTCM (RCC_AHB1ENR.DTCMRAMEN set), SRAM1 or SRAM2 on the
   * STM32F723 */
  U2PS_DWC2_OPEN_NOT_DMA,
  /* the stream's other fields (endpoints, mps, feedback), or a stream
   * still open or closing: the device stack's, not the app's; report it */
  U2PS_DWC2_OPEN_STREAM
} u2ps_dwc2_open_err_t;

/* The class memory checks of u2ps_dport_stream_open(), in this order:
 * count_min <= count <= count_max, mem not 0, stride at least
 * u2ps_dwc2_out_xfrsiz(mps), stride and mem multiples of 'align' (a power
 * of 2, U2PS_DPORT_ALIGN). Whether the memory is DMA memory is the
 * caller's check. */
u2ps_dwc2_open_err_t u2ps_dwc2_mem_check(uintptr_t mem, uint16_t stride, uint16_t count,
                                         uint16_t mps, uint16_t count_min, uint16_t count_max,
                                         uint16_t align);

/* ---- Microframes ----------------------------------------------------------- */

/* The microframe of a transfer the core completed in a microframe of
 * parity 'odd', from FNSOF read at the completion (bits above 13 are
 * ignored): fn - ((fn ^ odd) & 1), modulo 2^14. Read after the completion,
 * fn is that microframe or the next one, so the result is exact; a value
 * read before it (an interrupt that entered in the microframe before the
 * packet's and served its completion in the same pass) gives two
 * microframes too early. Used for the data packet's microframe P and the
 * feedback endpoint's poll microframe Q (a measured rule). */
u2ps_uframe_t u2ps_dwc2_uframe(uint32_t fnsof, bool odd);

/* The parity to arm and the microframes lost, from the B-next rule */
typedef struct {
  bool odd;      /* the parity to arm the endpoint for */
  uint16_t late; /* LATE microframes, P+1..now; 0 if the arm is in time for P+1 */
} u2ps_dwc2_bnext_t;

/* B-next: the parity of the re-arm after the packet of microframe p, with
 * 'now' the microframe read immediately before the OTG_DOEPCTLx write that
 * enables the endpoint (a measured rule beyond the manuals' enable window,
 * RM0456 73.15.6, p. 3445):
 * - (now - p) mod 2^14 <= 1: parity(p + 1). With now = p + 1 the arm falls
 *   inside the target microframe and works if its token has not come yet.
 * - otherwise: parity(now + 1), and the microframes p+1..now, (now - p)
 *   mod 2^14 of them, are LATE.
 * The stream logic below never passes a 'now' behind p.
 * An injection build may be built with U2PS_DWC2_OLD_RULE = 1, for a
 * comparison: then every arm is for parity(now + 1), and p+1..now are
 * LATE. */
u2ps_dwc2_bnext_t u2ps_dwc2_bnext(u2ps_uframe_t p, u2ps_uframe_t now);

/* ---- Isochronous OUT stream ------------------------------------------------
 *
 * One buffer of class memory is armed at a time (PKTCNT = 1, XFRSIZ =
 * u2ps_dwc2_out_xfrsiz(mps)); the others are free or held by the device
 * stack, which releases them in arrival order. The reserve: a buffer is
 * armed whenever the stream is open (U2PS_DWC2_OUT_RESERVE). A completed
 * packet is delivered only if a free buffer is left to arm after it, that
 * is while the device stack holds fewer than count - 1; otherwise it is a
 * reserve packet: counted NO_BUFFER in its own microframe, not delivered,
 * and its buffer armed again (its data is overwritten unread; nothing is
 * copied). So every packet the host sends is delivered or counted, and a
 * host pause inside a hold stays NO_PACKET. The handler's sequence, with
 * the data endpoint served first in each pass and OTG_DOEPINTx read before
 * OTG_DSTS:
 *
 * At OTG_DOEPINTx.XFRC of the data endpoint:
 *   1. If the packet is not valid (OTG_DOEPTSIZx.RXDPID other than DATA0
 *      with one packet received, RM0431 32.16.6 and RM0456 73.15.6,
 *      p. 3446, application step 6): u2ps_dwc2_out_drop() with FNSOF
 *      read at that point; it gives the buffer to arm again, the same one.
 *      Otherwise u2ps_dwc2_out_complete() with FNSOF read at that point,
 *      not at interrupt entry, and reread = false. If it returns false, the
 *      packet's microframe does not lie after the last packet's: read FNSOF
 *      once more and call it again with reread = true.
 *   2. If it gave a buffer to arm, write OTG_DOEPDMAx and OTG_DOEPTSIZx for
 *      it: also the same buffer again, since the core advanced OTG_DOEPDMAx
 *      past the packet (RM0456 73.14.57, p. 3387).
 *   3. u2ps_dwc2_out_arm() with FNSOF read immediately before the
 *      OTG_DOEPCTLx write; if it returns true, write OTG_DOEPCTLx with
 *      EPENA, CNAK and the parity. Steps 2 and 3 run without preemption.
 *   4. Upcalls, after step 3, in bus order: for a packet to deliver
 *      (buf not U2PS_DWC2_NO_BUF), on_missed for late, no_buffer and
 *      no_packet of the packet record, then on_packet (buffer buf,
 *      microframe uframe), then on_missed for the LATE microframes that
 *      step 3 returned. A reserve packet (buf U2PS_DWC2_NO_BUF, arm_buf
 *      set) has no upcall; it is data for the feedback's rule
 *      (u2ps_dwc2_fb_data).
 * At INCOMPISOOUT, served after the data endpoint's XFRC and EPDISD of the
 * same pass: u2ps_dwc2_out_incomplete(), and if it returns true, the
 * parity written to OTG_DOEPCTLx (SODDFRM or SEVNFRM only), then
 * u2ps_dwc2_out_eopf_after(), and EOPF unmasked for the follow.
 * At u2ps_dport_release(), in any context: u2ps_dwc2_out_release(), and no
 * register write, since a buffer is always armed and a freed one is armed
 * at the next completion.
 * At a close: u2ps_dwc2_out_closing() when the close begins (SGONAK);
 * completions until the endpoint is disabled are dropped and nothing is
 * armed; u2ps_dwc2_out_close() when it has ended. At the stream's end
 * (before on_closed, before on_reset, and inside connect(false)), and in
 * thread at u2ps_dport_report_holes() while the stream is open:
 * u2ps_dwc2_out_tail(), whose holes the port reports with on_missed.
 * A count of microframes above 65,535 is reported in several on_missed
 * calls of at most 65,535 each.
 */

/* Where the stream's logic stands */
typedef enum {
  U2PS_DWC2_OUT_CLOSED,    /* no stream */
  U2PS_DWC2_OUT_ARMED,     /* one buffer armed; waiting for its XFRC */
  U2PS_DWC2_OUT_COMPLETED, /* a packet completed; u2ps_dwc2_out_arm() decides */
  U2PS_DWC2_OUT_DROPPED,   /* a packet was dropped, or was a reserve packet;
                              u2ps_dwc2_out_arm() re-arms its buffer */
  U2PS_DWC2_OUT_CLOSING    /* a close runs: completions drop, nothing is armed */
} u2ps_dwc2_out_state_t;

typedef struct {
  uint8_t state;         /* u2ps_dwc2_out_state_t */
  bool started;          /* a packet has come since the open */
  bool armed_odd;        /* the parity the armed buffer was armed for */
  bool following;        /* the parity follows the bus until the next XFRC */
  u2ps_uframe_t follow_fn; /* microframe of the last follow's EOPF */
  bool gap_seen;         /* an INCOMPISOOUT of the armed parity since the last XFRC */
  u2ps_uframe_t gap_fn;  /* its microframe */
  uint16_t count;        /* buffers of the stream */
  uint16_t head;         /* the oldest held buffer */
  uint16_t held;         /* packets delivered and not released */
  u2ps_uframe_t last;    /* microframe of the last packet, P, delivered or reserve */
  u2ps_uframe_t ref;     /* microframe of the last completion or drop: the arm's base */
  u2ps_uframe_t seen;    /* microframes last+1..seen are reported or recorded */
  uint32_t late;         /* LATE recorded for the next packet record */
  uint32_t no_buffer;    /* NO_BUFFER recorded likewise (reserve packets) */
  uint32_t no_packet;    /* NO_PACKET recorded likewise (before reserve packets, and a
                            dropped packet's hole) */
  /* Counters of u2ps_dport_stats_t */
  uint32_t late_completions; /* completions whose arm decision read a microframe after P */
  uint32_t late_total;       /* LATE microframes */
  uint32_t no_buffer_total;  /* NO_BUFFER microframes: reserve packets */
  uint32_t errors;           /* states the hardware cannot produce, invalid packets, bad releases */
  uint32_t close_drops;      /* packets that landed during a close, dropped */
  uint32_t follows;          /* parity writes of the follow (u2ps_dwc2_out_eopf) */
  uint32_t follow_late;      /* follow writes that landed once their microframe had begun,
                                EOPF passes of a follow late or skipped, and gap evidence
                                not two microframes apart: a packet may have been lost */
  uint32_t gap_follows;      /* follows started in a gap (u2ps_dwc2_out_incomplete) */
  uint32_t incomp_other;     /* INCOMPISOOUT of the other parity, idle: nothing done */
  uint32_t gap_cleared;      /* gap evidence that an XFRC (completion or drop) cleared */
  uint32_t gap_own;          /* of those, the XFRC of the evidence's own microframe */
} u2ps_dwc2_out_t;

/* One completed packet and what to report before it */
typedef struct {
  uint16_t buf;         /* the packet's buffer, or U2PS_DWC2_NO_BUF: nothing to deliver */
  u2ps_uframe_t uframe; /* its microframe P */
  uint32_t late;        /* LATE microframes recorded */
  uint32_t no_buffer;   /* NO_BUFFER microframes recorded */
  uint32_t no_packet;   /* the rest of the hole since the last packet: NO_PACKET */
  uint16_t arm_buf;     /* the buffer to arm now, or U2PS_DWC2_NO_BUF */
} u2ps_dwc2_out_packet_t;

/* Opens a stream of 'count' buffers (at least U2PS_DWC2_OUT_RESERVE + 1,
 * 2) and arms buffer 0 for parity(now + 1), with now from FNSOF read
 * immediately before the OTG_DOEPCTLx write; the parity then follows the
 * bus until the first packet (u2ps_dwc2_out_eopf), and again in a gap
 * after it (u2ps_dwc2_out_incomplete). Returns false, with nothing armed, if
 * count is lower. The counters are kept: zero the struct once, before the
 * first open. */
bool u2ps_dwc2_out_open(u2ps_dwc2_out_t *o, uint16_t count, uint32_t fnsof, bool *odd);

/* The data endpoint's parity follows the bus from the open to the first
 * packet, and from a gap's start (u2ps_dwc2_out_incomplete) to the next
 * packet, as the feedback endpoint's does to the first poll
 * (u2ps_dwc2_fb_eopf): the core receives only in microframes of the armed
 * parity (RM0456 73.15.6, p. 3445), so a parity guessed once at the open
 * would lose a first packet in a microframe of the other one. At each
 * EOPF while the buffer is armed and no XFRC has come since the follow
 * began, with fnsof read at the EOPF:
 * - true, and parity(now + 1) in *odd, which the register layer writes
 *   (SODDFRM or SEVNFRM) and with which the packet's P is then taken; the
 *   pass becomes the reference of the next;
 * - false, writing nothing, and the follow goes on, for a pass more than
 *   one microframe after the reference (a pass skipped, or this one late,
 *   so probably early in its microframe, where a write could land before
 *   or under that microframe's packet): it counts in follow_late, keeps
 *   the parity armed, and becomes the reference. A pass served after the
 *   next SOF right after it is then taken for a timely one (a known
 *   limit, README);
 * - false, and the follow ends, at the next XFRC (completion or drop) and
 *   in a close; false also while nothing is armed, and with following
 *   false (the register layer clears it to turn the follow off).
 * The register layer skips the write while EPENA is clear or NAKSTS set
 * (a packet received, still to complete with the parity it came in,
 * which its P is taken with), and in the pass that started the follow.
 * The follow has no time limit, as the feedback endpoint's has none:
 * however late the host sends the first packet, a follow that ended
 * before it would leave a guessed parity. Measured on the NUCLEO-U5A5ZJ-Q
 * from an open: the first packet was lost in 0 of 50 opens at 192 kHz
 * with it and in 25 of 50 without it (0 of 10 and 5 of 10 at 768 kHz). It
 * costs one EOPF pass of the handler per microframe until the packet
 * (U2PS_DWC2_PASS_EOPF in the pass profile; in a gap's follow, with
 * INCOMPISOOUT in the same pass, 288 cycles on the STM32F723 and 266 on
 * the STM32U5G9, measured by bit-perfect-checker on 2026-11-12). */
bool u2ps_dwc2_out_eopf(u2ps_dwc2_out_t *o, uint32_t fnsof, bool *odd);

/* After that write, or the one of u2ps_dwc2_out_incomplete(), with fnsof
 * read right after it: a write that lands once the microframe it was meant
 * for has begun (FNSOF no longer the one read before it) may have lost a
 * packet in that microframe; it counts in follow_late, so the loss is not
 * silent. */
void u2ps_dwc2_out_eopf_after(u2ps_dwc2_out_t *o, uint32_t fnsof);

/* After the first packet, the parity follows the bus again in a gap of
 * the host of three or more microframes, once INCOMPISOOUT (OTG_GINTSTS
 * bit 21) has shown the gap. Without it, the parity armed after the last
 * packet stays through the gap, and a host that resumes in a microframe of
 * the other parity loses that packet without a signal (release 0.1.0). A
 * gap of exactly two needs no write: the next microframe of the armed
 * parity is the host's.
 * - The core raises INCOMPISOOUT with EOPF when an isochronous OUT
 *   endpoint has not completed its transfer in the current microframe
 *   (RM0456 73.14.7, p. 3334; RM0431 32.15.6): no packet came, one was
 *   dropped (Rx FIFO full, CRC error, corrupted token), or the packet's
 *   data has not yet left the Rx FIFO (RM0456 73.15.6, p. 3446-3447;
 *   RM0431 32.16.6, "Incomplete isochronous OUT data transfers"). A packet
 *   that completes late raises it too: measured on DWC2 at 705.6 and
 *   768 kHz with large packets, 0.01 to 0.03 % of the packets completed
 *   117 to 121 us into their own microframe, a range around the EOPF point
 *   (PFIVL 95 %, 118.75 us), and their re-arm ran after the next SOF.
 * - INCOMPISOOUT comes only in a microframe of the armed parity: the
 *   manuals' condition (EONUM = FNSOF[0] and EPENA, RM0456 p. 3447,
 *   application step 3), and measured while the host streams:
 *   bit-perfect-checker counted no INCOMPISOOUT inside 12 streams at 705.6
 *   and 768 kHz on each of the STM32F723 and the STM32U5G9 (2026-11-12).
 *   If a core raised it at every EOPF, the models of the tests show the
 *   same results; incomp_other then counts one per packet.
 * - A packet received is still to complete while NAKSTS is set: the core
 *   sets the endpoint's NAK when PKTCNT reaches 0, as the packet is
 *   written to the Rx FIFO (RM0456 73.15.6, p. 3444, internal data flow 2
 *   to 7, which name isochronous packets, and p. 3445, isochronous flow 1;
 *   RM0431 32.16.6; the SNAK text, RM0456 73.14.59, p. 3390, has it set
 *   "on a transfer completed interrupt"). Measured with the NAKSTS
 *   probe of the injection build (u2ps_dwc2_probe, u2ps_dwc2_dcd.h) by
 *   bit-perfect-checker on the STM32F723 and the STM32U5G9 with a Linux
 *   host, 2026-11-12: in every packet probed (4,800 at 192 kHz, 4,799 at
 *   768 kHz, each core), NAKSTS was seen set before EPENA cleared, with a
 *   lead of at most 103 to 105 cycles at 192 kHz and 281 to 283 at
 *   768 kHz. Derived, not measured: since the lead grows with the packet's
 *   size, NAK comes at the end of the packet's reception and NAKSTS reads
 *   0 while the packet is on the wire (up to about 17 us); and with that
 *   lead (1.3 to 1.8 us at 768 kHz), the packets completed 117 to 121 us
 *   into their microframe entered the Rx FIFO about 115 to 120 us into
 *   it, some after the EOPF point. Not measured: other DWC2 cores.
 *   Inferred, not measured: that only the re-arm's CNAK clears it, and that a
 *   corrupted packet, flushed from the Rx FIFO without decrementing PKTCNT
 *   (p. 3444, flow 2), leaves it clear. If NAK came only at the XFRC, or
 *   never, a miss followed by a packet completing late, and a late first
 *   packet after a gap, would lose one packet more than 0.1.0 and stamp
 *   one a microframe off; the register test's model table
 *   (tests/test_dwc2_dcd.c, test_models) lists it per case.
 * At INCOMPISOOUT, with idle true only if this pass served no XFRC of the
 * endpoint and OTG_DOEPCTLx, read once, has EPENA set and NAKSTS clear,
 * and with fnsof read then: the evidence is an idle INCOMPISOOUT in a
 * microframe of the armed parity; the next one, two microframes later
 * with no XFRC between, makes the gap sure. Then returns true and
 * parity(now + 1) in *odd, which the register layer writes (SODDFRM or
 * SEVNFRM), and the follow runs at EOPF (u2ps_dwc2_out_eopf) until the next
 * XFRC, as from an open; it counts in gap_follows. Every XFRC and drop
 * clears the evidence (counted in gap_cleared if there was some, and in
 * gap_own if the XFRC is of the evidence's own microframe: a first
 * evidence only, since a second one starts the follow), and so does an
 * open. False, writing nothing, otherwise; then an idle INCOMPISOOUT of
 * the other parity counts in incomp_other (a pass served after the SOF;
 * or raised for another reason), and evidence other than two microframes
 * apart (a pass skipped or late) in follow_late.
 * Not followed, so lost as in release 0.1.0, with the hole one microframe
 * longer than the host's gap:
 * - the host's next packet after a single microframe without one, and
 *   after a packet the core drops (CRC error): the endpoint stays armed for
 *   that microframe's parity; after a drop the device misses two
 *   microframes in which the host sent;
 * - the host's next packet after a re-arm that came after its token
 *   (B-next): two microframes are lost.
 * Also lost, when an INCOMPISOOUT or EOPF pass of a gap comes after the
 * next SOF: a late first evidence (counted in incomp_other) or second one
 * (counted in incomp_other, then follow_late) delays the follow by two
 * microframes or more, and a host that resumes in the delay, in a
 * microframe of the other parity, loses that packet (a resume in the gap's
 * fourth microframe, three after its first, when the first is late); a
 * late or skipped EOPF pass of the follow leaves a microframe unarmed
 * (counted in follow_late, at that pass or at the XFRC that ends the
 * follow). A pass two microframes late can count nowhere: it is taken as
 * the evidence of its own microframe.
 * Not found (a known limit, README): a pass served after the next SOF,
 * early in a microframe for an event of an earlier one, that the
 * microframe number does not show: an EOPF pass right after a late one,
 * which became its reference (repeated, while every pass is a microframe
 * late, in every microframe); two INCOMPISOOUT passes of a gap's start,
 * each two microframes late; the first EOPF pass after an open whose
 * unmasking of EOPF came an odd number, three or more, of microframes
 * after its arm. Such a pass writes a parity before or under the host's
 * packet of its microframe.
 * Not RM0456 p. 3447, application steps 5 and 6 (discard, EPDIS, EPDISD,
 * enable again): an idle endpoint holds nothing to discard, and that
 * recovery may miss the next microframe (step 6); only the parity of the
 * armed buffer changes (inferred: neither manual restricts SODDFRM and
 * SEVNFRM on an enabled endpoint, RM0456 73.14.59, p. 3389-3391; RM0431
 * 32.15.61).
 * Not measured: whether a packet of the current microframe is on the wire
 * at its EOPF point, where every follow write reads NAKSTS. RM0456 says
 * only that at INCOMPISOOUT "no active transfers remain in progress on
 * this endpoint on the USB" (73.15.6, p. 3446-3447, "Incomplete
 * isochronous OUT data transfers", internal data flow 2); USB 2.0 lets a
 * host transmit later in the microframe (11.2.5, up to EOF1); and the
 * derived arrival times above put some packets after the EOPF point.
 * NAKSTS reads 0 while a packet is on the wire (derived), so a parity
 * written then lands under it: stamped a microframe off, and the next
 * packet lost. gap_own counts such a packet only when it is a gap's first
 * evidence; a capture of the late packets on the bus would show whether
 * it happens. */
bool u2ps_dwc2_out_incomplete(u2ps_dwc2_out_t *o, uint32_t fnsof, bool idle, bool *odd);

/* A close begins: from now on completions are dropped (counted in
 * close_drops) and releases never arm. Held packets may still be
 * released. */
void u2ps_dwc2_out_closing(u2ps_dwc2_out_t *o);

/* The stream has ended (close done, or bus reset); the counters stay. */
void u2ps_dwc2_out_close(u2ps_dwc2_out_t *o);

/* The received length: the programmed XFRSIZ minus the XFRSIZ the core left
 * ("Payload size in memory = application programmed initial transfer size
 * - core updated final transfer size", RM0431 32.16.6, application
 * requirement 3; RM0456 73.15.6, p. 3443). A remainder above the
 * programmed size cannot come from the core: it gives 0 and counts in
 * errors. */
static inline uint16_t u2ps_dwc2_out_len(u2ps_dwc2_out_t *o, uint32_t programmed, uint32_t left)
{
  if (left > programmed) {
    o->errors++;
    return 0;
  }
  return (uint16_t)(programmed - left);
}

/* At the XFRC of the armed buffer, with fnsof read at the completion; P is
 * taken with the armed parity. P must lie after the last packet's
 * microframe (the guard): if it is behind (see above), the call returns
 * false and changes nothing when reread is false; with reread true it
 * counts in errors, takes P as read (the hardware's microframe) and reports
 * no miss for it. On true, *r holds the packet, what to report before it,
 * and the buffer to arm; the armed buffer is now held. Before the first
 * packet of a stream no hole is reported. If the device stack holds
 * count - U2PS_DWC2_OUT_RESERVE packets, it is a reserve packet instead:
 * r->buf is U2PS_DWC2_NO_BUF, r->arm_buf the same buffer, nothing to
 * report now; its microframe is recorded as NO_BUFFER and the hole before
 * it as NO_PACKET, for the next packet record or the tail, and the arm
 * decision follows as after a drop. Either way P is the base of the next
 * arm and of the next hole. In CLOSING the packet is dropped (r->buf and
 * r->arm_buf U2PS_DWC2_NO_BUF, close_drops counted); in any other state
 * but ARMED (a completion the core cannot give) it counts in errors and
 * sets r->buf and r->arm_buf to U2PS_DWC2_NO_BUF. */
bool u2ps_dwc2_out_complete(u2ps_dwc2_out_t *o, uint32_t fnsof, bool reread,
                              u2ps_dwc2_out_packet_t *r);

/* At the XFRC of the armed buffer, for a packet that must not be delivered
 * (an invalid data PID), with fnsof read at the completion. Counts in
 * errors; the microframe joins the hole before the next packet. Returns the
 * buffer to arm again (the same one), after which u2ps_dwc2_out_arm()
 * decides as after a completion; in CLOSING it counts in close_drops and
 * returns U2PS_DWC2_NO_BUF, and in any other state but ARMED it counts
 * in errors and returns U2PS_DWC2_NO_BUF. */
uint16_t u2ps_dwc2_out_drop(u2ps_dwc2_out_t *o, uint32_t fnsof);

/* The arm decision, after u2ps_dwc2_out_complete() or
 * u2ps_dwc2_out_drop(), with fnsof read immediately before the
 * OTG_DOEPCTLx write. Returns true, and a buffer is to be armed with
 * a->odd, after every completion and drop outside a close: a buffer is
 * always free to arm. A 'now' behind the completion's microframe cannot
 * come from the hardware: it counts in errors and is taken as that
 * microframe. Counts a late completion if now differs from the
 * completion's microframe.
 * - After a delivered packet: B-next from P gives the parity, and a->late
 *   LATE microframes are reported after the packet.
 * - After a drop or a reserve packet: B-next from its microframe. If the
 *   arm is late, the microframes since the last report up to a dropped
 *   one are recorded as NO_PACKET (none for a reserve packet, which is
 *   the base itself) and the late ones as LATE, for the next packet
 *   record or the tail; a->late is 0. */
bool u2ps_dwc2_out_arm(u2ps_dwc2_out_t *o, uint32_t fnsof, u2ps_dwc2_bnext_t *a);

/* Releases the n oldest held packets: bookkeeping only, nothing to arm,
 * as a buffer is always armed (U2PS_DWC2_OUT_RESERVE); in CLOSING it frees
 * them too. n = 0 does nothing; n above the held count, a release with no
 * stream, or one while a completion or drop awaits its arm decision, does
 * nothing and counts in errors. Inline: the release costs no call. The
 * ring's indices stay below count (head < count, held < count), so
 * head + n is below 2 * count, and one subtraction takes it modulo
 * count; the sum stays in 32 bits until then, as it passes 65,535 above
 * 32,768 buffers. */
static inline void u2ps_dwc2_out_release(u2ps_dwc2_out_t *o, uint16_t n)
{
  if (n == 0u)
    return;
  if (o->state == U2PS_DWC2_OUT_CLOSED || o->state == U2PS_DWC2_OUT_COMPLETED ||
      o->state == U2PS_DWC2_OUT_DROPPED || n > o->held) {
    o->errors++;
    return;
  }
  uint32_t head = (uint32_t)o->head + n;
  o->head = (uint16_t)(head >= o->count ? head - o->count : head);
  o->held = (uint16_t)(o->held - n);
}

/* The stream's tail, at its end (before on_closed or on_reset, or inside
 * connect(false)), in any state: *r gets the holes recorded after the last
 * delivered packet, up to the last packet received (reserve packets as
 * NO_BUFFER, the hole before each as NO_PACKET, a dropped packet's hole as
 * NO_PACKET, and the LATE microframes of an arm after them), with buf and
 * arm_buf U2PS_DWC2_NO_BUF and uframe the last packet's microframe; they
 * are taken, so a second call gives none. Nothing is recorded before a
 * stream's first packet, nor during a close. Also while the stream goes
 * on (u2ps_dport_report_holes): the next packet record then holds only
 * what comes after, since every completion is the base of the next hole. */
void u2ps_dwc2_out_tail(u2ps_dwc2_out_t *o, u2ps_dwc2_out_packet_t *r);

/* ---- Feedback IN endpoint ---------------------------------------------------
 *
 * MCNT = PKTCNT = 1, one packet per arm; the data must be in the Tx FIFO
 * before the IN token, or the core sends a zero-length packet (RM0431
 * 32.16.6, "Generic periodic IN data transfers", requirement 3; RM0456
 * 73.15.6, p. 3453). The core sends in microframes of the armed parity
 * (EONUM). The logic keeps the parity that is armed (armed_odd) apart from
 * the parity of the next arm (odd):
 * - The first arm after the open uses parity(now + 1). Until the first
 *   XFRC the parity then follows the bus: at each EOPF (OTG_GINTSTS bit
 *   15, raised at the periodic frame interval of every microframe, RM0431
 *   32.15.6; RM0456 73.14.6, p. 3330) the register layer writes SODDFRM or
 *   SEVNFRM for the next microframe to the enabled endpoint, so the host's
 *   first poll finds its parity armed in whatever microframe it comes. The
 *   host's polls all have one parity, which only the first XFRC tells; a
 *   guess fails for half of the opens, and that poll gets a zero-length
 *   packet. Neither manual restricts when SODDFRM and SEVNFRM may be
 *   written (RM0431 32.15.51; RM0456 73.14.48, p. 3377); that the write
 *   takes effect on an enabled endpoint is inferred and measured on the
 *   board. A token that comes after the EOPF of its microframe finds the
 *   next microframe's parity. The follow has no time limit: it ends only
 *   at the first XFRC, or with the stream. Linux polls the feedback
 *   endpoint up to about 240 ms after SET_INTERFACE at high rates, and a
 *   follow that ended earlier left a guessed parity, wrong for half of
 *   those opens. It costs one EOPF pass of the handler per microframe
 *   until the first poll (the cycles of that pass are still to be
 *   measured; U2PS_DWC2_PASS_EOPF in the pass profile); a host that never
 *   polls the feedback endpoint keeps it for the whole stream. With the
 *   follow turned off (a bench comparison) the parity guessed at the open
 *   stays, and the switch rule below takes over at once.
 * - After each XFRC the endpoint is re-armed with the same parity; the
 *   parity is not taken from FNSOF at the XFRC, which is wrong when the
 *   XFRC is handled in the next microframe. The XFRC gives the poll
 *   microframe Q (u2ps_dwc2_uframe with the armed parity), and the next
 *   poll is expected at Q + period.
 * - IISOIXFR (OTG_GINTSTS bit 20) fires at the periodic frame interval for
 *   a non-empty isochronous IN FIFO scheduled for that microframe (RM0431
 *   32.16.6, internal data flow 6; RM0456 73.15.6, p. 3454), so with a
 *   period above 2 also in armed-parity microframes the host does not poll
 *   (inferred). The register layer sets that interval to 95 % of the
 *   microframe (OTG_DCFG.PFIVL = 11, RM0456 73.14.38, p. 3366), so a late
 *   IN token still comes before it. The logic acts on IISOIXFR only in an
 *   expected poll microframe, or one later if handled late: the manual's
 *   recovery, each step on its own interrupt (SNAK and EPDIS; at EPDISD the
 *   Tx FIFO flush and a new arm, RM0431 32.16.6 "Incomplete isochronous IN
 *   data transfers"; RM0456 p. 3454-3455).
 * - Once an XFRC has confirmed the armed parity, a missed expected poll
 *   gets the recovery with the same parity. The parity is switched only if
 *   no XFRC comes for one period after the open with the follow off, or
 *   after an arm made by a recovery or by a switch; the switch uses the
 *   same recovery.
 * - IISOIXFR comes in every armed-parity microframe in which the FIFO is
 *   still full at the periodic frame interval, polled or not, since after
 *   an XFRC the re-arm fills it again (measured on the U5: about 4000 per
 *   second at a period of 8). The logic needs it only around an expected
 *   poll: u2ps_dwc2_fb_watch() tells the register layer the microframes
 *   in which to have it unmasked. OTG_GINTSTS still records it while
 *   masked (RM0431 32.15.7; RM0456 73.14.8, p. 3336), so the register
 *   layer clears it before it unmasks.
 * - An XFRC that comes during a recovery confirms the armed parity (Q from
 *   that parity, and a pending switch is cancelled); the register layer
 *   does not re-arm on it, the recovery's arm follows.
 * - A recovery or a switch counts in recoveries (fb_recoveries) only if
 *   the stream's data flowed at it and a poll came after it. Data flowed
 *   if a data packet came in the period before the IISOIXFR's microframe,
 *   that is with (fn - P) mod 2^14 from -2 to period, P being the last
 *   packet's microframe, which the register layer gives with
 *   u2ps_dwc2_fb_data(); such a recovery waits in 'pending' until the
 *   next XFRC moves it to recoveries. All others count in
 *   idle_recoveries: those before the stream's first packet, those after
 *   the data stopped, and those pending when the stream closes
 *   (u2ps_dwc2_fb_close) or the next opens. At a stream's end hosts stop
 *   polling the feedback some periods before the data (measured on the
 *   U5 behind Linux: about 4 ms at 48 and 192 kHz), and no poll comes
 *   after those recoveries. The action is the same either way. A packet a
 *   multiple of 2.048 s before the IISOIXFR, within a period, aliases into
 *   the window.
 * The handler takes an XFRC before an IISOIXFR seen in the same pass. */

typedef enum {
  U2PS_DWC2_FB_NONE,    /* nothing to do */
  U2PS_DWC2_FB_RECOVER, /* the manual's recovery; re-arm with the same parity */
  U2PS_DWC2_FB_SWITCH   /* the same recovery; re-arm with the other parity (f->odd) */
} u2ps_dwc2_fb_action_t;

typedef struct {
  uint16_t period;        /* microframes between polls, 2^(bInterval-1) */
  bool odd;               /* the parity of the next arm */
  bool armed_odd;         /* the parity armed now */
  bool following;         /* the parity follows the bus until the first XFRC */
  bool confirmed;         /* an XFRC came since the last arm made at the open, a recovery or a switch */
  bool expect_known;      /* expect holds a poll microframe */
  bool recovering;        /* a recovery runs until u2ps_dwc2_fb_rearm() */
  u2ps_uframe_t expect;   /* the next poll microframe expected */
  u2ps_uframe_t armed_at; /* microframe read at the open or at a recovery's arm */
  bool data_seen;           /* a data packet has come since the open */
  u2ps_uframe_t data_at;    /* the microframe of the last data packet */
  uint32_t pending;         /* recoveries while data flowed, until the next XFRC */
  uint32_t recoveries;      /* fb_recoveries of u2ps_dport_stats_t: data flowed, a poll followed */
  uint32_t idle_recoveries; /* the other recoveries and switches */
  /* The microframes in which IISOIXFR is needed (u2ps_dwc2_fb_watch), set
   * by every call that changes them: x = (fn - win_start) mod 2^14 lies in
   * the window if x < win_len and (x & win_mask) <= 1 */
  u2ps_uframe_t win_start;
  uint16_t win_len, win_mask;
} u2ps_dwc2_fb_t;

/* Opens the feedback endpoint for bInterval 'interval', with now from FNSOF
 * read immediately before the OTG_DIEPCTLx write; f->odd and f->armed_odd
 * are parity(now + 1), and the parity follows the bus until the first
 * XFRC (u2ps_dwc2_fb_eopf). Returns false if interval lies outside
 * U2PS_DWC2_FB_INTERVAL_MIN..U2PS_DWC2_FB_INTERVAL_MAX. No data packet
 * has come. f->recoveries and f->idle_recoveries are kept, and recoveries
 * still pending from the last stream count in idle_recoveries: zero the
 * struct once, before the first open. */
bool u2ps_dwc2_fb_open(u2ps_dwc2_fb_t *f, uint8_t interval, uint32_t fnsof);

/* At EOPF while the parity follows the bus, with FNSOF read at that point:
 * returns true with f->odd and f->armed_odd set to parity(fn + 1), which
 * the register layer writes to the enabled endpoint (SODDFRM or SEVNFRM
 * with the endpoint's characteristics, without EPENA). Returns false, and
 * changes nothing, once the follow is over: the register layer masks EOPF
 * again. The follow ends only at the first XFRC (or is turned off,
 * u2ps_dwc2_fb_follow_off), however late the first poll comes. The
 * handler takes the feedback endpoint's XFRC before an EOPF seen in the
 * same pass. */
bool u2ps_dwc2_fb_eopf(u2ps_dwc2_fb_t *f, uint32_t fnsof);

/* At OTG_DIEPINTx.XFRC, with FNSOF read at that point. Ends the follow,
 * and the pending recoveries count in f->recoveries. Returns true if the
 * register layer re-arms now, with f->odd (the armed parity); false during
 * a recovery, whose arm follows. */
bool u2ps_dwc2_fb_xfrc(u2ps_dwc2_fb_t *f, uint32_t fnsof);

/* The stream has closed or stopped: recoveries still pending, which no
 * poll followed, count in f->idle_recoveries */
void u2ps_dwc2_fb_close(u2ps_dwc2_fb_t *f);

/* The follow turned off right after the open (u2ps_dwc2_dcd_mode_t, a
 * bench comparison): the guessed parity stays, and the switch rule takes
 * over at once */
void u2ps_dwc2_fb_follow_off(u2ps_dwc2_fb_t *f);

/* A data packet of the stream came in microframe p (its microframe P, as
 * u2ps_dwc2_out_complete() gives it) */
static inline void u2ps_dwc2_fb_data(u2ps_dwc2_fb_t *f, u2ps_uframe_t p)
{
  f->data_seen = true;
  f->data_at = p & U2PS_UFRAME_MASK;
}

/* At IISOIXFR, with FNSOF read at that point: what to do. A recovery or a
 * switch while data flows waits in f->pending for the next XFRC, otherwise
 * counts in f->idle_recoveries (above); until u2ps_dwc2_fb_rearm()
 * further IISOIXFR are ignored. */
u2ps_dwc2_fb_action_t u2ps_dwc2_fb_iisoixfr(u2ps_dwc2_fb_t *f, uint32_t fnsof);

/* The recovery's new arm, with FNSOF read immediately before the
 * OTG_DIEPCTLx write; it arms f->odd, which becomes the armed parity. */
void u2ps_dwc2_fb_rearm(u2ps_dwc2_fb_t *f, uint32_t fnsof);

/* Whether IISOIXFR is needed in the microframe FNSOF reads now: while no
 * poll is known (with the follow off before the first poll, or after a
 * switch), always;
 * with an expected poll E, in E - 1 and E and in the same microframes of
 * the periods after E, so that a missed poll is seen in its own
 * microframe or one later, up to half the wrap after E (E - 1 + 8192);
 * never while the parity follows the bus or a recovery runs, and not in
 * the poll microframe of the XFRC just taken. The handler asks for every
 * data packet, so the window is computed once per change of the
 * feedback's state (each poll, recovery, switch and follow step) into
 * win_start, win_len and win_mask, and tested here inline. */
static inline bool u2ps_dwc2_fb_watch(const u2ps_dwc2_fb_t *f, uint32_t fnsof)
{
  uint16_t x = (uint16_t)((fnsof - f->win_start) & U2PS_UFRAME_MASK);
  return x < f->win_len && (x & f->win_mask) <= 1u;
}

#endif /* U2PS_DWC2_ISO_H */
