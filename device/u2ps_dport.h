/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dport.h: the device port interface, between the device stack and
 * exactly one device port per build. Unstable: it may change in any
 * release before 1.0.0.
 *
 * Rules:
 * - One port per build, chosen at compile time. Upcalls (port -> device
 *   stack) are plain extern functions; there are no function-pointer tables.
 * - The port speaks in meaning (packets, microframes, holes), not registers.
 *   Re-arming stays inside the port.
 * - The port opens one stream: an isochronous OUT data endpoint, polled every
 *   microframe, with its isochronous IN feedback endpoint.
 * - The device stack lends buffers (class memory). The port hands over each
 *   packet where its DMA wrote it.
 *
 * Port handlers: each port names its interrupt handlers in its own
 * configuration header. The app's vector functions call them, so the app can
 * wrap them. All vectors of a port share one priority, so upcalls never nest.
 *
 * The port's configuration header, u2ps_dport_cfg.h, comes with each port
 * and defines its constants. The app sizes class memory with
 * the first four:
 * - U2PS_DPORT_STRIDE(mps): the least stride for buffers of mps bytes;
 * - U2PS_DPORT_ISO_OUT_MIN: the least number of buffers of a stream,
 *   U2PS_DPORT_ISO_OUT_RESERVE + 1;
 * - U2PS_DPORT_ALIGN: the alignment of class memory, in bytes;
 * - U2PS_DPORT_ISO_OUT_RESERVE: the buffers of a stream the port keeps
 *   armed while it is open (DWC2 1; ChipIdea 2), each port citing its
 *   manual: the device stack holds at most count - U2PS_DPORT_ISO_OUT_RESERVE
 *   (see on_missed);
 * - U2PS_DPORT_UFRAME_EXACT: 1 if on_packet's microframe numbers are
 *   exact and on_missed comes immediately before the packet that ends a
 *   hole, or before the stream's end, 0 otherwise (see on_missed);
 * - U2PS_DPORT_EP0_MAX and U2PS_DPORT_EP_MAX; the DMA memory placement
 *   and the reach rule of u2ps_dport_dma_check();
 * - U2PS_DPORT_VBUS_SENSE: 1 if the port senses VBUS itself, for VBUS
 *   mode port (u2ps_dev.h); 0 on every port so far.
 * The library's build constants U2PS_ISO_OUT_MPS_MAX (the largest mps of
 * any stream) and U2PS_ISO_OUT_COUNT_MAX (the most buffers of a stream)
 * size the port's endpoint memory; the device stack never opens a stream
 * beyond them.
 *
 * Device states and requests follow USB 2.0 Chapter 9 (Revision 2.0).
 */
#ifndef U2PS_DPORT_H
#define U2PS_DPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_types.h"

/* Maximum packet size of EP0 at both speeds. High speed allows only 64
 * (USB 2.0 9.6.1); the device stack serves one device descriptor at both
 * speeds, so the port packetises every EP0 stage in 64-byte packets. */
#define U2PS_EP0_MPS 64u

/* The section of the device stack's DMA memory (the control buffer) and of
 * the port's (port memory). It has no load image; the stack and the port
 * zero their memory in it at init, so a linker script may place it NOLOAD
 * and the startup code need not zero it. A linker script places it in DMA
 * memory ahead of its *(.bss*) rule; otherwise that rule takes it.
 * u2ps_dev_init() checks where it lies (u2ps_dport_dma_check). */
#define U2PS_DMA_SECTION ".bss.u2ps_dma"

/* u2ps_miss_t, U2PS_UFRAME_UNKNOWN, u2ps_speed_t (a port reports
 * FULL or HIGH only) and u2ps_dev_err_t are in u2ps_types.h */

/* One stream: isochronous OUT data every microframe, plus its feedback IN */
typedef struct {
  uint8_t ep_out;      /* 1..EP_MAX of the port */
  uint8_t ep_fb;       /* 1..EP_MAX of the port, or 0 for none */
  uint8_t fb_interval; /* bInterval of the feedback endpoint; each port states its limits */
  uint8_t fb_size;     /* 4 (16.16 at high speed, USB 2.0 5.12.4.2) */
  uint32_t fb_value;   /* first feedback value */
  uint16_t mps;        /* wMaxPacketSize bits 10:0 */
  uint16_t stride;     /* bytes from one buffer to the next, >= the port's STRIDE(mps) */
  uint16_t count;      /* number of buffers, from the port's minimum up; the device stack
                          holds at most count - U2PS_DPORT_ISO_OUT_RESERVE */
  uint8_t *mem;        /* class memory: count * stride bytes, aligned as the port
                          requires, in DMA memory */
} u2ps_stream_t;

