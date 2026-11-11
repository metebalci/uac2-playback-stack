/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_chipidea_iso.h: the ChipIdea device port's decision logic for its
 * stream, the isochronous OUT data endpoint and its isochronous IN feedback
 * endpoint, and for the close. Pure functions on plain state, with no
 * register access: the port's handlers read the registers and the dTDs,
 * call these functions and write what they return. They build on the PC and
 * on ARM.
 *
 * Manual: i.MX RT1060 Processor Reference Manual, IMXRT1060RM Rev. 3,
 * 07/2021, chapter 42, cited as RM with section and printed page.
 * - Each class buffer has one port-owned dTD, which carries one packet:
 *   dQH Mult 1, "Exactly MULT Packets per (micro) Frame are ...
 *   received", and a dTD retires on every packet, short or full (RM
 *   42.5.6.4.3, p. 2414-2415). Here a slot is a buffer with its dTD,
 *   numbered 0..count-1.
 * - The controller follows the dTD list by itself; software keeps its own
 *   head and tail (RM 42.5.6.6.1, p. 2419) and appends with the ATDTW
 *   tripwire; if the endpoint has run dry, it writes the dQH next pointer,
 *   clears Active and Halt and primes again (RM 42.5.6.6.3, p. 2420-2421).
 *   For continuous reception the list stays "out ahead of the device
 *   controller by at least two (micro)frames" (RM p. 2415), so the port
 *   keeps two slots queued (U2PS_CHIPIDEA_ISO_OUT_RESERVE), and a stream
 *   has at least U2PS_CHIPIDEA_ISO_OUT_MIN slots.
 * - A prime of an isochronous endpoint takes effect for the next
 *   (micro)frame (RM 42.5.6.4.3, p. 2414): a prime written in microframe t
 *   receives from t + 1 on, or from t + 2 if it was written too late in t
 *   to complete before the next SOF (RM 42.5.6.4.3.1, note on p. 2416).
 * - Nothing records a host skip: a primed isochronous dTD waits for a
 *   packet indefinitely, and an unprimed OUT endpoint ignores packets
 *   (RM 42.5.6.4.3, p. 2414; Table 42-72, p. 2416). Holes are therefore found from
 *   microframe numbers: the dTD's Frame Number, "the frame number in which
 *   a packet finishes" (RM Table 42-64, p. 2398), whose unit the manual
 *   does not state, or FRINDEX, whose bits 13:3 hold the SOF's frame number
 *   and bits 2:0 the microframe (RM 42.7.21, p. 2464), the layout of
 *   u2ps_uframe_t.
 *
 * The Frame Number unit is a build-time choice of the port,
 * U2PS_CHIPIDEA_FN_UNIT in u2ps_dport_cfg.h, which the register layer
 * passes to u2ps_chipidea_out_open():
 * - U2PS_CHIPIDEA_FN_UFRAMES: the 11 bits are bits 10:0 of the packet's
 *   microframe. Every hole is exact and comes immediately before the packet
 *   that ends it (U2PS_DPORT_UFRAME_EXACT 1).
 * - U2PS_CHIPIDEA_FN_FRAMES: the 11 bits are the packet's frame number,
 *   FRINDEX bits 13:3. The holes of a frame are exact once a packet of a
 *   later frame has come, and are reported before that packet. The first
 *   packet's place in its frame is estimated as the latest that FRINDEX at
 *   its pass allows; if the pass came late, holes of that first frame are
 *   lost, never added.
 * - U2PS_CHIPIDEA_FN_NONE, the fallback count: the field is not used.
 *   At each pass the balance is the microframes elapsed (FRINDEX, modulo
 *   2^14) minus the dTDs retired, which is the number of microframes
 *   without a packet. It counts one too many while the packet of the
 *   current microframe has not yet retired, for example when the
 *   interrupt enters one microframe after the last retirement, and the
 *   next pass takes that back. The duplicate guard therefore reports only
 *   what the balance shows at two passes in a row (the smaller of the
 *   two). If the balance falls below what was reported, nothing is
 *   reported, each microframe of the difference counts once in
 *   fallback_over (of u2ps_dport_stats_t; not an error), and
 *   later holes are reported only once the balance exceeds the reported
 *   total again. Totals are right to within one microframe; holes are
 *   placed only to the pass.
 * All three units also count microframes with FRINDEX where the endpoint
 * was dry (below); there FRINDEX is exact, since no packet can be in
 * flight. FRINDEX counts the SOFs received (RM 42.7.21, p. 2464): an SOF
 * the device misses gives the fallback count one phantom microframe
 * without a packet, and the next frame's SOF takes it back (inferred).
 *
 * No hole of any reason is reported before the stream's first delivered
 * packet (u2ps_dport.h, on_missed); a CORRUPTED packet before it is only
 * counted.
 *
 * Microframe numbers given with a packet are exact with FN_UFRAMES. With
 * the other units they are an estimate: the last one plus one plus the
 * holes reported before the packet, so that the device stack, which
 * derives NO_PACKET from microframe distances (u2ps_dev_on_packet), finds
 * the port's own NO_PACKET count.
 *
 * Microframe distances are taken modulo 2^14. A microframe counts as
 * behind another only if it lies up to U2PS_CHIPIDEA_BEHIND microframes
 * before it; every other distance is a forward gap, so a gap of up to
 * 2.048 s minus that window is counted, and a longer one aliases.
 */
