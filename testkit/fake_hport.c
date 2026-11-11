/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * fake_hport.c: see fake_hport.h.
 */
#include "fake_hport.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "u2ps_host.h"
#include "u2ps_uframe.h"

u2ps_hfake_call_t u2ps_hfake_calls[U2PS_HFAKE_CALLS_MAX];
unsigned u2ps_hfake_ncalls;
unsigned u2ps_hfake_violations;
unsigned u2ps_hfake_masks;
uint32_t u2ps_hfake_now;
u2ps_host_err_t u2ps_hfake_init_err;
u2ps_speed_t u2ps_hfake_speed = U2PS_SPEED_HIGH;
uint32_t u2ps_hfake_segment_ms;
uint32_t u2ps_hfake_resume_ms;
int u2ps_hfake_test_selector = -1;
u2ps_hfake_answer_t (*u2ps_hfake_device)(uint8_t addr, const u2ps_hfake_setup_t *setup,
                                         uint8_t *data, uint16_t *len);

static bool started;   /* init succeeded once */
static bool masked;    /* irq_mask(true) without irq_mask(false) */
static bool upcall;    /* inside an upcall */
static bool connected; /* a device is on the root port */
static bool enabled;   /* a reset ended and no disable, disconnect or new reset since */
static bool resetting; /* a reset segment runs */
static uint32_t segment_end; /* ChipIdea model: when the running segment ends */
static bool enable_due;      /* on_enabled at the next step */
static bool test_mode;
static u2ps_speed_t port_speed; /* the speed the port enabled at */
static bool suspended;       /* suspend called; no resume ended since */
static bool resuming;        /* the resume K runs */
static uint32_t k_start;     /* when it began */

/* The control transfer in flight */
static struct {
  bool pending;
  bool due;      /* its completion comes at the next step */
  bool aborted;  /* control_abort was called */
  unsigned log;  /* its log entry, or U2PS_HFAKE_CALLS_MAX */
  u2ps_hport_control_t result;
  uint16_t len;
  /* A split transfer: 1 the SETUP stage in flight, 2 its SETUP done and
   * the data stage not yet asked for, 3 the data stage in flight */
  unsigned split;
  u2ps_hfake_answer_t answer;  /* the device's answer, kept for the data stage */
  uint16_t answer_len;
  uint16_t wlength;
  bool to_host;
} ctl;
static uint8_t split_buf[1024]; /* the device's data stage of a split transfer */

static u2ps_hport_stats_t stats;

/* The stream */
u2ps_hfake_stream_state_t u2ps_hfake_stream_state;
u2ps_hstream_t u2ps_hfake_stream;
bool u2ps_hfake_open_result = true;
u2ps_uframe_t u2ps_hfake_uframe_now;
unsigned u2ps_hfake_queued;
unsigned u2ps_hfake_queue_calls;
void (*u2ps_hfake_bus_out)(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe);
void (*u2ps_hfake_bus_corrupt)(uint16_t len, u2ps_uframe_t uframe);
int (*u2ps_hfake_bus_fb)(uint8_t *data, uint16_t max, u2ps_uframe_t uframe);
void (*u2ps_hfake_ms_hook)(uint32_t now_ms);
void (*u2ps_hfake_tick)(uint32_t now_ms);
void (*u2ps_hfake_on_mask)(void);

bool u2ps_hfake_dma_ok = true;
struct u2ps_hfake_dma u2ps_hfake_dma[U2PS_HFAKE_DMA_MAX];
unsigned u2ps_hfake_dma_count;
u2ps_hport_range_t u2ps_hfake_ranges[U2PS_HPORT_RANGES_MAX] = {{0u, UINTPTR_MAX}};
unsigned u2ps_hfake_range_count = 1;
unsigned u2ps_hfake_range_calls;
/* The checks since the last init: they become the list at the next init */
static struct u2ps_hfake_dma dma_next[U2PS_HFAKE_DMA_MAX];
static unsigned dma_next_count;