/* The port's counters. fb_recoveries counts the feedback endpoint's
 * recoveries while the stream's data flows; each port states its rule.
 * fallback_over counts the microframes a port's estimated hole count
 * reported too many and later found (ChipIdea's fallback count); errors
 * counts real faults only. */
typedef struct {
  uint32_t late_completions, late, no_buffer, corrupted, fb_recoveries, errors, fallback_over;
} u2ps_dport_stats_t;

/* ---- Device stack -> port ------------------------------------------------ */

/* App, thread, through u2ps_dev_init(), first of the port calls of every
 * init, with the vectors not masked: true if every byte of 'len' bytes at
 * 'mem', the device stack's DMA memory (its control buffer), and every
 * byte of the port's own DMA memory (port memory) lie where the port's DMA
 * reaches them, by the rule the port's configuration header states;
 * false makes the init return U2PS_DEV_ERR_DMA_MEMORY and do nothing
 * else. It only reads: no hardware start-up. */
bool u2ps_dport_dma_check(const void *mem, uint32_t len);

/* App, thread, before the port's vectors are enabled; detached. Waits, with
 * a timeout, for PHY power and core reset. Port memory lies in DMA memory;
 * the port zeroes it here, so its section needs no zeroing at startup.
 * Returns U2PS_DEV_OK, or why the controller did not come up, mapped onto
 * u2ps_dev_err_t (never U2PS_DEV_ERR_DESCRIPTION). The device stack
 * calls it until it has succeeded once, and u2ps_dport_reinit() after. */
u2ps_dev_err_t u2ps_dport_init(void);

/* A re-init of a port that has started: detached, with no stream (the
 * device stack detaches first). No hardware start-up: the port returns to
 * the state its start left (address 0, EP0 idle, no stream, not
 * suspended), keeping its counters. Thread, vectors masked. */
void u2ps_dport_reinit(void);

/* Attach (true) or detach (false) the pull-up. Thread; the device stack
 * calls it only after a successful u2ps_dport_init(), and attaches only
 * while VBUS is present (u2ps_dev.h, VBUS modes app and bus), with or
 * without the vectors masked. At a detach, called with the vectors
 * masked, the port first stops a stream itself, at once and with bounded
 * waits, as at a bus reset (a close still pending ends here, and no
 * on_closed follows; all class memory returns), reports the stream's tail
 * (u2ps_dev_on_missed, here in thread), and aborts every EP0 stage; then
 * it removes the pull-up and ends test mode. The device stack
 * detaches at u2ps_dev_connect(false), at a re-init and at a VBUS loss
 * (USB 2.0 7.2.1). */
void u2ps_dport_connect(bool on);

/* Device stack, thread: masks (true) and unmasks (false) the port's vectors
 * around port calls made from thread. Masked time counts against the port's
 * interrupt latency limit.
 * - irq_mask(true) is a compiler and CPU barrier: when it returns, no
 *   handler of the port runs, and none has a write in flight (on Cortex-M,
 *   DSB and ISB after the mask). irq_mask(false) is a compiler barrier.
 * - Calls do not nest: the device stack never masks twice without unmasking
 *   in between, so the port keeps no count. */
void u2ps_dport_irq_mask(bool masked);

/* Inside u2ps_dev_on_setup() of SET_ADDRESS, before u2ps_dport_ep0_status().
 * The port applies the address after the status stage (USB 2.0 9.4.6). The
 * device stack enters the Address state already at the SETUP, so a status
 * completion reported after the next SETUP changes nothing. */
void u2ps_dport_set_address(uint8_t addr);

/* In u2ps_dev_on_ep0_status() of SET_FEATURE(TEST_MODE), that is after the
 * status stage (USB 2.0 9.4.9). The port enters test mode 'selector'
 * (1..4, Table 9-7) at once; it must be complete within 3 ms. A power
 * cycle ends it (USB 2.0 7.1.20), and so does u2ps_dport_connect(false),
 * which the device stack also calls at a VBUS loss: without VBUS the
 * device must not drive the bus. */
void u2ps_dport_test_mode(uint8_t selector);

