/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * fake_hport.h: a fake host port for the PC tests and the PC test kit
 * (u2ps_testkit_host.h, which wraps it for integrators). It implements every call
 * of u2ps_hport.h, records each in order with its arguments, the context it
 * came from and the test's time, and checks the port interface's contracts.
 * Tests inject a connect, a disconnect, a disabled port or a remote
 * wakeup, and step the time; each injection and each completion runs as
 * the port's interrupt would, calling the host stack's upcalls.
 *
 * The device behind the root port is a function (u2ps_hfake_device) that
 * answers each control transfer; host_support.h has one that answers from
 * fixtures, and the round-trip test one that passes the transfer to the
 * device stack.
 *
 * Time: u2ps_hfake_step() advances it by 1 ms. At each step the fake first
 * completes what is due (a control transfer, an abort, the end of a reset
 * segment), then calls u2ps_host_tick() with the new time (or u2ps_hfake_tick).
 */
#ifndef U2PS_FAKE_HPORT_H
#define U2PS_FAKE_HPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_hport.h"
#include "u2ps_hport_cfg.h"

/* The kinds of port call in the log; irq_mask is checked and counted
 * (u2ps_hfake_masks), not logged */
typedef enum {
  U2PS_HFAKE_INIT,
  U2PS_HFAKE_RESET,
  U2PS_HFAKE_DISABLE,
  U2PS_HFAKE_TEST_MODE,
  U2PS_HFAKE_CONTROL,
  U2PS_HFAKE_CONTROL_SETUP, /* the SETUP stage alone */
  U2PS_HFAKE_CONTROL_DATA,  /* the data and status stages after it */
  U2PS_HFAKE_CONTROL_ABORT,
  U2PS_HFAKE_SUSPEND,
  U2PS_HFAKE_RESUME,        /* arg: on; ret: what it returned */
  U2PS_HFAKE_STREAM_OPEN,
  U2PS_HFAKE_QUEUE,
  U2PS_HFAKE_STREAM_CLOSE,
  U2PS_HFAKE_STATS,
  U2PS_HFAKE_DMA_CHECK, /* arg: the len checked; ret: what it returned */
  U2PS_HFAKE_MISSING /* u2ps_hfake_get() found no such call */
} u2ps_hfake_kind_t;

/* Where a port call came from */
typedef enum {
  U2PS_HFAKE_FROM_THREAD, /* thread, vectors not masked */
  U2PS_HFAKE_FROM_MASKED, /* thread, vectors masked */
  U2PS_HFAKE_FROM_UPCALL  /* inside an upcall */
} u2ps_hfake_from_t;

/* A SETUP packet's fields (USB 2.0 9.3, Table 9-2) */
typedef struct {
  uint8_t bmRequestType;
  uint8_t bRequest;
  uint16_t wValue;
  uint16_t wIndex;
  uint16_t wLength;
} u2ps_hfake_setup_t;

typedef struct {
  u2ps_hfake_kind_t kind;
  u2ps_hfake_from_t from;
  uint32_t ms;         /* the test's time at the call */
  uint32_t arg;        /* reset, resume: bool; test_mode: selector; control, control_setup:
                          address */
  uint8_t mps0;             /* control, control_setup: the packet size of endpoint 0 */
  bool ret;                 /* resume: what it returned */
  u2ps_hfake_setup_t setup; /* control, control_setup */
  /* control, control_setup, control_data: when on_control came; 0xFFFFFFFF
   * if it never did */
  uint32_t done_ms;
  u2ps_hport_control_t result; /* control, control_setup, control_data: what on_control reported */
  uint16_t len;                /* and its len */
} u2ps_hfake_call_t;

#define U2PS_HFAKE_CALLS_MAX 512u

extern u2ps_hfake_call_t u2ps_hfake_calls[U2PS_HFAKE_CALLS_MAX];
extern unsigned u2ps_hfake_ncalls;

/* Port violations seen: rules of the port interface broken, or the
 * test's mistakes (each is also printed) */
extern unsigned u2ps_hfake_violations;
/* irq_mask(true) calls */
extern unsigned u2ps_hfake_masks;

/* The test's time in ms; u2ps_hfake_step() advances it */
extern uint32_t u2ps_hfake_now;

/* What u2ps_hport_init() returns */
extern u2ps_host_err_t u2ps_hfake_init_err;
/* What u2ps_hport_dma_check() returns (true until a test sets it), and
 * the memory the calls before the last u2ps_hport_init() named (the calls
 * of an init the stack refused change it not). The fake
 * reports the SETUP packet or a data stage of a control transfer outside
 * that memory as a port violation, and an init that checked none. Not
 * cleared by u2ps_hfake_reset_all(): the memory lasts as the port's start
 * does. */