/* The port's queue, one more than the stack may fill, to see an excess */
#define QUEUE_SIZE (U2PS_HOST_QUEUE_MAX + 1u)
static struct {
  const uint8_t *data;
  uint16_t len;
} queue[QUEUE_SIZE];
static unsigned q_head;
static bool stream_started; /* a packet has gone out since the open */
static uint16_t no_buffer; /* empty microframes since the last packet */
static uint16_t late;      /* LATE microframes before the next packet */
static uint16_t late_due;  /* injected: microframes the next packet's arm is late */
static bool corrupt_due;
static bool fb_late_due;
static u2ps_uframe_t poll_next; /* the next feedback poll's microframe */
static unsigned sub;            /* microframes since the last 1 ms step */
static uint8_t fb_buf[1024];

/* The stream ends without a report; its packets are the stack's again */
static void stream_stop(void)
{
  u2ps_hfake_stream_state = U2PS_HFAKE_ST_IDLE;
  u2ps_hfake_queued = 0;
}

static void violation(const char *what)
{
  u2ps_hfake_violations++;
  printf("fake host port: port violation: %s (at %u ms)\n", what, (unsigned)u2ps_hfake_now);
}

static u2ps_hfake_from_t check_call(u2ps_hfake_kind_t kind)
{
  u2ps_hfake_from_t from = upcall   ? U2PS_HFAKE_FROM_UPCALL
                           : masked ? U2PS_HFAKE_FROM_MASKED
                                    : U2PS_HFAKE_FROM_THREAD;
  /* Calls from thread need the mask, except the first init */
  if (from == U2PS_HFAKE_FROM_THREAD && !(kind == U2PS_HFAKE_INIT && !started))
    violation("a port call from thread without the mask");
  if (!started && kind != U2PS_HFAKE_INIT)
    violation("a port call before a successful init");
  return from;
}

static unsigned log_call(u2ps_hfake_kind_t kind, uint32_t arg)
{
  u2ps_hfake_from_t from = check_call(kind);
  if (u2ps_hfake_ncalls >= U2PS_HFAKE_CALLS_MAX)
    return U2PS_HFAKE_CALLS_MAX;
  u2ps_hfake_calls[u2ps_hfake_ncalls] = (u2ps_hfake_call_t){
    .kind = kind, .from = from, .ms = u2ps_hfake_now, .arg = arg, .done_ms = 0xFFFFFFFFu};
  return u2ps_hfake_ncalls++;
}

void u2ps_hfake_clear_log(void)
{
  u2ps_hfake_ncalls = 0;
}

void u2ps_hfake_reset_all(void)
{
  u2ps_hfake_clear_log();
  u2ps_hfake_violations = 0;
  u2ps_hfake_masks = 0;
  u2ps_hfake_now = 0;
  u2ps_hfake_init_err = U2PS_HOST_OK;
  u2ps_hfake_dma_ok = true;
  u2ps_hfake_speed = U2PS_SPEED_HIGH;
  u2ps_hfake_segment_ms = 0;
  u2ps_hfake_resume_ms = 0;
  u2ps_hfake_test_selector = -1;
  u2ps_hfake_range_calls = 0;
  /* The port's start lasts, as the host stack's record of it does: a
   * re-init after it comes with the mask */
  masked = upcall = connected = enabled = resetting = enable_due = test_mode = false;
  suspended = resuming = false;
  memset(&ctl, 0, sizeof ctl);
  memset(&stats, 0, sizeof stats);
  u2ps_hfake_stream_state = U2PS_HFAKE_ST_IDLE;
  memset(&u2ps_hfake_stream, 0, sizeof u2ps_hfake_stream);
  u2ps_hfake_open_result = true;
  u2ps_hfake_uframe_now = 0;
  u2ps_hfake_queued = u2ps_hfake_queue_calls = 0;
  u2ps_hfake_bus_out = NULL;
  u2ps_hfake_bus_corrupt = NULL;
  u2ps_hfake_bus_fb = NULL;
  u2ps_hfake_ms_hook = NULL;
  u2ps_hfake_tick = NULL;
  u2ps_hfake_on_mask = NULL;
  late_due = 0;
  corrupt_due = fb_late_due = false;
  sub = 0;
}

int u2ps_hfake_find(u2ps_hfake_kind_t kind, unsigned from)
{
  for (unsigned i = from; i < u2ps_hfake_ncalls; i++)
    if (u2ps_hfake_calls[i].kind == kind)
      return (int)i;
  return -1;
}