#ifndef U2PS_CHIPIDEA_ISO_H
#define U2PS_CHIPIDEA_ISO_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_types.h"

/* The most slots of a stream: the device library's build constant, which
 * also bounds the count the device stack passes to stream_open */
#ifndef U2PS_ISO_OUT_COUNT_MAX
#error "u2ps_chipidea_iso.h: U2PS_ISO_OUT_COUNT_MAX is not defined: link u2ps_device (ARM builds), whose public compile definitions set it, or u2ps_chipidea_dcd (the PC build sets it there)"
#endif
#define U2PS_CHIPIDEA_SLOT_MAX U2PS_ISO_OUT_COUNT_MAX

/* No slot: returned where a slot or a dTD index is expected */
#define U2PS_CHIPIDEA_NO_SLOT 0xFFFFu

/* The slots the port keeps queued, its U2PS_DPORT_ISO_OUT_RESERVE: two.
 * An unprimed OUT endpoint ignores the host's packets ("For unprimed RX
 * endpoints, the response to an OUT transaction is to ignore the packet",
 * RM 42.5.6.4.3, p. 2414; Table 42-72, p. 2416), and for continuous
 * reception the list stays "out ahead of the device controller by at least
 * two (micro)frames" (RM p. 2415): the dTD that receives and the one after
 * it, queued while the port takes the first. The device stack holds at most
 * count - 2. */
#define U2PS_CHIPIDEA_ISO_OUT_RESERVE 2u

/* The least number of slots of a stream: the reserve and one the device
 * stack may hold */
#define U2PS_CHIPIDEA_ISO_OUT_MIN (U2PS_CHIPIDEA_ISO_OUT_RESERVE + 1u)

/* Microframes a number may lie before a reference and count as behind it
 * rather than as a forward gap of nearly 2^14 */
#define U2PS_CHIPIDEA_BEHIND 8u

/* The feedback endpoint's bInterval. Each poll retires one of the two
 * feedback dTDs, and the other stays primed while the port appends the
 * retired one again. With a period of at least 2 microframes the list stays
 * ahead of the controller by the two microframes that RM p. 2415 asks for.
 * USB 2.0 allows 1 to 16 for high-speed isochronous endpoints (9.6.6,
 * Table 9-13, p. 271). */
#define U2PS_CHIPIDEA_FB_INTERVAL_MIN 2u
#define U2PS_CHIPIDEA_FB_INTERVAL_MAX 16u

/* The Frame Number units (U2PS_CHIPIDEA_FN_UNIT) */
#define U2PS_CHIPIDEA_FN_NONE    0
#define U2PS_CHIPIDEA_FN_UFRAMES 1
#define U2PS_CHIPIDEA_FN_FRAMES  2

/* ---- Descriptors and registers --------------------------------------------- */

/* dTD token bits (RM Table 42-63, p. 2397-2398): Status bits 7:0 */
#define U2PS_CHIPIDEA_DTD_ACTIVE     0x80u /* the controller has not yet retired the dTD */
#define U2PS_CHIPIDEA_DTD_HALTED     0x40u
#define U2PS_CHIPIDEA_DTD_BUF_ERROR  0x20u /* Data Buffer Error: overflow, the dQH halts */
#define U2PS_CHIPIDEA_DTD_XACT_ERROR 0x08u /* Transaction Error: on ISO OUT, a CRC error */