#define U2PS_HFAKE_DMA_MAX 4u
extern bool u2ps_hfake_dma_ok;
extern struct u2ps_hfake_dma {
  const void *mem;
  uint32_t len;
} u2ps_hfake_dma[U2PS_HFAKE_DMA_MAX];
extern unsigned u2ps_hfake_dma_count;
/* What u2ps_hport_dma_ranges() reports: u2ps_hfake_range_count ranges of
 * u2ps_hfake_ranges, by default one, all of memory ([0, UINTPTR_MAX)). Not
 * cleared by u2ps_hfake_reset_all(), as the port's memory; a test that
 * narrows them restores them. u2ps_hfake_range_calls counts the calls
 * since u2ps_hfake_reset_all(); one outside a successful init's thread
 * context is a port violation. */
extern u2ps_hport_range_t u2ps_hfake_ranges[U2PS_HPORT_RANGES_MAX];
extern unsigned u2ps_hfake_range_count;
extern unsigned u2ps_hfake_range_calls;
/* The speed the next reset ends at (default HIGH; FULL or LOW). The fake
 * checks each control transfer's mps0 against it (64 at high speed; 8,
 * 16, 32 or 64 at full speed; 8 at low speed) and refuses a stream at
 * full or low speed. */
extern u2ps_speed_t u2ps_hfake_speed;
/* How resume(false) ends a resume. 0, the DWC2 model (default): at once,
 * true at the first call. n > 0, the ChipIdea model: the port times the K
 * itself, and resume(false) returns true only once the K has lasted n ms
 * since resume(true) or the remote wakeup. */
extern uint32_t u2ps_hfake_resume_ms;
/* 0: a reset segment lasts until reset(false), as on DWC2 (default). n > 0:
 * each segment ends by itself after n ms, as on ChipIdea, and reset(false)
 * does nothing. */
extern uint32_t u2ps_hfake_segment_ms;
/* The selector of the last test_mode call; -1 if none */
extern int u2ps_hfake_test_selector;

/* What the device does with a control transfer */
typedef enum {
  U2PS_HFAKE_ANSWER_OK,    /* all stages complete; device-to-host data in data, *len bytes */
  U2PS_HFAKE_ANSWER_STALL, /* a request error */
  U2PS_HFAKE_ANSWER_ERROR, /* no answer the port accepts, beyond its retries */
  U2PS_HFAKE_ANSWER_NONE   /* no completion at all: the transfer hangs until an abort */
} u2ps_hfake_answer_t;

/* The device: called when the stack starts a control transfer, with its
 * address, the SETUP and the data stage (wLength bytes; host-to-device:
 * what the host sends). Its answer completes at the next step. */
extern u2ps_hfake_answer_t (*u2ps_hfake_device)(uint8_t addr, const u2ps_hfake_setup_t *setup,
                                                uint8_t *data, uint16_t *len);

/* Clear the call log only */
void u2ps_hfake_clear_log(void);
/* Clear everything: log, violations, time (to 0), the port's state; not
 * the device function, nor the port's start, which lasts as the host
 * stack's record of it does (a failed init ends it) */
void u2ps_hfake_reset_all(void);

/* Index of the first logged call of 'kind' at or after 'from', or -1 */
int u2ps_hfake_find(u2ps_hfake_kind_t kind, unsigned from);
/* The first logged call of 'kind' at or after 'from'; if there is none, a
 * record of kind U2PS_HFAKE_MISSING */
u2ps_hfake_call_t u2ps_hfake_get(u2ps_hfake_kind_t kind, unsigned from);
/* Number of logged calls of 'kind' */
unsigned u2ps_hfake_count(u2ps_hfake_kind_t kind);
/* The nth control transfer (0 first) in the log, or NULL */
const u2ps_hfake_call_t *u2ps_hfake_control_nth(unsigned nth);

/* The vectors are masked now (thread) */
bool u2ps_hfake_masked(void);
/* Called once, at the next irq_mask(true), right after the mask, and then
 * cleared: code that preempts the stack's masked region, as an interrupt
 * of another source or a second thread would (NULL: none) */
extern void (*u2ps_hfake_on_mask)(void);
/* An upcall is running now */
bool u2ps_hfake_in_upcall(void);

/* Bus events, each as the port's interrupt */
void u2ps_hfake_connect(void);
/* The device leaves: the port drops any control transfer and reset without
 * a completion, then on_disconnect */
void u2ps_hfake_disconnect(void);
/* The port disables itself (babble): as a disconnect, but on_disabled */
void u2ps_hfake_disabled(void);
/* The device signals a remote wakeup, a K: while the port is suspended it
 * starts driving the resume K itself, as the controller does, then
 * on_wakeup; otherwise only on_wakeup */