/* EP0 rules for every port:
 * - A SETUP aborts any armed EP0 stage and any STALL; EP0 then NAKs until
 *   the device stack arms a stage for the new request.
 * - An EP0 call from thread (vectors masked) arms nothing while the port
 *   holds a SETUP it has not yet delivered through u2ps_dev_on_setup().
 *   The call belongs to the request that SETUP replaced; the port drops it
 *   and delivers the SETUP once unmasked. Otherwise a deferred reply to a
 *   cancelled request would answer the new one: DWC2 NAKs EP0 on a SETUP,
 *   but arming writes CNAK (RM0431 32.15.50, OTG_DIEPCTL0).
 *   Detection: DWC2 in buffer DMA mode, OTG_DOEPINT0 bit 15 STPKTRX
 *   (RM0431 32.15.58); ChipIdea, ENDPTSETUPSTAT read after the prime
 *   (IMXRT1060RM Rev. 3 42.5.6.4.2.2, p. 2412-2413).
 *   DWC2 narrows the window but cannot close it: STPKTRX may be set only
 *   when the setup stage ends, at the host's first token after the SETUP
 *   (RM0456 73.14.55, p. 3384), and a CNAK written before it can meet that
 *   token. The port checks again after each EP0 arm and takes a stage
 *   back (SNAK, and EPDIS if still enabled) when a SETUP has arrived.
 * - A completion upcall (u2ps_dev_on_ep0_data, u2ps_dev_on_ep0_status)
 *   reports only a stage armed after the last SETUP the port delivered. A
 *   port that finds the completion of an older stage pending together with
 *   a SETUP (ChipIdea serves SETUPs before completions, IMXRT1060RM Rev. 3
 *   42.5.6.7.1, Table 42-75) drops it. Delivered late, it would be taken
 *   for the new request's: a SET_FEATURE(TEST_MODE) would enter test mode
 *   before its own status stage.
 * - Lost ACK (USB 2.0 8.5.3.3): if the host's status OUT comes while the
 *   port still waits for the ACK of the last IN data packet, the port takes
 *   the host's OUT as that ACK. It calls u2ps_dev_on_ep0_data(), NAKs the
 *   status OUT until the device stack arms ep0_status(false) (which it does
 *   inside that upcall), and then calls u2ps_dev_on_ep0_status().
 * - Cache: the control buffer is U2PS_EP0_BUF bytes, 32-byte aligned and
 *   a whole number of 32-byte lines, in section U2PS_DMA_SECTION, which
 *   the app's linker script places in DMA memory.
 *   If that memory is cached, the port cleans the data cache over the data
 *   before an IN stage; before an OUT stage it cleans and invalidates the
 *   buffer's lines, so no dirty line is written back over the received data,
 *   and invalidates them again after it. No other data shares those lines.
 *
 * EP0 data stages. Called in an upcall, or from thread with the vectors
 * masked. One call covers one whole data stage; the port splits it into
 * U2PS_EP0_MPS packets. data lies in DMA memory (the device stack's
 * control buffer) and stays valid until the stage completes.
 * - ep0_in: send len bytes. With zlp, the port ends the stage with a
 *   zero-length packet after the last full packet; the device stack decides
 *   it (USB 2.0 9.4.3, 8.5.3.2). len 0 sends one zero-length packet.
 * - ep0_out: receive len bytes into data.
 * Completion: u2ps_dev_on_ep0_data(). */
void u2ps_dport_ep0_in(const void *data, uint16_t len, bool zlp);
void u2ps_dport_ep0_out(void *data, uint16_t len);

/* EP0 status stage: in = true sends the zero-length IN status packet (no-data
 * and control-write requests); in = false accepts the host's zero-length OUT
 * packet (control-read requests). Completion: u2ps_dev_on_ep0_status().
 * Timing: status within 50 ms of a no-data request, first data within 500 ms
 * (USB 2.0 9.2.6.4; 9.2.6.5 for class requests). */
void u2ps_dport_ep0_status(bool in);

/* EP0 request error: STALL in both directions until the next SETUP
 * (USB 2.0 9.2.7, 8.5.3). */
void u2ps_dport_ep0_stall(void);

/* On SET_INTERFACE to a streaming alternate setting, once the last stream's
 * u2ps_dev_on_closed() has come; also from inside that on_closed, which
 * may open the next stream on the same class memory; or, for a start the
 * app deferred, from thread with the vectors masked (u2ps_dev_stream_open).
 * The device stack calls it only once the app has lent the class memory
 * (on_stream_start returned U2PS_STREAM_OPEN, or the deferred open): the
 * endpoints are enabled here and nowhere before, so no packet arrives
 * before the app is ready. Bounded time. Returns false if s does not fit
 * the port.
 * Cache: if class memory is cached, the port invalidates a buffer's lines
 * before it arms the buffer, and again after the packet has completed,
 * before on_packet, so no line the CPU fetched meanwhile hides the DMA's
 * data (speculative reads can fill a line meanwhile: erratum ERR011573,
 * "Core: Speculative accesses might be performed to memory unmapped in
 * MPU", i.MX RT1060 Chip Errata IMXRT1060CE, silicon A Rev. 1.3 and B
 * Rev. 1.1). */
bool u2ps_dport_stream_open(const u2ps_stream_t *s);

/* On alternate setting 0, a new alternate setting or deconfiguration. Returns
 * at once; the port finishes on its own interrupts, reports the stream's
 * tail (u2ps_dev_on_missed) and then calls u2ps_dev_on_closed(). Class
 * memory returns at on_closed; held packets stay readable until the next
 * open. */
void u2ps_dport_stream_close(void);