/* The token of a dTD the port appends: Total Bytes 'bytes' (bits 30:16),
 * IOC (bit 15), MultO (bits 11:10, 0 except for a transmit ISO dTD) and
 * Active, the other Status bits 0 (RM Table 42-63, p. 2397-2398; RM
 * 42.5.6.6.2, p. 2420). Total Bytes is at most 0x4000 here, the size RM
 * recommends; more is cut to that. */
uint32_t u2ps_chipidea_dtd_token(uint16_t bytes, uint8_t multo);

/* The bytes a retired dTD did not move: Total Bytes of its token, which the
 * controller decrements by the bytes moved (RM Table 42-63, p. 2397) */
uint16_t u2ps_chipidea_dtd_left(uint32_t token);

/* The capabilities word of an isochronous endpoint's dQH: Mult 1 (bits
 * 31:30), ZLT 1 (bit 29, not relevant for ISO), Maximum Packet Length
 * (bits 26:16, at most 1024), IOS 0 (RM Table 42-58, p. 2395) */
uint32_t u2ps_chipidea_dqh_iso_caps(uint16_t mps);

/* The bit of endpoint ep (0..7) in ENDPTPRIME, ENDPTFLUSH, ENDPTSTAT and
 * ENDPTCOMPLETE: bit ep for the receive (OUT) direction, bit 16 + ep for
 * the transmit (IN) direction (RM 42.7.35-42.7.38, p. 2484-2487); 0 for an
 * ep above 7 */
uint32_t u2ps_chipidea_ep_bit(uint8_t ep, bool in);