u2ps_hfake_call_t u2ps_hfake_get(u2ps_hfake_kind_t kind, unsigned from)
{
  int i = u2ps_hfake_find(kind, from);
  if (i < 0)
    return (u2ps_hfake_call_t){.kind = U2PS_HFAKE_MISSING, .ms = 0xFFFFFFFFu, .arg = 0xFFFFFFFFu,
                               .done_ms = 0xFFFFFFFFu};
  return u2ps_hfake_calls[i];
}

unsigned u2ps_hfake_count(u2ps_hfake_kind_t kind)
{
  unsigned n = 0;
  for (unsigned i = 0; i < u2ps_hfake_ncalls; i++)
    n += u2ps_hfake_calls[i].kind == kind;
  return n;
}

const u2ps_hfake_call_t *u2ps_hfake_control_nth(unsigned nth)
{
  for (unsigned i = 0; i < u2ps_hfake_ncalls; i++)
    if (u2ps_hfake_calls[i].kind == U2PS_HFAKE_CONTROL && nth-- == 0u)
      return &u2ps_hfake_calls[i];
  return NULL;
}

bool u2ps_hfake_masked(void)
{
  return masked;
}

bool u2ps_hfake_in_upcall(void)
{
  return upcall;
}

bool u2ps_hfake_control_pending(void)
{
  return ctl.pending;
}

bool u2ps_hfake_suspended(void)
{
  return suspended;
}

bool u2ps_hfake_resuming(void)
{
  return resuming;
}

/* A call that needs the port running: not suspended, not resuming */
static void check_awake(const char *what)
{
  if (suspended || resuming) {
    char m[96];
    snprintf(m, sizeof m, "%s while the port is suspended or resuming", what);
    violation(m);
  }
}

/* mps0 against the speed the port enabled at (USB 2.0 5.5.3) */
static void check_mps0(uint8_t mps0)
{
  bool ok = port_speed == U2PS_SPEED_HIGH  ? mps0 == 64u
            : port_speed == U2PS_SPEED_LOW ? mps0 == 8u
                                           : mps0 == 8u || mps0 == 16u || mps0 == 32u || mps0 == 64u;
  if (!ok)
    violation("a control transfer with an mps0 the speed does not allow");
}

/* An upcall runs as the port's interrupt: never while the vectors are
 * masked, never inside another */
static bool upcall_begin(void)
{
  if (masked || upcall) {
    violation("an interrupt while masked or inside an upcall (the test's mistake)");
    return false;
  }
  upcall = true;
  return true;
}

static void upcall_end(void)
{
  upcall = false;
}

/* len bytes at p lie in the DMA memory the stack checked at its last init */
static void check_dma(const char *what, const void *p, uint32_t len)
{
  if (len == 0u)
    return;
  for (unsigned i = 0; i < u2ps_hfake_dma_count; i++) {
    uintptr_t a = (uintptr_t)p, m = (uintptr_t)u2ps_hfake_dma[i].mem;
    if (p != NULL && a >= m && a - m <= u2ps_hfake_dma[i].len &&
        len <= u2ps_hfake_dma[i].len - (a - m))
      return;
  }
  char msg[96];
  snprintf(msg, sizeof msg, "%s outside the DMA memory the stack checked at init", what);
  violation(msg);
}

/* ---- Port calls --------------------------------------------------------------- */

bool u2ps_hport_dma_check(const void *mem, uint32_t len)
{
  /* Before any other port call of an init, so not through check_call */
  if (u2ps_hfake_ncalls < U2PS_HFAKE_CALLS_MAX)
    u2ps_hfake_calls[u2ps_hfake_ncalls++] = (u2ps_hfake_call_t){
      .kind = U2PS_HFAKE_DMA_CHECK,
      .from = upcall ? U2PS_HFAKE_FROM_UPCALL : masked ? U2PS_HFAKE_FROM_MASKED : U2PS_HFAKE_FROM_THREAD,
      .ms = u2ps_hfake_now, .arg = len, .ret = u2ps_hfake_dma_ok, .done_ms = 0xFFFFFFFFu};
  if (upcall)
    violation("dma_check inside an upcall");
  if (dma_next_count < U2PS_HFAKE_DMA_MAX)
    dma_next[dma_next_count++] = (struct u2ps_hfake_dma){mem, len};
  else
    violation("more dma_check calls in one init than the fake keeps (the test's limit)");
  return u2ps_hfake_dma_ok;
}