/* Any context, including inside u2ps_dev_on_packet(): releases the n
 * oldest held packets, in arrival order. It writes no register: a buffer
 * is always armed, and a freed one is armed at the next completion.
 * Bounded; each port states its cost. With no stream open (after on_closed
 * or a bus reset), or for more packets than are held, it does nothing and
 * counts in errors; the device stack checks both before it calls. */
void u2ps_dport_release(uint16_t n);

/* Device stack, thread, vectors masked, while a stream is open and not
 * closing: u2ps_dev_tick() at each tick that sees the long gap (no packet
 * delivered for more than U2PS_GAP_PAUSE microframes' worth). The port
 * reports, before it returns and with u2ps_dev_on_missed() (here in
 * thread), the holes it holds back now: those after the last delivered
 * packet, up to the last packet received (NO_BUFFER, LATE, CORRUPTED, and
 * NO_PACKET between them where the port knows it), as it does for the
 * stream's tail, and takes them, so that neither the next packet nor the
 * tail reports them again. Holes it can place only with a later packet
 * stay for that packet. Nothing before the stream's first packet. Bounded;
 * no register write. */
void u2ps_dport_report_holes(void);

/* Any context: one store. The new feedback value goes out within two polls. */
void u2ps_dport_fb_set(uint32_t value);

/* The port's counters */
const u2ps_dport_stats_t *u2ps_dport_stats(void);

/* ---- Port -> device stack (upcalls) ----------------------------------------
 * From the port's handlers, never nested, in bus order. */

/* Bus reset. The port has already stopped a stream itself and reported its
 * tail (on_missed); a close still pending ends here, and no on_closed
 * follows. All class memory returns. */
void u2ps_dev_on_reset(void);

/* The speed the reset ended at; after on_reset, before the first SETUP */
void u2ps_dev_on_speed(u2ps_speed_t speed);

void u2ps_dev_on_suspend(void);
void u2ps_dev_on_resume(void);

/* A SETUP packet; 8 bytes, valid during the call. The device stack may answer
 * later from thread (deferred reply); EP0 NAKs after a SETUP until the stack
 * arms a stage. A new SETUP cancels a pending reply. */
void u2ps_dev_on_setup(const uint8_t setup[8]);

/* The data stage armed by ep0_in or ep0_out has completed; len bytes moved.
 * Only for a stage armed after the last delivered SETUP (EP0 rules above). */
void u2ps_dev_on_ep0_data(uint16_t len);

/* The status stage armed by ep0_status has completed. Only for a stage
 * armed after the last delivered SETUP (EP0 rules above). */
void u2ps_dev_on_ep0_status(void);

/* A packet of the stream, after reception continues. data is in class memory
 * and is held until released. Several calls per interrupt are allowed. */
void u2ps_dev_on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe);

/* A hole of 'uframes' microframes, reported once (a run of NO_PACKET
 * microframes longer than 2^14 is counted modulo 2^14); it may be split
 * into consecutive reports with different reasons, and one longer than
 * 65,535 microframes into several reports:
 * - with U2PS_DPORT_UFRAME_EXACT, immediately before the packet that ends
 *   the hole, or before the stream's end;
 * - without it, as soon as the port is certain, at the latest with the first
 *   packet known to lie after the hole; its place among that frame's
 *   packets is unknown;
 * - never for microframes before a stream's first packet, nor after the
 *   last packet the port received. While a stream is open the port keeps
 *   a buffer armed (U2PS_DPORT_ISO_OUT_RESERVE), so every packet that
 *   arrives is delivered or counted NO_BUFFER in its own microframe; a
 *   microframe without one is NO_PACKET. Holes after the last delivered
 *   packet, up to the last packet received, are reported before on_closed
 *   and on_reset, and inside u2ps_dport_connect(false) and
 *   u2ps_dport_report_holes() (thread, vectors masked). Packets the host
 *   sends before the open are lost without a
 *   report; the device stack holds SET_INTERFACE's status stage until the
 *   open, so that happens only to a start that has passed U2PS_START_MAX
 *   (u2ps_dev.h). DWC2
 *   receives in microframes of one parity at a time (RM0456 73.15.6, p.
 *   3445); its data endpoint's parity follows the bus at each EOPF until
 *   the first packet, however late it comes (u2ps_dwc2_iso.h), so a first
 *   packet is lost only where a follow write lands after its microframe
 *   has begun, which the port counts (out_follow_late in
 *   u2ps_dwc2_dcd_diag()), or with the follow turned off for a bench
 *   comparison. Whether the first packet is ever lost on a board is still
 *   to be measured. */
void u2ps_dev_on_missed(uint16_t uframes, u2ps_miss_t why);

/* The stream closed by u2ps_dport_stream_close() has stopped */
void u2ps_dev_on_closed(void);

#endif /* U2PS_DPORT_H */