/* ---- Isochronous OUT stream ------------------------------------------------
 *
 * The stream's slots are each in exactly one of three places: the queue,
 * the dTDs linked into the endpoint's list that a pass has not yet taken
 * (retired by the controller or not), in list order; held, the packets
 * delivered to the device stack and not yet released, in arrival order;
 * or released, the slots the device stack has released since the last
 * retirement, which that retirement's append queues. The open queues
 * every slot. Positions in the queue count from its head;
 * u2ps_chipidea_out_at() gives the slot at a position.
 *
 * The reserve: U2PS_CHIPIDEA_ISO_OUT_RESERVE slots stay free (queued or
 * released). A retired dTD with a good packet is delivered only if that
 * leaves at least that many free; otherwise it is a reserve packet:
 * counted NO_BUFFER in its own microframe, reported before the next
 * packet or in the tail, not delivered, and its slot queued again at once
 * (its data is overwritten unread; nothing is copied). After every
 * retirement's append at least that many are queued, so the endpoint does
 * not run dry while the device stack holds, every packet the host sends
 * is delivered or counted, and a host pause inside a hold stays
 * NO_PACKET. A release is bookkeeping only: it writes no register.
 *
 * The register layer's sequences:
 *
 * At the open: check with u2ps_chipidea_close_idle() that neither
 * endpoint is still primed from the last stream; link the dTDs of every
 * slot in queue order (token u2ps_chipidea_dtd_token(mps, 0), the last
 * with T), write the dQH (caps u2ps_chipidea_dqh_iso_caps(mps), next
 * pointer the first slot's dTD), enable the endpoint
 * (u2ps_chipidea_ep_ctrl()) and prime once.
 *
 * The dry read. ENDPTSTAT reads 0 for a moment while the controller primes
 * the next dTD after a retirement, and ENDPTPRIME reads 1 for a moment then
 * (RM 42.7.35, p. 2484; 42.7.37, p. 2485-2486); the manual does not say
 * that the two windows overlap, so two plain reads may both give 0 on an
 * endpoint that stays primed. The register layer therefore reads the
 * endpoint's bits in both registers under the ATDTW tripwire, which the
 * controller clears "when state machine is hazard region" (USBCMD.ATDTW,
 * RM 42.7.18, p. 2455): set ATDTW, read ENDPTPRIME and ENDPTSTAT, read
 * ATDTW, and repeat from the start while it reads 0; then clear ATDTW. The
 * endpoint is dry only if both bits read 0 in a read that ATDTW guarded
 * (inferred: the manual names the tripwire for appends only).
 *
 * At every entry of the port's handler while a stream is open, whatever
 * the interrupt (the feedback endpoint's completion comes once per poll
 * period, and a lost prime stops the data endpoint's own completions):
 *   1. Count the dTDs at the queue's head that have retired, in order, up
 *      to the first that has not: n. A dTD has retired if its Active bit is
 *      0, or if Halted or Data Buffer Error is set, whose Active bit the
 *      manual leaves unstated after an overflow (RM p. 2410).
 *   2. Read FRINDEX, after the count, so that every dTD counted lies at or
 *      before it.
 *   3. If n > 0: u2ps_chipidea_out_pass(n, FRINDEX), then for each of the
 *      n dTDs, in order, u2ps_chipidea_out_retire() with its token and its
 *      Frame Number (page 1 bits 10:0). Report the record's holes
 *      (on_missed: NO_PACKET, LATE, NO_BUFFER, CORRUPTED), then deliver its
 *      packet if it has one. If the record asks for an append, append it
 *      (below) before the next retire. (RM 42.5.6.6.4, p. 2421: "DCD must
 *      search the dTD linked list and retire all dTDs that have finished".)
 *   4. If dTDs remain queued and the dry read finds the endpoint dry, the
 *      controller has lost its prime with dTDs queued (an overflow halt, or
 *      erratum ERR050101 on silicon A): count the retired dTDs at the
 *      queue's head (k, normally 0), read FRINDEX immediately before the
 *      prime, call u2ps_chipidea_out_primed(k, FRINDEX) and prime the slot
 *      it returns, as in Case 1 below.
 *
 * An append, from a record of u2ps_chipidea_out_retire(), in the
 * handler:
 *   1. Write the token of each appended slot (positions first..first+n-1,
 *      at once: a later retire moves the positions) and link them in that
 *      order, the last with T.
 *   2. If tail is U2PS_CHIPIDEA_NO_SLOT, the queue was empty: Case 1.
 *      Otherwise link tail's dTD to the first appended one and run the
 *      ATDTW sequence (RM 42.5.6.6.3, Case 2). If ENDPTPRIME showed the
 *      endpoint, or the sequence read ENDPTSTAT 1, the append is done.
 *   3. Case 1, the endpoint is dry: count the retired dTDs at the queue's
 *      head (k), read FRINDEX immediately before the prime, call
 *      u2ps_chipidea_out_primed(k, FRINDEX); unless it returns
 *      U2PS_CHIPIDEA_NO_SLOT, write the dQH next pointer with that
 *      slot's dTD and T 0 as one word, clear Active and Halt in the dQH
 *      and write ENDPTPRIME.
 *
 * Dry microframes. The endpoint is dry when its list has run out (every
 * queued dTD retired before the port took them) or when it lost its
 * prime. From the last packet up to the prime that ends it, no packet can
 * arrive, so FRINDEX read at that prime bounds the dry run exactly. Its
 * microframes are LATE: the port took the retired dTDs too late (its
 * interrupt latency or ITC against the queue's slack), or the endpoint
 * lost its prime; the device stack's holds cannot make it dry (the
 * reserve). An overflow's own microframe is LATE with the halt that
 * follows it. As on DWC2, LATE thus also covers microframes lost because
 * the port's handler came late, not only a lost prime.
 * If retired dTDs not yet taken were still queued at that prime (k > 0),
 * they lie before the dry run: with a Frame Number unit the run is counted
 * when the k-th of them has been taken at its Frame Number, so holes
 * between them stay NO_PACKET and their microframe numbers stay exact; the
 * fallback count counts them at the prime. Either way the run is reported
 * before the packet after them. The microframe after the prime, if no
 * packet came in it, counts with the dry run's reason too, since a prime
 * late in its microframe receives only from the one after (RM p. 2416).
 *
 * A close (u2ps_chipidea_close_begin()):
 *   1. Stop new transfers: write ENDPTCTRLn with RXE and TXE 0
 *      (u2ps_chipidea_ep_ctrl(..., false)), so that a refused flush does
 *      not let the controller prime the next linked dTD (RM 42.7.40,
 *      p. 2489-2490). What RXE 0 does to an isochronous dTD already primed
 *      the manual does not state: it is measured on the board. Appends stop
 *      too (u2ps_chipidea_out_closing()).
 *   2. Write ENDPTFLUSH with the endpoints' bits, enable USBSTS.SRI.
 *   3. At each SOF: u2ps_chipidea_close_sof() with ENDPTFLUSH,
 *      ENDPTPRIME and ENDPTSTAT; flush again when it says so.
 *   4. At DONE, u2ps_chipidea_out_tail(), whose holes the port reports with
 *      on_missed, then u2ps_chipidea_out_close(), then on_closed.
 * Until u2ps_chipidea_out_close() retired dTDs are dropped (counted in
 * close_drops), releases free their packets and nothing is queued, and
 * nothing is primed. At a bus reset and inside connect(false) the port
 * likewise reports u2ps_chipidea_out_tail() before on_reset or before it
 * returns, and so does u2ps_dport_report_holes() while the stream is open.
 * A count above 65,535 goes in several on_missed calls.
 */