/* Not logged, so that the call log of an init is the same with and without
 * the contract checks */
uint32_t u2ps_hport_dma_ranges(u2ps_hport_range_t *ranges)
{
  u2ps_hfake_range_calls++;
  if (upcall || !started)
    violation("dma_ranges inside an upcall, or without a successful init");
  unsigned n = u2ps_hfake_range_count < U2PS_HPORT_RANGES_MAX ? u2ps_hfake_range_count
                                                               : U2PS_HPORT_RANGES_MAX;
  memcpy(ranges, u2ps_hfake_ranges, n * sizeof ranges[0]);
  return n;
}

u2ps_host_err_t u2ps_hport_init(void)
{
  log_call(U2PS_HFAKE_INIT, 0);
  if (dma_next_count == 0u)
    violation("an init without a dma_check before it");
  memcpy(u2ps_hfake_dma, dma_next, sizeof dma_next);
  u2ps_hfake_dma_count = dma_next_count;
  dma_next_count = 0;
  if (started && !masked)
    violation("a re-init without the mask");
  if (u2ps_hfake_init_err == U2PS_HOST_OK) {
    started = true;
    /* The controller resets: no session, no test mode; a device still on
     * the root port is reported again by the test (u2ps_hfake_connect) */
    enabled = resetting = enable_due = test_mode = suspended = resuming = false;
    memset(&ctl, 0, sizeof ctl);
    u2ps_hfake_test_selector = -1;
    stream_stop();
  } else {
    started = false;
  }
  return u2ps_hfake_init_err;
}

void u2ps_hport_irq_mask(bool on)
{
  /* Counted, not logged: the stack masks at every tick that has work */
  u2ps_hfake_masks += on;
  if (upcall)
    violation("irq_mask inside an upcall");
  if (on == masked)
    violation(on ? "irq_mask(true) nested" : "irq_mask(false) without irq_mask(true)");
  masked = on;
  if (on && u2ps_hfake_on_mask != NULL) {
    void (*f)(void) = u2ps_hfake_on_mask;
    u2ps_hfake_on_mask = NULL;
    f();
  }
}

void u2ps_hport_reset(bool on)
{
  log_call(U2PS_HFAKE_RESET, on);
  if (test_mode)
    violation("reset in test mode");
  if (on) {
    if (!connected)
      violation("reset(true) without a device");
    if (ctl.pending)
      violation("reset(true) with a control transfer in flight");
    check_awake("reset(true)");
    resetting = true;
    enabled = false;
    enable_due = false;
    segment_end = u2ps_hfake_now + u2ps_hfake_segment_ms;
  } else if (u2ps_hfake_segment_ms == 0u) {
    if (!resetting)
      violation("reset(false) without a reset segment");
    resetting = false;
    enable_due = connected;
  }
}

void u2ps_hport_disable(void)
{
  log_call(U2PS_HFAKE_DISABLE, 0);
  enabled = resetting = enable_due = suspended = resuming = false;
  stream_stop();
  /* Whatever was in flight ends without a completion */
  if (ctl.pending && ctl.log < U2PS_HFAKE_CALLS_MAX)
    u2ps_hfake_calls[ctl.log].done_ms = 0xFFFFFFFFu;
  memset(&ctl, 0, sizeof ctl);
}

void u2ps_hport_test_mode(uint8_t selector)
{
  log_call(U2PS_HFAKE_TEST_MODE, selector);
  if (selector < 1u || selector > 5u)
    violation("test_mode with a selector outside 1..5");
  if (enabled || resetting || ctl.pending || suspended || resuming)
    violation("test_mode without a disable first");
  test_mode = true;
  u2ps_hfake_test_selector = selector;
}

static u2ps_hfake_setup_t setup_of(const uint8_t setup[8])
{
  return (u2ps_hfake_setup_t){.bmRequestType = setup[0],
                              .bRequest = setup[1],
                              .wValue = (uint16_t)(setup[2] | setup[3] << 8),
                              .wIndex = (uint16_t)(setup[4] | setup[5] << 8),
                              .wLength = (uint16_t)(setup[6] | setup[7] << 8)};
}

