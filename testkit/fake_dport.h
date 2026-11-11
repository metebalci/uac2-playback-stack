/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * fake_dport.h: a fake device port for the PC tests and the PC test kit
 * (u2ps_testkit_dev.h, which wraps it for integrators). It implements every
 * call of u2ps_dport.h, records each in order with its arguments and the
 * context it came from, and checks the port interface's contracts. Tests
 * inject bus events, SETUP packets and stage completions; each injection
 * runs as the port's interrupt would, calling the device stack's upcalls.
 *
 * On top of it, u2ps_fake_host_control() plays the host's side of one whole control
 * transfer, packet by packet, as USB 2.0 5.5.3 and 8.5.3 describe it.
 *
 * A detach (connect(false)) stops a stream and EP0 as a bus reset does,
 * ends test mode, and must come with the vectors masked; reinit only after
 * a successful init, detached, with the vectors masked.
 *
 * VBUS: the fake keeps the level on the cable. In VBUS mode app it plays
 * the app's poll too, reporting each level it sets with u2ps_dev_vbus(),
 * and reports a pull-up on without VBUS (USB 2.0 7.1.5.1) as a contract
 * violation.
 *
 * The stream side models a port that keeps the stream's class memory as a
 * ring of buffers: each injected packet goes into the next buffer and is
 * held until released. It keeps U2PS_DPORT_ISO_OUT_RESERVE (1) buffer
 * armed, as DWC2 does: a packet that comes while the device stack holds
 * count - 1 is a reserve packet, not delivered, a NO_BUFFER microframe;
 * the microframes without a packet between reserve packets are NO_PACKET.
 * Both are reported before the next delivered packet, or, as the stream's
 * tail, before on_closed, before on_reset and inside connect(false), or
 * inside u2ps_dport_report_holes(), which must come from thread with the
 * vectors masked while a stream is open (a port violation otherwise).
 */
#ifndef U2PS_FAKE_DPORT_H
#define U2PS_FAKE_DPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_dport.h"
#include "u2ps_dport_cfg.h"

typedef enum {
  U2PS_FAKE_INIT,
  U2PS_FAKE_REINIT,
  U2PS_FAKE_CONNECT,
  U2PS_FAKE_IRQ_MASK,
  U2PS_FAKE_SET_ADDRESS,
  U2PS_FAKE_TEST_MODE,
  U2PS_FAKE_EP0_IN,
  U2PS_FAKE_EP0_OUT,
  U2PS_FAKE_EP0_STATUS,
  U2PS_FAKE_EP0_STALL,
  U2PS_FAKE_STREAM_OPEN,
  U2PS_FAKE_STREAM_CLOSE,
  U2PS_FAKE_RELEASE,
  U2PS_FAKE_REPORT_HOLES,
  U2PS_FAKE_FB_SET,
  U2PS_FAKE_STATS,
  U2PS_FAKE_DMA_CHECK, /* len: none; arg: what it returned */
  U2PS_FAKE_MISSING /* u2ps_fake_get() found no such call */
} u2ps_fake_kind_t;

/* Where a port call came from */
typedef enum {
  U2PS_FAKE_FROM_THREAD,  /* thread, vectors not masked */
  U2PS_FAKE_FROM_MASKED,  /* thread, vectors masked */
  U2PS_FAKE_FROM_RESET,   /* inside u2ps_dev_on_reset() */
  U2PS_FAKE_FROM_SETUP,   /* inside u2ps_dev_on_setup() */
  U2PS_FAKE_FROM_DATA,    /* inside u2ps_dev_on_ep0_data() */
  U2PS_FAKE_FROM_STATUS,  /* inside u2ps_dev_on_ep0_status() */
  U2PS_FAKE_FROM_STREAM,  /* inside on_packet, on_missed or on_closed */
  U2PS_FAKE_FROM_OTHER    /* inside another upcall */
} u2ps_fake_from_t;

typedef struct {
  u2ps_fake_kind_t kind;
  u2ps_fake_from_t from;
  uint32_t arg; /* connect/irq_mask/ep0_status: bool; set_address, test_mode, release, fb_set;
                   stream_open: fb_value */
  uint16_t len; /* ep0_in, ep0_out */
  bool zlp;     /* ep0_in */
  bool dropped; /* an EP0 call the port dropped: a SETUP was held */
} u2ps_fake_call_t;

#define U2PS_FAKE_CALLS_MAX 512u

extern u2ps_fake_call_t u2ps_fake_calls[U2PS_FAKE_CALLS_MAX];
extern unsigned u2ps_fake_ncalls;