/* Holes to report before a packet, in this order */
typedef struct {
  uint32_t no_packet;
  uint32_t late;
  uint32_t no_buffer;
  uint32_t corrupted;
} u2ps_chipidea_holes_t;

/* Slots to append (u2ps_chipidea_out_retire()) */
typedef struct {
  uint16_t first; /* queue position of the first slot to append */
  uint16_t n;     /* slots to append, in queue order; 0: none */
  uint16_t tail;  /* the slot to link them after, or U2PS_CHIPIDEA_NO_SLOT:
                     the queue was empty (Case 1) */
} u2ps_chipidea_append_t;

/* One retired dTD: what to report, the packet, and what to append */
typedef struct {
  u2ps_chipidea_holes_t holes; /* report first */
  uint16_t buf;                /* the packet's slot, or U2PS_CHIPIDEA_NO_SLOT: no packet */
  uint16_t len;                /* its length */
  u2ps_uframe_t uframe;        /* its microframe (exact with FN_UFRAMES) */
  u2ps_chipidea_append_t append; /* a dropped dTD's slot to append again */
} u2ps_chipidea_out_packet_t;

/* A dry prime made while k retired dTDs were still queued: the dry run
 * lies after them */
#define U2PS_CHIPIDEA_PEND_MAX 4u
typedef struct {
  uint16_t k;                  /* dTDs still to take before it applies */
  u2ps_uframe_t t;             /* FRINDEX at the prime (Frame Number units) */
  u2ps_chipidea_holes_t holes; /* the run, counted at the prime (fallback count) */
} u2ps_chipidea_dry_t;

/* Where the stream's logic stands */
typedef enum {
  U2PS_CHIPIDEA_OUT_CLOSED,
  U2PS_CHIPIDEA_OUT_OPEN,
  U2PS_CHIPIDEA_OUT_CLOSING
} u2ps_chipidea_out_state_t;

typedef struct {
  uint8_t state;           /* u2ps_chipidea_out_state_t */
  uint8_t unit;            /* U2PS_CHIPIDEA_FN_* */
  bool started;            /* a dTD has retired since the open: the balance has a base */
  bool based;              /* the base is confirmed: a balance below the reports is an over-report */
  uint16_t count;          /* slots of the stream */
  uint16_t mps;            /* Total Bytes of each dTD */
  uint16_t q_head, q_len;  /* the queue */
  uint16_t h_head, h_len;  /* the held packets, after h_free released ones */
  uint16_t h_free;         /* released slots at the head of h, queued at the next retirement */
  bool delivered;          /* a packet has been delivered since the open */
  bool grace;              /* a dry prime was the last event: the microframe after it
                              counts with its reason, LATE */
  uint16_t counted;      /* dTDs at the queue's head already counted in the balance */
  uint16_t n_since;      /* dTDs counted after ref (Frame Number units) */
  uint16_t pass_left;    /* dTDs of the current pass not yet taken */
  u2ps_uframe_t ref;     /* the balance counts the microframes up to ref */
  u2ps_uframe_t now;     /* FRINDEX at the last pass */
  u2ps_uframe_t uframe;  /* microframe given with the last packet, plus the holes taken
                            after it (u2ps_chipidea_out_tail) */
  int32_t surplus;       /* microframes up to ref without a counted dTD, less the holes accounted */
  int32_t surplus_prev;  /* surplus after the previous pass: the duplicate guard */
  int32_t low;           /* the lowest surplus below 0 since the last report */
  u2ps_chipidea_holes_t next;  /* holes to report before the next packet */
  uint8_t pend_len;            /* dry primes whose retired dTDs are still queued */
  u2ps_chipidea_dry_t pend[U2PS_CHIPIDEA_PEND_MAX];
  uint16_t q[U2PS_CHIPIDEA_SLOT_MAX];
  uint16_t h[U2PS_CHIPIDEA_SLOT_MAX];
  /* Counters, for u2ps_dport_stats_t; kept across opens */
  uint32_t no_packet_total;  /* NO_PACKET microframes reported */
  uint32_t late_total;       /* LATE microframes */
  uint32_t no_buffer_total;  /* NO_BUFFER microframes: reserve packets */
  uint32_t corrupted_total;  /* CORRUPTED packets */
  uint32_t fallback_over;    /* microframes the fallback count reported too many, found later */
  uint32_t errors;           /* states the hardware cannot produce, bad calls */
  uint32_t close_drops;      /* dTDs retired during a close, dropped */
} u2ps_chipidea_out_t;