static u2ps_hfake_answer_t answer_of_device(uint8_t addr, const u2ps_hfake_setup_t *s,
                                            uint8_t *data, uint16_t *len)
{
  return u2ps_hfake_device != NULL ? u2ps_hfake_device(addr, s, data, len)
                                   : U2PS_HFAKE_ANSWER_ERROR;
}

static u2ps_hport_control_t result_of(u2ps_hfake_answer_t a)
{
  return a == U2PS_HFAKE_ANSWER_OK      ? U2PS_HPORT_CONTROL_OK
         : a == U2PS_HFAKE_ANSWER_STALL ? U2PS_HPORT_CONTROL_STALL
                                        : U2PS_HPORT_CONTROL_ERROR;
}

void u2ps_hport_control(uint8_t addr, uint8_t mps0, const uint8_t setup[8], void *data)
{
  unsigned i = log_call(U2PS_HFAKE_CONTROL, addr);
  u2ps_hfake_setup_t s = setup_of(setup);
  if (i < U2PS_HFAKE_CALLS_MAX) {
    u2ps_hfake_calls[i].setup = s;
    u2ps_hfake_calls[i].mps0 = mps0;
  }
  if (ctl.pending || ctl.split != 0u)
    violation("a control transfer while one is in flight");
  if (!enabled)
    violation("a control transfer on a port not enabled");
  check_awake("a control transfer");
  check_mps0(mps0);
  if (s.wLength > 0u && data == NULL)
    violation("a data stage without a buffer");
  check_dma("the SETUP packet", setup, 8u);
  check_dma("the data stage", data, s.wLength);
  memset(&ctl, 0, sizeof ctl);
  ctl.pending = true;
  ctl.log = i;
  uint16_t len = 0;
  u2ps_hfake_answer_t a = answer_of_device(addr, &s, data, &len);
  if (a == U2PS_HFAKE_ANSWER_NONE)
    return;
  ctl.due = true;
  ctl.result = result_of(a);
  ctl.len = a == U2PS_HFAKE_ANSWER_OK ? len : 0u;
  if (ctl.len > s.wLength)
    violation("the device model sent more than wLength (the test's mistake)");
}

/* The SETUP stage completes at the next step; the device's answer to the
 * whole request is taken now and kept for the data stage */
void u2ps_hport_control_setup(uint8_t addr, uint8_t mps0, const uint8_t setup[8])
{
  unsigned i = log_call(U2PS_HFAKE_CONTROL_SETUP, addr);
  u2ps_hfake_setup_t s = setup_of(setup);
  if (i < U2PS_HFAKE_CALLS_MAX) {
    u2ps_hfake_calls[i].setup = s;
    u2ps_hfake_calls[i].mps0 = mps0;
  }
  if (ctl.pending || ctl.split != 0u)
    violation("control_setup while a control transfer is in flight");
  if (!enabled)
    violation("control_setup on a port not enabled");
  check_awake("control_setup");
  check_mps0(mps0);
  check_dma("the SETUP packet", setup, 8u);
  memset(&ctl, 0, sizeof ctl);
  ctl.pending = true;
  ctl.log = i;
  ctl.split = 1;
  ctl.wlength = s.wLength;
  ctl.to_host = (s.bmRequestType & 0x80u) != 0u;
  if (s.wLength > sizeof split_buf || !ctl.to_host) {
    violation("control_setup of a request the fake does not split (the test's mistake)");
    ctl.answer = U2PS_HFAKE_ANSWER_ERROR;
  } else {
    uint16_t len = 0;
    ctl.answer = answer_of_device(addr, &s, split_buf, &len);
    ctl.answer_len = len;
    if (len > s.wLength)
      violation("the device model sent more than wLength (the test's mistake)");
  }
  /* The device acknowledges every SETUP it sees (USB 2.0 8.5.3) */
  if (ctl.answer == U2PS_HFAKE_ANSWER_NONE)
    return;
  ctl.due = true;
  ctl.result =
      ctl.answer == U2PS_HFAKE_ANSWER_ERROR ? U2PS_HPORT_CONTROL_ERROR : U2PS_HPORT_CONTROL_OK;
  ctl.len = 0;
}