/* Port violations the fake has seen: rules of the port interface broken,
 * or the test's mistakes (each is also printed) */
extern unsigned u2ps_fake_violations;

/* What EP0 does with the host's next token */
typedef enum {
  U2PS_FAKE_EP0_NAK,        /* nothing armed */
  U2PS_FAKE_EP0_IN_DATA,    /* ep0_in armed */
  U2PS_FAKE_EP0_OUT_DATA,   /* ep0_out armed */
  U2PS_FAKE_EP0_STATUS_IN,  /* ep0_status(true) armed */
  U2PS_FAKE_EP0_STATUS_OUT, /* ep0_status(false) armed */
  U2PS_FAKE_EP0_STALLED
} u2ps_fake_ep0_t;

extern u2ps_fake_ep0_t u2ps_fake_ep0;
extern uint8_t u2ps_fake_address;   /* the address the port answers to */
extern int u2ps_fake_test_selector; /* -1 until test_mode */
extern u2ps_dev_err_t u2ps_fake_init_err; /* what u2ps_dport_init() returns */
/* What u2ps_dport_dma_check() returns (true until a test sets it), and the
 * memory the last call named. The fake reports an EP0 data stage outside
 * that memory as a port violation: the stack's DMA memory is what it
 * checked. Not cleared by u2ps_fake_reset_all(): the memory lasts as the
 * port's start does. */
extern bool u2ps_fake_dma_ok;
extern const void *u2ps_fake_dma_mem;
extern uint32_t u2ps_fake_dma_len;
extern bool u2ps_fake_attached;     /* the pull-up is on */

/* VBUS on the cable; present until a test sets it */
extern bool u2ps_fake_vbus;
/* VBUS goes (false) or comes (true). In VBUS mode app also the app's poll:
 * u2ps_dev_vbus(present), then a check that no pull-up is on without
 * VBUS. Not reset by u2ps_fake_reset_all(): VBUS is a fact of the board. */
void u2ps_fake_vbus_set(bool present);
/* The vectors are masked now (thread) */
bool u2ps_fake_masked(void);
/* Called once, at the next irq_mask(true), right after the mask, and then
 * cleared: code that preempts the stack's masked region, as an interrupt
 * of another source or a second thread would (NULL: none) */
extern void (*u2ps_fake_on_mask)(void);

/* Clear the call log (not the port's state) */
void u2ps_fake_clear_log(void);
/* Clear everything: log, EP0, address, test mode, violations; not the
 * port's start, which, as the device stack's record of it, lasts for the
 * process: once u2ps_dport_init() has succeeded, re-inits use reinit */
void u2ps_fake_reset_all(void);

/* Index of the first logged call of 'kind' at or after 'from', or -1 */
int u2ps_fake_find(u2ps_fake_kind_t kind, unsigned from);
/* The first logged call of 'kind' at or after 'from'; if there is none, a
 * record of kind U2PS_FAKE_MISSING with every field all ones */
u2ps_fake_call_t u2ps_fake_get(u2ps_fake_kind_t kind, unsigned from);
/* Number of logged calls of 'kind' */
unsigned u2ps_fake_count(u2ps_fake_kind_t kind);

/* Bus events and EP0 completions, each run as the port's interrupt */
void u2ps_fake_bus_reset(u2ps_speed_t speed); /* on_reset, then on_speed */
void u2ps_fake_suspend(void);
void u2ps_fake_resume(void);
void u2ps_fake_setup(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                     uint16_t wLength);
/* The host has taken the armed IN data stage */
void u2ps_fake_ep0_in_done(void);
/* The host has sent len bytes of the armed OUT data stage */
void u2ps_fake_ep0_out_done(const void *data, uint16_t len);
/* The armed status stage has completed; a new address applies now */
void u2ps_fake_ep0_status_done(void);

/* The armed status stage completes on the bus (a new address applies), but
 * the port reports it only at u2ps_fake_report_status(), as a port does when a
 * SETUP and the completion are pending in one interrupt and it serves the
 * SETUP first. If a SETUP was delivered in between, the completion is stale:
 * the fake drops it, as u2ps_dport.h requires, and counts it in
 * u2ps_fake_stale_dropped; with u2ps_fake_deliver_stale it delivers it anyway and
 * reports a contract violation. */
void u2ps_fake_ep0_status_done_unreported(void);
void u2ps_fake_report_status(void);
extern bool u2ps_fake_deliver_stale;
extern unsigned u2ps_fake_stale_dropped;