/* Opens a stream of 'count' slots (U2PS_CHIPIDEA_ISO_OUT_MIN up to
 * U2PS_CHIPIDEA_SLOT_MAX) of packets of up to mps bytes (1..1024), with
 * the Frame Number unit 'unit'. Every slot is queued, in order 0..count-1.
 * Returns false, with the stream closed, if an argument is out of range.
 * The counters are kept: zero the struct once, before the first open. */
bool u2ps_chipidea_out_open(u2ps_chipidea_out_t *o, uint16_t count, uint16_t mps, uint8_t unit);

/* The slot at queue position pos, or U2PS_CHIPIDEA_NO_SLOT beyond the
 * queue */
uint16_t u2ps_chipidea_out_at(const u2ps_chipidea_out_t *o, uint16_t pos);

/* Starts a pass: n retired dTDs at the queue's head, FRINDEX read after
 * counting them. With FN_NONE it updates the balance and runs the
 * duplicate guard; what it finds is reported before the pass's first
 * packet. n = 0 does nothing; n above the queue counts in errors and takes
 * the queue. */
void u2ps_chipidea_out_pass(u2ps_chipidea_out_t *o, uint16_t n, uint32_t frindex);

/* Takes the dTD at the queue's head, retired, with its token and its Frame
 * Number field. *r gets the holes to report before it, and:
 * - a good packet (Status 0): its slot, now held, its length (mps minus
 *   Total Bytes left) and its microframe; or, if fewer than
 *   U2PS_CHIPIDEA_ISO_OUT_RESERVE slots would stay free (queued, or
 *   released and not yet queued), a reserve packet: no packet, NO_BUFFER 1
 *   reported before the next packet (or in the tail), and the slot
 *   appended again;
 * - Transaction Error, a CRC error on an isochronous OUT packet whose data
 *   the controller stored "for the application software to sort out" (RM
 *   p. 2415; Table 42-74, p. 2422): no packet; CORRUPTED 1 is reported
 *   before the next packet, and the slot is appended again;
 * - Data Buffer Error or Halted, an overflow that halts the dQH (RM Table
 *   42-74, p. 2422): no packet, counted in errors; the slot is appended
 *   again and, as the endpoint is dry, primed. Its microframe and the
 *   halted ones are LATE. Such a dTD is taken even with Active set.
 * A token with Active set and neither of those bits (not retired), a
 * remainder above mps, or an empty queue count in errors; such an active
 * dTD is not taken. r->append holds the slots to append: those the device
 * stack released since the last retirement, oldest first, then a slot
 * queued again. In CLOSING the dTD is dropped (close_drops) and nothing is
 * appended. */
void u2ps_chipidea_out_retire(u2ps_chipidea_out_t *o, uint32_t token, uint32_t fn,
                                u2ps_chipidea_out_packet_t *r);

/* Releases the n oldest held packets: bookkeeping only, nothing to append
 * or prime now. Their slots, oldest first, are queued at the next
 * retirement (u2ps_chipidea_out_retire's append), while the queue keeps
 * the reserve meanwhile. n = 0 does nothing; n above the held packets, or
 * no stream, does nothing and counts in errors. In CLOSING the packets are
 * freed and never queued. */