void u2ps_hport_control_data(void *data)
{
  unsigned i = log_call(U2PS_HFAKE_CONTROL_DATA, 0);
  if (ctl.split != 2u) {
    violation("control_data without a completed control_setup");
    return;
  }
  check_awake("control_data");
  if (!enabled)
    violation("control_data on a port not enabled");
  if (data == NULL && ctl.wlength > 0u)
    violation("a data stage without a buffer");
  check_dma("the data stage", data, ctl.wlength);
  ctl.pending = true;
  ctl.log = i;
  ctl.split = 3;
  ctl.due = true;
  ctl.result = result_of(ctl.answer);
  ctl.len = ctl.answer == U2PS_HFAKE_ANSWER_OK ? ctl.answer_len : 0u;
  if (ctl.answer == U2PS_HFAKE_ANSWER_OK && data != NULL)
    memcpy(data, split_buf, ctl.len);
}

void u2ps_hport_suspend(void)
{
  log_call(U2PS_HFAKE_SUSPEND, 0);
  if (!enabled || ctl.pending || ctl.split != 0u || u2ps_hfake_stream_state != U2PS_HFAKE_ST_IDLE)
    violation("suspend on a port not enabled, or with a transfer or a stream");
  if (suspended || resuming)
    violation("suspend while suspended or resuming");
  suspended = true;
}

bool u2ps_hport_resume(bool on)
{
  unsigned i = log_call(U2PS_HFAKE_RESUME, on);
  bool done = false;
  if (on) {
    if (!suspended || resuming)
      violation("resume(true) on a port not suspended, or resuming");
    resuming = true;
    k_start = u2ps_hfake_now;
  } else {
    if (!resuming)
      violation("resume(false) without a resume");
    done = u2ps_hfake_resume_ms == 0u || u2ps_hfake_now - k_start >= u2ps_hfake_resume_ms;
    if (done)
      suspended = resuming = false;
  }
  if (i < U2PS_HFAKE_CALLS_MAX)
    u2ps_hfake_calls[i].ret = done;
  return done;
}

void u2ps_hport_control_abort(void)
{
  log_call(U2PS_HFAKE_CONTROL_ABORT, 0);
  if (!ctl.pending) {
    violation("control_abort without a control transfer");
    return;
  }
  if (ctl.aborted)
    violation("control_abort twice");
  ctl.aborted = true;
  /* A transfer not yet complete ends ABORTED; one already due keeps its
   * result */
  if (!ctl.due) {
    ctl.due = true;
    ctl.result = U2PS_HPORT_CONTROL_ABORTED;
    ctl.len = 0;
  }
}

bool u2ps_hport_stream_open(const u2ps_hstream_t *s)
{
  log_call(U2PS_HFAKE_STREAM_OPEN, s->ep_out);
  if (u2ps_hfake_stream_state != U2PS_HFAKE_ST_IDLE)
    violation("stream_open with a stream");
  if (!enabled || ctl.pending)
    violation("stream_open on a port not enabled, or with a control transfer in flight");
  check_awake("stream_open");
  if (port_speed != U2PS_SPEED_HIGH)
    violation("stream_open at full or low speed");
  bool fb_ok = s->ep_fb == 0u ? s->fb_interval == 0u && s->fb_mps == 0u
                              : s->ep_fb <= 15u && s->fb_interval >= 1u && s->fb_interval <= 16u &&
                                  s->fb_mps >= 4u && s->fb_mps <= sizeof fb_buf;
  if (s->addr != 1u || s->ep_out < 1u || s->ep_out > 15u || s->mps < 1u || s->mps > 1024u || !fb_ok)
    violation("stream_open with a stream outside u2ps_hport.h");
  /* The DWC2 port's limit (U2PS_HFAKE_FB_INTERVAL_MAX) */
  if (s->ep_fb != 0u && s->fb_interval > U2PS_HFAKE_FB_INTERVAL_MAX)
    return false;
  if (!u2ps_hfake_open_result)
    return false;
  u2ps_hfake_stream = *s;
  u2ps_hfake_stream_state = U2PS_HFAKE_ST_OPEN;
  u2ps_hfake_queued = 0;
  stream_started = false;
  no_buffer = late = 0;
  poll_next = u2ps_hfake_uframe_now;
  return true;
}