/* The ACK of the last IN data packet is lost and the host's status OUT comes
 * instead (USB 2.0 8.5.3.3): the port reports the data stage done, waits for
 * ep0_status(false), then completes the status stage */
void u2ps_fake_ep0_in_ack_lost(void);

/* A SETUP arrives at the next irq_mask(true): the port holds it, EP0 calls
 * from thread arm nothing (they are logged with 'dropped'), and the port
 * delivers it at irq_mask(false). For u2ps_fake_host_resume(), this SETUP is the
 * host's current transfer. */
void u2ps_fake_setup_on_mask(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                             uint16_t wIndex, uint16_t wLength);
/* The same at the nth irq_mask(true) from now (1: the next) */
void u2ps_fake_setup_on_mask_nth(unsigned nth, uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex, uint16_t wLength);

/* The most bytes of the device stack's control buffer that changed within
 * one masked region (irq_mask(true) to irq_mask(false)), before a held
 * SETUP is delivered: what the thread copied with the vectors masked.
 * u2ps_fake_reset_all() zeroes it. */
extern unsigned u2ps_fake_masked_bytes_max;

/* ---- The stream ------------------------------------------------------------ */

/* The fake checks stream_open against its u2ps_dport_cfg.h and against
 * U2PS_ISO_OUT_MPS_MAX and U2PS_ISO_OUT_COUNT_MAX */

typedef enum {
  U2PS_FAKE_ST_IDLE,   /* no stream; on_closed has come, or a bus reset */
  U2PS_FAKE_ST_OPEN,   /* stream_open succeeded */
  U2PS_FAKE_ST_CLOSING /* stream_close called; u2ps_fake_closed() not yet */
} u2ps_fake_stream_state_t;

extern u2ps_fake_stream_state_t u2ps_fake_stream_state;
extern u2ps_stream_t u2ps_fake_stream; /* as the last successful stream_open passed it */
extern bool u2ps_fake_open_result;     /* what u2ps_dport_stream_open() returns, if valid */
extern uint32_t u2ps_fake_fb;          /* the value the feedback endpoint sends at its next poll */
extern unsigned u2ps_fake_held;        /* packets delivered and not yet released */
extern const uint8_t *u2ps_fake_last_data; /* where the last injected packet lies */
extern u2ps_dport_stats_t u2ps_fake_stats; /* what u2ps_dport_stats() returns */

/* A packet of len bytes in microframe uframe arrives, as the port's
 * interrupt: into the next buffer of class memory (filled with the bytes
 * seq, seq + 1, ... of a running counter), then on_packet. If the device
 * stack holds count - U2PS_DPORT_ISO_OUT_RESERVE, it is a reserve packet:
 * not delivered, NO_BUFFER, reported before the next delivered packet or
 * in the stream's tail. */
void u2ps_fake_packet(uint16_t len, u2ps_uframe_t uframe);
/* The same with the packet's bytes: len bytes from data go into the next
 * buffer (a loopback with the host stack) */
void u2ps_fake_packet_bytes(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe);
/* A hole report, as the port's interrupt */
void u2ps_fake_missed(uint16_t uframes, u2ps_miss_t why);
/* The close has ended, as the port's interrupt: the stream's tail, then
 * on_closed */
void u2ps_fake_closed(void);

/* The host's view of one control transfer */
typedef enum {
  U2PS_FAKE_HOST_OK,
  U2PS_FAKE_HOST_STALL, /* request error in the data or status stage */
  U2PS_FAKE_HOST_NAK,   /* EP0 still NAKs: the answer is pending */
  U2PS_FAKE_HOST_ERROR  /* a protocol fault: see the printed reason */
} u2ps_fake_host_result_t;

/* One whole control transfer: SETUP, data stage (packet by packet: the host
 * stops at wLength bytes or at a short packet) and status stage. For a
 * device-to-host request, the data lands in 'data' and its length in
 * '*actual'; for host-to-device, 'data' holds wLength bytes to send. */
u2ps_fake_host_result_t u2ps_fake_host_control(uint8_t bmRequestType, uint8_t bRequest,
                                               uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                               void *data, uint16_t *actual);

/* Finish a transfer that u2ps_fake_host_control() left NAKing, once the stack has
 * armed a stage: as u2ps_fake_host_control() from the data stage on. */
u2ps_fake_host_result_t u2ps_fake_host_resume(void *data, uint16_t *actual);

#endif /* U2PS_FAKE_DPORT_H */