void u2ps_chipidea_out_release(u2ps_chipidea_out_t *o, uint16_t n);

/* The endpoint is dry and the register layer primes it, after an append
 * found it dry or after the handler found the prime lost: k retired dTDs
 * at the queue's head, FRINDEX read immediately before the prime. Notes
 * the dry run up to FRINDEX, LATE, to be counted as described above, and
 * returns the slot whose dTD the dQH's next pointer gets: the first one
 * not retired. Returns U2PS_CHIPIDEA_NO_SLOT, with nothing to prime, if
 * every queued dTD has retired, or outside OPEN (counted in errors).
 * Before the stream's first dTD nothing is counted. More than
 * U2PS_CHIPIDEA_PEND_MAX primes with retired dTDs queued count in errors,
 * and the last run takes the new one's place. */
uint16_t u2ps_chipidea_out_primed(u2ps_chipidea_out_t *o, uint16_t k, uint32_t frindex);

/* The stream's tail, at its end (before on_closed or on_reset, or inside
 * connect(false)), in any state: *h gets the holes found after the last
 * delivered packet, up to the last packet taken (reserve packets as
 * NO_BUFFER, the holes between them, CRC errors, dry runs), placed as the
 * unit allows (exact with FN_UFRAMES); they are taken, so a second call
 * gives none. None before the stream's first packet. Also while the stream
 * goes on (u2ps_dport_report_holes, in thread with the vector masked):
 * holes not yet found stay for the next packet, whose microframe (an
 * estimate with FN_FRAMES and FN_NONE) still counts the ones taken. */
void u2ps_chipidea_out_tail(u2ps_chipidea_out_t *o, u2ps_chipidea_holes_t *h);

/* A close begins */
void u2ps_chipidea_out_closing(u2ps_chipidea_out_t *o);

/* The stream has ended (close done, or bus reset); the counters stay */
void u2ps_chipidea_out_close(u2ps_chipidea_out_t *o);

/* ---- Feedback IN endpoint --------------------------------------------------
 *
 * Two dTDs, 0 and 1, point at the port's feedback word, each with Total
 * Bytes fb_size and MultO 1 (RM Table 42-63, p. 2398), so each sends one
 * packet at a poll. The device has no interval setting: a primed dTD waits
 * for the host's IN token, and an unprimed endpoint answers it with a
 * zero-length packet (RM 42.5.6.4.3, p. 2414; Table 42-72, p. 2416). The
 * controller fetches a transmit dTD's data when it primes it (RM
 * 42.5.6.3.3.3, p. 2407-2408), which for the second dTD is when the first
 * retires; a new feedback value therefore goes out within two polls
 * (inferred). The open links 0 then 1 and primes once; each dTD a pass
 * takes is appended again at once, as for the data endpoint (append
 * sequence above, with u2ps_chipidea_fb_primed() for Case 1). */

typedef struct {
  uint8_t state;   /* u2ps_chipidea_out_state_t */
  uint8_t head;    /* queue: the dTDs in list order */
  uint8_t len;
  uint8_t q[2];
  uint32_t polls;      /* dTDs retired: feedback packets sent */
  uint32_t recoveries; /* primes after the list ran dry: fb_recoveries. A dry list needs
                          polls that retired both dTDs, so none comes once the host stops
                          polling at a stream's end, and no rule on the data is needed. */
  uint32_t errors;
} u2ps_chipidea_fb_t;

/* Opens the feedback endpoint for bInterval 'interval'; both dTDs are
 * queued, 0 first. Returns false if interval lies outside
 * U2PS_CHIPIDEA_FB_INTERVAL_MIN..U2PS_CHIPIDEA_FB_INTERVAL_MAX. The
 * counters are kept: zero the struct once. */
bool u2ps_chipidea_fb_open(u2ps_chipidea_fb_t *f, uint8_t interval);

/* Takes the retired dTD at the queue's head (token with Active 0) and
 * appends it again at once: returns its index (0 or 1) and sets *tail to
 * the dTD to link it after, or U2PS_CHIPIDEA_NO_SLOT if the queue was
 * empty (Case 1). Returns U2PS_CHIPIDEA_NO_SLOT, counting in errors, for
 * an active token or an empty queue; in CLOSING it takes the dTD without
 * appending it. */