void u2ps_hport_queue(const uint8_t *data, uint16_t len)
{
  check_call(U2PS_HFAKE_QUEUE);
  u2ps_hfake_queue_calls++;
  check_awake("queue");
  if (u2ps_hfake_stream_state != U2PS_HFAKE_ST_OPEN) {
    violation("queue without an open stream");
    return;
  }
  if ((uintptr_t)data % U2PS_HPORT_ALIGN != 0u || len > u2ps_hfake_stream.mps)
    violation("queue of a packet misaligned or longer than mps");
  if (u2ps_hfake_queued >= U2PS_HOST_QUEUE_MAX) {
    violation("queue beyond U2PS_HOST_QUEUE_MAX");
    stats.errors++;
    if (u2ps_hfake_queued == QUEUE_SIZE)
      return;
  }
  unsigned i = (q_head + u2ps_hfake_queued++) % QUEUE_SIZE;
  queue[i].data = data;
  queue[i].len = len;
}

void u2ps_hport_stream_close(void)
{
  log_call(U2PS_HFAKE_STREAM_CLOSE, 0);
  if (u2ps_hfake_stream_state != U2PS_HFAKE_ST_OPEN)
    violation("stream_close without an open stream");
  u2ps_hfake_stream_state = U2PS_HFAKE_ST_CLOSING;
}

const u2ps_hport_stats_t *u2ps_hport_stats(void)
{
  log_call(U2PS_HFAKE_STATS, 0);
  return &stats;
}

/* ---- Events ------------------------------------------------------------------- */

void u2ps_hfake_connect(void)
{
  connected = true;
  if (!upcall_begin())
    return;
  u2ps_host_on_connect();
  upcall_end();
}

static void drop_all(void)
{
  enabled = resetting = enable_due = suspended = resuming = false;
  stream_stop();
  memset(&ctl, 0, sizeof ctl);
}

void u2ps_hfake_disconnect(void)
{
  connected = false;
  drop_all();
  if (!upcall_begin())
    return;
  u2ps_host_on_disconnect();
  upcall_end();
}

void u2ps_hfake_disabled(void)
{
  drop_all();
  if (!upcall_begin())
    return;
  u2ps_host_on_disabled();
  upcall_end();
}

void u2ps_hfake_wakeup(void)
{
  if (suspended && !resuming) {
    resuming = true;
    k_start = u2ps_hfake_now;
  }
  if (!upcall_begin())
    return;
  u2ps_host_on_wakeup();
  upcall_end();
}

void u2ps_hfake_step(void)
{
  u2ps_hfake_now++;
  /* A reset segment that ends by itself (ChipIdea model) */
  if (resetting && u2ps_hfake_segment_ms > 0u && u2ps_hfake_now >= segment_end) {
    resetting = false;
    enable_due = true;
  }
  if (enable_due) {
    enable_due = false;
    enabled = true;
    port_speed = u2ps_hfake_speed;
    if (upcall_begin()) {
      u2ps_host_on_enabled(u2ps_hfake_speed);
      upcall_end();
    }
  }
  if (ctl.pending && ctl.due) {
    u2ps_hport_control_t r = ctl.result;
    uint16_t len = ctl.len;
    if (ctl.log < U2PS_HFAKE_CALLS_MAX) {
      u2ps_hfake_calls[ctl.log].done_ms = u2ps_hfake_now;
      u2ps_hfake_calls[ctl.log].result = r;
      u2ps_hfake_calls[ctl.log].len = len;
    }
    /* A SETUP stage that completed OK waits for its data stage; anything
     * else ends the transfer */
    if (ctl.split == 1u && r == U2PS_HPORT_CONTROL_OK && !ctl.aborted) {
      ctl.split = 2;
      ctl.pending = ctl.due = false;
    } else {
      memset(&ctl, 0, sizeof ctl);
    }
    if (upcall_begin()) {
      u2ps_host_on_control(r, len);
      upcall_end();
    }
  }
  if (u2ps_hfake_tick != NULL)
    u2ps_hfake_tick(u2ps_hfake_now);
  else
    u2ps_host_tick(u2ps_hfake_now);
  if (u2ps_hfake_ms_hook != NULL)
    u2ps_hfake_ms_hook(u2ps_hfake_now);
}

