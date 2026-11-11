/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_hport.h: the host port interface, between the host stack and exactly
 * one host port per build. Unstable: it may change in any release before
 * 1.0.0.
 *
 * Rules, as on the device side (u2ps_dport.h):
 * - One port per build, chosen at compile time. Upcalls (port -> host
 *   stack) are plain extern functions; there are no function-pointer tables.
 * - The port speaks in meaning (packets, microframes, holes), not registers.
 *   Re-arming stays inside the port: it sends from its own queue of packets.
 * - The port drives one root port with one device (no hubs) at address 0
 *   until SET_ADDRESS, then at address 1, at high, full or low speed (USB
 *   2.0 7.1.2.3: a host's port supports all three).
 * - It runs one stream at a time, at high speed only: an isochronous OUT
 *   data endpoint served every microframe, with or without an isochronous
 *   IN feedback endpoint. At full and low speed the host stack only
 *   identifies the device, or runs the test device (u2ps_host.h), with
 *   control transfers, suspend and resume.
 * - Frames: at high speed the port sends a SOF every microframe (125 us);
 *   at full speed one every frame (1 ms), and at low speed a keep-alive
 *   every frame (USB 2.0 7.1.7.6, 11.8.4.1). The host stack times every
 *   wait of its own in milliseconds (u2ps_host_tick), at any speed.
 *
 * Context:
 * - All of a port's vectors share one priority, so upcalls never nest.
 * - The host stack calls the port from thread only with the vectors masked
 *   (u2ps_hport_irq_mask), and needs no mask for calls made inside an
 *   upcall. Masked time counts against the port's latency limit.
 * - Port handlers: each port names its interrupt handlers in its own
 *   configuration header; the app's vector functions call them.
 *
 * The port's configuration header, u2ps_hport_cfg.h, comes with each port
 * and defines its constants:
 * - U2PS_HPORT_ALIGN: the alignment in bytes of every packet the app
 *   queues; a packet that starts elsewhere is not sent without a copy
 *   (u2ps_host.h, zero copy);
 * - U2PS_HPORT_LEAD: how many microframes ahead the port arms a packet;
 * - U2PS_HPORT_UFRAME_EXACT: 1 if on_sent's microframe numbers are exact
 *   and each hole is reported immediately before the on_sent that ends it;
 * and the reach rule of u2ps_hport_dma_check().
 *
 * VBUS is not the port's: the controllers here cannot supply it, so the
 * app senses it and the board supplies it (u2ps_host.h, VBUS).
 *
 * References: USB 2.0 Specification, Revision 2.0.
 */
#ifndef U2PS_HPORT_H
#define U2PS_HPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_types.h"

/* The section of the host stack's DMA memory (the SETUP packet and the
 * control buffer) and of the port's (port memory), the device stack's
 * section too (u2ps_dport.h). It has no load image; the stack and the port
 * zero their memory in it at init, so a linker script may place it NOLOAD
 * and the startup code need not zero it. A linker script places it in DMA
 * memory ahead of its *(.bss*) rule, otherwise that rule takes it.
 * u2ps_host_init() checks where it lies (u2ps_hport_dma_check). */
#define U2PS_DMA_SECTION ".bss.u2ps_dma"

/* One stream: isochronous OUT data every microframe, with or without
 * feedback */
typedef struct {
  uint8_t addr;        /* device address (always 1: root port, no hubs) */
  uint8_t ep_out;      /* data endpoint number 1..15 */
  uint8_t ep_fb;       /* feedback endpoint number 1..15; 0: none (nominal rate) */
  uint8_t fb_interval; /* bInterval: one poll every 2^(fb_interval-1) microframes */
  uint16_t mps;        /* data wMaxPacketSize bits 10:0, <= 1024 (one transaction) */
  uint16_t fb_mps;     /* feedback wMaxPacketSize bits 10:0, >= 4 */
} u2ps_hstream_t;

/* How a control transfer ended (u2ps_host_on_control) */
typedef enum {
  U2PS_HPORT_CONTROL_OK,      /* all stages completed */
  U2PS_HPORT_CONTROL_STALL,   /* the device stalled a stage: a request error (USB 2.0 8.5.3.4) */
  U2PS_HPORT_CONTROL_ERROR,   /* three transaction errors in a row (USB 2.0 10.2.6) */
  U2PS_HPORT_CONTROL_ABORTED  /* u2ps_hport_control_abort() ended it */
} u2ps_hport_control_t;

/* The port's counters */
typedef struct {
  uint32_t sent;             /* packets sent */
  uint32_t late_completions; /* sent packets whose re-arm read a later microframe */
  uint32_t late;             /* LATE microframes */
  uint32_t no_buffer;        /* NO_BUFFER microframes */
  uint32_t corrupted;        /* CORRUPTED packets */
  uint32_t fb_polls;         /* feedback polls made */
  uint32_t fb_missed;        /* feedback polls without a valid packet, or skipped */
  uint32_t control_retries;  /* control transactions retried after an error */
  uint32_t errors;           /* real faults only: queue overruns, AHB errors, babble */
} u2ps_hport_stats_t;

/* ---- Host stack -> port --------------------------------------------------- */

/* App, thread, through u2ps_host_init(), first of the port calls of every
 * init, once for each block of the host stack's DMA memory: true if every
 * byte of 'len' bytes at 'mem', and every byte of the port's own DMA
 * memory (port memory), lie where the port's DMA reaches them, by the rule
 * the port's configuration header states; false makes the init return
 * U2PS_HOST_ERR_DMA_MEMORY and do nothing else. It only reads: no hardware
 * start-up; the vectors may be masked or not. */
bool u2ps_hport_dma_check(const void *mem, uint32_t len);

/* A range of memory the port's DMA reaches, [start, end) */
typedef struct {
  uintptr_t start;
  uintptr_t end;
} u2ps_hport_range_t;

/* The most ranges u2ps_hport_dma_ranges() reports */
#define U2PS_HPORT_RANGES_MAX 4u

/* App, thread, through u2ps_host_init() in a build with
 * U2PS_CONTRACT_CHECKS=1, once after each u2ps_hport_init() that returned
 * U2PS_HOST_OK: writes the memory where the port's DMA reaches a queued
 * packet now, by the same rule as u2ps_hport_dma_check(), as at most
 * U2PS_HPORT_RANGES_MAX ranges in address order, none touching the next
 * (adjacent memory is one range), and returns how many. The host stack
 * keeps them until its next init and checks each queued packet against
 * them (U2PS_CONTRACT_HOST_QUEUE_DATA). It only reads. */
uint32_t u2ps_hport_dma_ranges(u2ps_hport_range_t *ranges);

/* App, thread, through u2ps_host_init(): PHY, core reset, host mode, the
 * port power bit; then idle, with no session, reporting a connect. The
 * first call comes before the port's vectors are enabled. A second call
 * resets the controller (DWC2 CSRST, ChipIdea USBCMD.RST), with the vectors
 * masked. On DWC2 it sets OTG_GUSBCFG.FHMOD and waits at least 25 ms
 * (RM0456 Rev 7, p. 3324), and sets OTG_GCCFG.FORCEHOSTPD, since CN15 of
 * the NUCLEO has no ID pin (p. 3347); OTG_HPRT.PPWR drives no VBUS (p.
 * 3358). Each call zeroes port memory after the core reset, so its
 * section needs no zeroing at startup. Returns U2PS_HOST_OK, or why the
 * controller did not come up. */
u2ps_host_err_t u2ps_hport_init(void);

/* Host stack, thread: masks (true) and unmasks (false) the port's vectors,
 * as u2ps_dport_irq_mask(): a compiler and CPU barrier; calls do not nest. */
void u2ps_hport_irq_mask(bool masked);

/* Starts (true) or ends (false) a reset segment of the root port. The host
 * stack times the reset: 50 ms in all (TDRSTR, USB 2.0 7.1.7.5).
 * - DWC2 runs one segment, which reset(false) ends; on_enabled follows.
 * - ChipIdea ends each segment itself, and reset(false) does nothing. If
 *   on_enabled comes before the 50 ms have passed, the host stack calls
 *   reset(true) again inside that upcall. Each segment lasts at least 10 ms
 *   (TDRST). */
void u2ps_hport_reset(bool on);

/* Stops SOFs and every transfer at once, with bounded waits: no on_control,
 * on_sent or on_closed follows for what was in flight, and the stream's
 * packets are the app's again. It also ends a suspend or a resume, at any
 * point. The port keeps reporting a disconnect and a connect. */
void u2ps_hport_disable(void);

/* Test mode 'selector' 1..5 (USB 2.0 Table 9-7, 7.1.20) on the root port,
 * at once; called with no session, after u2ps_hport_disable(). DWC2:
 * OTG_HPRT.PTCTL (RM0456 p. 3358), 5 is Test_Force_Enable; ChipIdea:
 * PORTSC1.PTC (IMXRT1060RM Rev. 3, p. 2474), 5 is FORCE_ENABLE_HS. Only a
 * power cycle or u2ps_hport_init() ends it. */
void u2ps_hport_test_mode(uint8_t selector);

/* One whole control transfer to device 'addr': SETUP, the data stage of
 * wLength bytes (setup bytes 6..7) into or from data, and the status stage,
 * packetised by mps0 bytes, the maximum packet size of the device's
 * endpoint 0 (USB 2.0 5.5.3, 9.6.1): 64 at high speed; 8, 16, 32 or 64 at
 * full speed; 8 at low speed. Until the device descriptor is read the host
 * stack passes 64 at high speed and 8 at full and low speed. setup and
 * data lie in the host stack's DMA memory (U2PS_DMA_SECTION) and stay
 * unchanged until on_control. One transfer at a time, never while the
 * port is suspended or resuming. The port retries NAKs without limit; it
 * retries a transaction error too, and the third in a row ends the
 * transfer with ERROR (USB 2.0 4.5.2, 10.2.6), the count restarting after
 * a transaction without a transmission error. Completion: on_control.
 * DWC2: OTG_HCCHARx.MPSIZ is mps0, and LSDEV is set at low speed (RM0456
 * Rev 7, 73.14.31). */
void u2ps_hport_control(uint8_t addr, uint8_t mps0, const uint8_t setup[8], void *data);

/* The SETUP stage alone of a control transfer to device 'addr' (as
 * u2ps_hport_control otherwise), for the test device's
 * SINGLE_STEP_SET_FEATURE (u2ps_host.h): on_control(OK, 0) once the device
 * has acknowledged the SETUP, or another result if it did not; the port
 * then sends nothing to the device but SOFs until
 * u2ps_hport_control_data(). DWC2: a channel transfer of the SETUP alone,
 * DPID SETUP (RM0456 73.14.35); ChipIdea: a qH whose qTD list holds the
 * SETUP qTD alone (IMXRT1060RM Rev. 3, 42.5.2.5.3). */
void u2ps_hport_control_setup(uint8_t addr, uint8_t mps0, const uint8_t setup[8]);

/* The data stage of the transfer whose SETUP stage u2ps_hport_control_setup()
 * sent, into or from data (as u2ps_hport_control), then its status stage.
 * Completion: on_control(result, len), as for a whole transfer. */
void u2ps_hport_control_data(void *data);

/* Ends the control transfer, or the stage of one, in flight; asynchronous:
 * exactly one on_control follows, ABORTED unless the transfer ended first.
 * DWC2: CHDIS, then CHH; ChipIdea: the asynchronous-advance doorbell. */
void u2ps_hport_control_abort(void);

/* Suspends the root port (USB 2.0 7.1.7.6, 11.9): no SOF, no keep-alive
 * from now on, at once. Called only with the port enabled and nothing in
 * flight: no control transfer and no stream (the host stack's rule). The
 * port then watches for a remote wakeup, a K from the device
 * (u2ps_host_on_wakeup). DWC2: OTG_HPRT.PSUSP (RM0456 73.14.30, p. 3359),
 * written with its write-1-to-clear bits PENA, PCDET, PENCHNG and POCCHNG
 * at 0, and WKUPINT unmasked; ChipIdea: PORTSC1.SUSP (IMXRT1060RM Rev. 3,
 * p. 2476). */
void u2ps_hport_suspend(void);

/* Resume of the suspended root port (USB 2.0 7.1.7.7). resume(true)
 * starts driving the resume K and returns false; the host stack times it
 * (at least 20 ms, TDRSMDN), then calls resume(false) on every tick until
 * it returns true: the K ends where the port ends it, and true says the
 * port is out of resume and sends SOFs (or keep-alives) again. On DWC2
 * resume(false) clears OTG_HPRT.PRES (the application times the resume,
 * RM0456 p. 3359) and returns true once PRES reads 0; on ChipIdea the
 * controller times the K and clears PORTSC1.FPR itself (IMXRT1060RM Rev. 3,
 * p. 2476), so resume(false) only reads FPR and returns true once it is 0.
 * After a remote wakeup (u2ps_host_on_wakeup) the port already drives the
 * K; the host stack then calls only resume(false). Read-modify-writes of
 * OTG_HPRT write its write-1-to-clear bits as 0, as for suspend. */
bool u2ps_hport_resume(bool on);

/* Opens the stream: the port sends one packet per microframe from its
 * queue, from the first microframe it can reach after the open, and polls
 * the feedback endpoint (if any) once per period in poll microframes it
 * chooses. Returns false if s does not fit the port. */
bool u2ps_hport_stream_open(const u2ps_hstream_t *s);

/* Queues the next packet of the stream, in order: len bytes at data, which
 * lies in DMA memory, starts on U2PS_HPORT_ALIGN and stays untouched until
 * its on_sent. In an upcall, or from thread with the vectors masked. On
 * cached memory the port cleans the packet's lines before its DMA reads
 * them. The queue holds at most U2PS_HOST_QUEUE_MAX packets; the host stack
 * checks it, and the port counts an excess in errors. Bounded cost, which
 * each port states. */
void u2ps_hport_queue(const uint8_t *data, uint16_t len);

/* Closes the stream; asynchronous: on_closed follows, and packets not yet
 * reported return there */
void u2ps_hport_stream_close(void);

/* The port's counters */
const u2ps_hport_stats_t *u2ps_hport_stats(void);

/* ---- Port -> host stack (upcalls) -----------------------------------------
 * From the port's handlers, never nested, in bus order. */

/* A device has connected to the root port */
void u2ps_host_on_connect(void);

/* The device has disconnected. The port has already stopped the stream and
 * any control transfer: no on_closed and no on_control follow. */
void u2ps_host_on_disconnect(void);

/* A reset segment has ended and the port is enabled at 'speed' (HIGH,
 * FULL or LOW: DWC2 OTG_HPRT.PSPD, RM0456 p. 3358; ChipIdea PORTSC1.PSPD) */
void u2ps_host_on_enabled(u2ps_speed_t speed);

/* The device signalled a remote wakeup, a K, while the port was suspended
 * (USB 2.0 7.1.7.7), and the port already drives the resume K itself
 * (DWC2: OTG_GINTSTS.WKUPINT, and PRES sets itself, RM0456 p. 3359;
 * ChipIdea: a J-to-K transition sets PORTSC1.FPR, IMXRT1060RM Rev. 3,
 * p. 2476). The host stack times the resume from here and calls
 * resume(false) as above. */
void u2ps_host_on_wakeup(void);

/* The hardware has disabled the port (port babble; DWC2 needs a soft reset,
 * RM0456 p. 3426). As at a disconnect, no on_closed and no on_control
 * follow. On DWC2 the port first resets its core inside the OTG_HS
 * handler: one pass of about 26 ms, at most 66 ms, in which thread code
 * and every interrupt at or below the USB priority wait. Give audio DMA,
 * watchdog feeds and safety polls a higher priority or a 66 ms margin. */
void u2ps_host_on_disabled(void);

/* The control transfer has ended; len bytes moved in its data stage */
void u2ps_host_on_control(u2ps_hport_control_t result, uint16_t len);

/* The oldest queued packet has gone out in microframe 'uframe' (or
 * U2PS_UFRAME_UNKNOWN); its memory returns. Every queued packet gets
 * exactly one on_sent; a packet that did not cross the bus intact gets
 * on_missed(1, CORRUPTED) at its own microframe first, and is not sent
 * again. */
void u2ps_host_on_sent(u2ps_uframe_t uframe);

/* A hole of 'uframes' microframes without a packet, for reason 'why'
 * (LATE, NO_BUFFER or CORRUPTED; never NO_PACKET), reported once,
 * immediately before the on_sent that ends it. Microframes before the
 * first packet, after the last one, and drained microframes are no hole.
 * A known limit of the DWC2 host port: it measures a run of microframes
 * with its queue dry as a signed 14-bit microframe distance
 * (u2ps_uframe.h), so a run of 8,192 microframes (1.024 s) or more counts
 * modulo 16,384, and as no hole (or no drained microframes) when that
 * remainder is 8,192 or more. */
void u2ps_host_on_missed(uint16_t uframes, u2ps_miss_t why);

/* A feedback poll in microframe 'uframe' brought len bytes, which lie in
 * port memory and are valid during the call */
void u2ps_host_on_fb(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe);

/* A feedback poll brought no valid packet (NO_PACKET), or was skipped
 * because the port could not arm it in time (LATE); the next poll stays
 * one period later */
void u2ps_host_on_fb_missed(u2ps_uframe_t uframe, u2ps_miss_t why);

/* The stream closed by u2ps_hport_stream_close() has stopped */
void u2ps_host_on_closed(void);

#endif /* U2PS_HPORT_H */