void u2ps_hfake_wakeup(void);
/* The port is suspended (suspend called, no resume ended since), and it
 * drives a resume K (resume(true) or a remote wakeup, not yet ended) */
bool u2ps_hfake_suspended(void);
bool u2ps_hfake_resuming(void);

/* One millisecond: completions due, then u2ps_host_tick(u2ps_hfake_now) */
void u2ps_hfake_step(void);
/* Steps until u2ps_hfake_now reaches 'ms' */
void u2ps_hfake_run_until(uint32_t ms);
/* Steps until 'done' returns true, at most 'max_ms' steps; true if it
 * did */
bool u2ps_hfake_wait(bool (*done)(void), uint32_t max_ms);

/* A control transfer is in flight (started, not yet completed) */
bool u2ps_hfake_control_pending(void);

/* ---- The stream: a bus that steps microframes ---------------------------------
 * The port sends one queued packet per microframe, in queue order; a packet
 * queued in an upcall of microframe M, or from thread before the step of
 * M+1, goes out in M+1. Every report comes in bus order within the
 * microframe's step: the holes before a packet, then its on_sent, then the
 * feedback poll's report. The fake checks u2ps_hstream_t, the queue's
 * limit (U2PS_HOST_QUEUE_MAX; an excess counts in errors), each packet's
 * alignment and length, and the context of each call. Its counters are
 * u2ps_hport_stats(). The log records stream_open and stream_close, not
 * each queue call. */

typedef enum {
  U2PS_HFAKE_ST_IDLE,   /* no stream */
  U2PS_HFAKE_ST_OPEN,   /* stream_open succeeded */
  U2PS_HFAKE_ST_CLOSING /* stream_close called; on_closed comes at the next microframe */
} u2ps_hfake_stream_state_t;

extern u2ps_hfake_stream_state_t u2ps_hfake_stream_state;
extern u2ps_hstream_t u2ps_hfake_stream; /* as the last stream_open passed it */
extern bool u2ps_hfake_open_result;      /* what a valid stream_open returns (default true) */
/* The longest feedback bInterval the fake opens, as the DWC2 port
 * (u2ps_dwc2_hcd.h): 14 to 16, which USB 2.0 and u2ps_hport.h allow, make
 * stream_open return false (no port violation), and the host stack ends
 * the start with U2PS_HOST_STOP_OPEN */
#define U2PS_HFAKE_FB_INTERVAL_MAX 13u
extern u2ps_uframe_t u2ps_hfake_uframe_now; /* the microframe the next step runs */
extern unsigned u2ps_hfake_queued;       /* packets in the port's queue */
extern unsigned u2ps_hfake_queue_calls;  /* queue calls since u2ps_hfake_reset_all() */

/* The device's side of the bus, set by the test (NULL: nothing there).
 * - bus_out: a packet goes out intact in microframe uframe.
 * - bus_corrupt: a packet goes out damaged (or not at all) in uframe; the
 *   device sees no valid packet there.
 * - bus_fb: a feedback poll in uframe: fills at most max bytes of data and
 *   returns their number, or -1 for no valid packet (NO_PACKET). */
extern void (*u2ps_hfake_bus_out)(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe);
extern void (*u2ps_hfake_bus_corrupt)(uint16_t len, u2ps_uframe_t uframe);
extern int (*u2ps_hfake_bus_fb)(uint8_t *data, uint16_t max, u2ps_uframe_t uframe);
/* Called after every 1 ms step with the test's time (NULL: nothing) */
extern void (*u2ps_hfake_ms_hook)(uint32_t now_ms);
/* The tick of every 1 ms step, in the app's place (NULL, the default:
 * u2ps_host_tick) */
extern void (*u2ps_hfake_tick)(uint32_t now_ms);

/* The next packet's arm is late: it goes out n microframes later, which
 * are LATE */
void u2ps_hfake_inject_late(uint16_t n);
/* The next packet does not cross the bus intact: on_missed(1, CORRUPTED),
 * then its on_sent (an AHB error on DWC2) */
void u2ps_hfake_inject_corrupt(void);
/* The next feedback poll is skipped: on_fb_missed(LATE), and the next poll
 * stays one period later */
void u2ps_hfake_inject_fb_late(void);

/* One microframe of the bus, as the port's interrupt */
void u2ps_hfake_uframe(void);
/* n microframes, and a 1 ms step (u2ps_hfake_step) after every eighth since the
 * last u2ps_hfake_reset_all() */
void u2ps_hfake_uframes(unsigned n);

#endif /* U2PS_FAKE_HPORT_H */