uint16_t u2ps_chipidea_fb_retire(u2ps_chipidea_fb_t *f, uint32_t token, uint16_t *tail);

/* An append found the endpoint dry, with k retired dTDs at the queue's
 * head: counts a recovery and returns the dTD for the dQH's next pointer,
 * the first one not retired, or U2PS_CHIPIDEA_NO_SLOT if none is left. */
uint16_t u2ps_chipidea_fb_primed(u2ps_chipidea_fb_t *f, uint16_t k);

void u2ps_chipidea_fb_closing(u2ps_chipidea_fb_t *f);
void u2ps_chipidea_fb_close(u2ps_chipidea_fb_t *f);

/* ---- Close ----------------------------------------------------------------
 *
 * The stream closes asynchronously (RM 42.5.6.6.5, p. 2421-2422), in the
 * sequence of the stream section above. The flush "may take a large amount
 * of time" and should not be waited for in an interrupt; it fails if a
 * packet is in progress, which then completes, and is repeated for the
 * endpoints still ready. The close is done only when ENDPTFLUSH,
 * ENDPTPRIME and ENDPTSTAT show neither endpoint at two SOFs in a row, so
 * that a prime the controller started at one of them has shown by the
 * next (inferred). Using the SOF interrupt as the clock is inferred. */

typedef enum {
  U2PS_CHIPIDEA_CLOSE_WAIT,  /* the flush runs: check again at the next SOF */
  U2PS_CHIPIDEA_CLOSE_FLUSH, /* the flush failed: write ENDPTFLUSH with *flush again */
  U2PS_CHIPIDEA_CLOSE_DONE   /* both endpoints stopped: close the stream, then on_closed */
} u2ps_chipidea_close_action_t;

typedef struct {
  bool active;      /* a close runs */
  uint8_t quiet;    /* SOFs in a row that showed both endpoints stopped */
  uint32_t mask;    /* the endpoints' bits in ENDPTFLUSH, ENDPTPRIME and ENDPTSTAT */
  uint32_t sofs;    /* SOFs checked in this close */
  uint32_t flushes; /* ENDPTFLUSH writes in this close */
  uint32_t open_busy; /* opens that found an endpoint still primed */
} u2ps_chipidea_close_t;

/* The value of ENDPTCTRLn for endpoint ep of a stream with data endpoint
 * ep_out and feedback endpoint ep_fb (0 for none) (RM 42.7.40,
 * p. 2489-2490): the stream's directions isochronous (RXT, TXT 01), enabled
 * and with their data toggle reset (RXE, RXR, TXE, TXR) if 'enable'; a
 * direction the stream does not use typed bulk (10) and disabled, since an
 * unused direction left a control type "causes undefined behavior" (RM
 * note on p. 2489). 0 for ep 0 or above 7. */
uint32_t u2ps_chipidea_ep_ctrl(uint8_t ep, uint8_t ep_out, uint8_t ep_fb, bool enable);

/* A close begins for data endpoint ep_out and feedback endpoint ep_fb (0
 * for none): returns the value to write to ENDPTFLUSH, after ENDPTCTRLn. */
uint32_t u2ps_chipidea_close_begin(u2ps_chipidea_close_t *c, uint8_t ep_out, uint8_t ep_fb);

/* At an SOF during the close, with ENDPTFLUSH, ENDPTPRIME and ENDPTSTAT read
 * then: what to do; *flush gets the bits to flush again for CLOSE_FLUSH.
 * Without a close running it returns CLOSE_DONE and does nothing else. */
u2ps_chipidea_close_action_t u2ps_chipidea_close_sof(u2ps_chipidea_close_t *c,
                                                         uint32_t endptflush, uint32_t endptprime,
                                                         uint32_t endptstat, uint32_t *flush);

/* At an open, with ENDPTPRIME and ENDPTSTAT read then: true if neither
 * endpoint is primed. Otherwise the open must not write the dQHs (RM
 * 42.5.6.5.1, p. 2418); it counts in open_busy, and stream_open refuses. */
bool u2ps_chipidea_close_idle(u2ps_chipidea_close_t *c, uint32_t endptprime,
                                uint32_t endptstat, uint8_t ep_out, uint8_t ep_fb);

#endif /* U2PS_CHIPIDEA_ISO_H */