void u2ps_hfake_run_until(uint32_t ms)
{
  while (u2ps_hfake_now < ms)
    u2ps_hfake_step();
}

bool u2ps_hfake_wait(bool (*done)(void), uint32_t max_ms)
{
  for (uint32_t i = 0; i < max_ms; i++) {
    if (done())
      return true;
    u2ps_hfake_step();
  }
  return done();
}

/* ---- The stream's microframes ---------------------------------------------------- */

void u2ps_hfake_inject_late(uint16_t n)
{
  late_due = n;
}

void u2ps_hfake_inject_corrupt(void)
{
  corrupt_due = true;
}

void u2ps_hfake_inject_fb_late(void)
{
  fb_late_due = true;
}

/* The holes before the packet that goes out now, in bus order */
static void report_holes(void)
{
  if (no_buffer > 0u) {
    stats.no_buffer += no_buffer;
    u2ps_host_on_missed(no_buffer, U2PS_MISS_NO_BUFFER);
    no_buffer = 0;
  }
  if (late > 0u) {
    stats.late += late;
    u2ps_host_on_missed(late, U2PS_MISS_LATE);
    late = 0;
  }
}

static void data_uframe(u2ps_uframe_t m)
{
  if (u2ps_hfake_queued == 0u) {
    /* NO_BUFFER, once the first packet has gone; reported at the next
     * packet only, so microframes after the last packet are no hole */
    if (stream_started && no_buffer < 0xFFFFu)
      no_buffer++;
    return;
  }
  if (late_due > 0u) {
    late_due--;
    late++;
    return;
  }
  const uint8_t *data = queue[q_head].data;
  uint16_t len = queue[q_head].len;
  q_head = (q_head + 1u) % QUEUE_SIZE;
  u2ps_hfake_queued--;
  stream_started = true;
  if (corrupt_due) {
    corrupt_due = false;
    if (u2ps_hfake_bus_corrupt != NULL)
      u2ps_hfake_bus_corrupt(len, m);
    report_holes();
    stats.corrupted++;
    stats.errors++;
    u2ps_host_on_missed(1, U2PS_MISS_CORRUPTED);
  } else {
    if (u2ps_hfake_bus_out != NULL)
      u2ps_hfake_bus_out(data, len, m);
    report_holes();
  }
  stats.sent++;
  u2ps_host_on_sent(m);
}

static void fb_uframe(u2ps_uframe_t m)
{
  if (u2ps_hfake_stream.ep_fb == 0u || m != poll_next)
    return;
  poll_next = u2ps_uframe_add(m, (uint16_t)(1u << (u2ps_hfake_stream.fb_interval - 1u)));
  if (fb_late_due) {
    fb_late_due = false;
    stats.fb_missed++;
    u2ps_host_on_fb_missed(m, U2PS_MISS_LATE);
    return;
  }
  stats.fb_polls++;
  int n = u2ps_hfake_bus_fb != NULL ? u2ps_hfake_bus_fb(fb_buf, u2ps_hfake_stream.fb_mps, m) : -1;
  if (n < 0) {
    stats.fb_missed++;
    u2ps_host_on_fb_missed(m, U2PS_MISS_NO_PACKET);
  } else {
    u2ps_host_on_fb(fb_buf, (uint16_t)n, m);
  }
}

void u2ps_hfake_uframe(void)
{
  u2ps_uframe_t m = u2ps_hfake_uframe_now;
  u2ps_hfake_uframe_now = u2ps_uframe_add(m, 1);
  if (u2ps_hfake_stream_state == U2PS_HFAKE_ST_IDLE)
    return;
  if (!upcall_begin())
    return;
  if (u2ps_hfake_stream_state == U2PS_HFAKE_ST_CLOSING) {
    /* The close ends: what was queued returns here */
    stream_stop();
    u2ps_host_on_closed();
  } else {
    data_uframe(m);
    if (u2ps_hfake_stream_state == U2PS_HFAKE_ST_OPEN)
      fb_uframe(m);
  }
  upcall_end();
}

void u2ps_hfake_uframes(unsigned n)
{
  for (unsigned i = 0; i < n; i++) {
    u2ps_hfake_uframe();
    if (++sub == 8u) {
      sub = 0;
      u2ps_hfake_step();
    }
  }
}
