/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * fake_dport.c: the fake device port and the host's side of control
 * transfers. See fake_dport.h.
 */
#include "fake_dport.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "u2ps_dev.h"

_Static_assert(U2PS_DPORT_ISO_OUT_MIN == U2PS_DPORT_ISO_OUT_RESERVE + 1u,
               "the least count lets the stack hold one packet");

u2ps_fake_call_t u2ps_fake_calls[U2PS_FAKE_CALLS_MAX];
unsigned u2ps_fake_ncalls;
unsigned u2ps_fake_violations;

u2ps_fake_ep0_t u2ps_fake_ep0;
uint8_t u2ps_fake_address;
int u2ps_fake_test_selector = -1;
u2ps_dev_err_t u2ps_fake_init_err = U2PS_DEV_OK;
bool u2ps_fake_dma_ok = true;
const void *u2ps_fake_dma_mem;
uint32_t u2ps_fake_dma_len;
bool u2ps_fake_attached;
bool u2ps_fake_vbus = true;
/* u2ps_dport_init() has succeeded. The device stack keeps its own
 * record for the whole process, so this one is never cleared either. */
static bool port_up;
bool u2ps_fake_deliver_stale;
unsigned u2ps_fake_stale_dropped;
unsigned u2ps_fake_masked_bytes_max;
void (*u2ps_fake_on_mask)(void);

u2ps_fake_stream_state_t u2ps_fake_stream_state;
u2ps_stream_t u2ps_fake_stream;
bool u2ps_fake_open_result = true;
uint32_t u2ps_fake_fb;
unsigned u2ps_fake_held;
const uint8_t *u2ps_fake_last_data;
u2ps_dport_stats_t u2ps_fake_stats;

/* The next buffer of the ring, and the byte counter of packet contents */
static unsigned ring_next;
static uint8_t seq;

/* The reserve (U2PS_DPORT_ISO_OUT_RESERVE): packets that came while the
 * app held count - RESERVE, NO_BUFFER, and the microframes without a
 * packet between them, NO_PACKET, since the last delivered packet; and the
 * microframe of the last packet received */
static uint32_t tail_no_buffer, tail_no_packet;
static u2ps_uframe_t last_rx;

static void stream_tail(void);

static void stream_forget(void)
{
  u2ps_fake_held = 0;
  tail_no_buffer = tail_no_packet = 0;
  last_rx = U2PS_UFRAME_UNKNOWN;
}

/* SETUPs delivered so far, and the one a completion not yet reported
 * belongs to */
static unsigned setup_gen;
static bool unreported;
static unsigned unreported_gen;

/* The armed EP0 stage */
static const uint8_t *ep0_in_data;
static uint8_t *ep0_out_buf;
static uint16_t ep0_len;
static bool ep0_zlp;

/* Context of the port calls */
static u2ps_fake_from_t upcall = U2PS_FAKE_FROM_THREAD;
static bool masked;
static bool status_in_this_setup; /* ep0_status already called in this on_setup */

/* The device stack's control buffer (u2ps_dev_priv.h), and its contents
 * when the vectors were last masked */
extern uint8_t u2ps_dev_ep0_buf[U2PS_EP0_BUF];
static uint8_t ep0_buf_at_mask[U2PS_EP0_BUF];
static int pending_address = -1;

/* A SETUP that arrives at an irq_mask(true) still to come (after
 * setup_mask_skip more), and one the port holds */
static bool setup_on_mask;
static unsigned setup_mask_skip;
static bool setup_held;
static uint8_t held_setup[8];

/* The transfer u2ps_fake_host_control() is running */
static struct {
  uint8_t bmRequestType;
  uint16_t wLength;
  bool data_done;
} cur;

static void violation(const char *what)
{
  u2ps_fake_violations++;
  printf("fake_dport: port violation: %s\n", what);
}

static u2ps_fake_from_t from(void)
{
  if (upcall != U2PS_FAKE_FROM_THREAD)
    return upcall;
  return masked ? U2PS_FAKE_FROM_MASKED : U2PS_FAKE_FROM_THREAD;
}

static u2ps_fake_call_t *log_call(u2ps_fake_kind_t kind, uint32_t arg)
{
  static u2ps_fake_call_t overflow;
  if (u2ps_fake_ncalls >= U2PS_FAKE_CALLS_MAX) {
    violation("call log full");
    return &overflow;
  }
  u2ps_fake_call_t *c = &u2ps_fake_calls[u2ps_fake_ncalls++];
  *c = (u2ps_fake_call_t){.kind = kind, .from = from(), .arg = arg};
  return c;
}

/* EP0 calls come from an upcall or from thread with the vectors masked */
static void check_ep0_context(const char *call)
{
  if (from() == U2PS_FAKE_FROM_THREAD) {
    char msg[96];
    snprintf(msg, sizeof msg, "%s from thread without irq_mask", call);
    violation(msg);
  }
}

/* One stage at a time: a stage is armed only while EP0 NAKs */
static void check_ep0_idle(const char *call)
{
  if (u2ps_fake_ep0 != U2PS_FAKE_EP0_NAK) {
    char msg[96];
    snprintf(msg, sizeof msg, "%s while EP0 state %d is armed", call, (int)u2ps_fake_ep0);
    violation(msg);
  }
}

void u2ps_fake_clear_log(void)
{
  u2ps_fake_ncalls = 0;
}

void u2ps_fake_reset_all(void)
{
  u2ps_fake_ncalls = 0;
  u2ps_fake_violations = 0;
  u2ps_fake_ep0 = U2PS_FAKE_EP0_NAK;
  u2ps_fake_address = 0;
  u2ps_fake_test_selector = -1;
  u2ps_fake_init_err = U2PS_DEV_OK;
  u2ps_fake_dma_ok = true;
  u2ps_fake_attached = false;
  upcall = U2PS_FAKE_FROM_THREAD;
  masked = false;
  pending_address = -1;
  setup_on_mask = false;
  setup_mask_skip = 0;
  setup_held = false;
  u2ps_fake_masked_bytes_max = 0;
  u2ps_fake_on_mask = NULL;
  u2ps_fake_deliver_stale = false;
  u2ps_fake_stale_dropped = 0;
  unreported = false;
  u2ps_fake_stream_state = U2PS_FAKE_ST_IDLE;
  memset(&u2ps_fake_stream, 0, sizeof u2ps_fake_stream);
  u2ps_fake_open_result = true;
  u2ps_fake_fb = 0;
  stream_forget();
  u2ps_fake_last_data = NULL;
  memset(&u2ps_fake_stats, 0, sizeof u2ps_fake_stats);
  ring_next = 0;
}

int u2ps_fake_find(u2ps_fake_kind_t kind, unsigned start)
{
  for (unsigned i = start; i < u2ps_fake_ncalls; i++)
    if (u2ps_fake_calls[i].kind == kind)
      return (int)i;
  return -1;
}

u2ps_fake_call_t u2ps_fake_get(u2ps_fake_kind_t kind, unsigned start)
{
  int i = u2ps_fake_find(kind, start);
  if (i >= 0)
    return u2ps_fake_calls[i];
  return (u2ps_fake_call_t){.kind = U2PS_FAKE_MISSING, .from = (u2ps_fake_from_t)-1,
                            .arg = 0xFFFFFFFFu, .len = 0xFFFFu, .zlp = true};
}

unsigned u2ps_fake_count(u2ps_fake_kind_t kind)
{
  unsigned n = 0;
  for (unsigned i = 0; i < u2ps_fake_ncalls; i++)
    n += u2ps_fake_calls[i].kind == kind;
  return n;
}

/* An EP0 data stage lies in the DMA memory the stack checked at init */
static void check_dma(const char *call, const void *data, uint16_t len)
{
  uintptr_t a = (uintptr_t)data, m = (uintptr_t)u2ps_fake_dma_mem;
  if (len == 0u)
    return;
  if (data == NULL || a < m || a - m > u2ps_fake_dma_len || len > u2ps_fake_dma_len - (a - m)) {
    char msg[96];
    snprintf(msg, sizeof msg, "%s data outside the DMA memory the stack checked at init", call);
    violation(msg);
  }
}

/* ---- The port interface ----------------------------------------------------- */

bool u2ps_dport_dma_check(const void *mem, uint32_t len)
{
  log_call(U2PS_FAKE_DMA_CHECK, u2ps_fake_dma_ok);
  if (from() != U2PS_FAKE_FROM_THREAD)
    violation("dma_check not from thread with the vectors unmasked");
  u2ps_fake_dma_mem = mem;
  u2ps_fake_dma_len = len;
  return u2ps_fake_dma_ok;
}

u2ps_dev_err_t u2ps_dport_init(void)
{
  log_call(U2PS_FAKE_INIT, 0);
  if (port_up)
    violation("init after a successful init: reinit is the call");
  if (u2ps_fake_init_err == U2PS_DEV_ERR_DESCRIPTION)
    violation("test: a port never returns U2PS_DEV_ERR_DESCRIPTION");
  port_up = u2ps_fake_init_err == U2PS_DEV_OK;
  return u2ps_fake_init_err;
}

void u2ps_dport_reinit(void)
{
  log_call(U2PS_FAKE_REINIT, 0);
  if (!port_up)
    violation("reinit before a successful init");
  if (from() != U2PS_FAKE_FROM_MASKED)
    violation("reinit without irq_mask, or inside an upcall");
  if (u2ps_fake_attached)
    violation("reinit while attached");
  if (u2ps_fake_stream_state != U2PS_FAKE_ST_IDLE)
    violation("reinit with a stream");
  u2ps_fake_ep0 = U2PS_FAKE_EP0_NAK;
  u2ps_fake_address = 0;
  pending_address = -1;
}

void u2ps_dport_connect(bool on)
{
  log_call(U2PS_FAKE_CONNECT, on);
  if (!port_up)
    violation("connect before a successful init");
  if (on) {
#if U2PS_VBUS == U2PS_VBUS_APP
    if (!u2ps_fake_vbus)
      violation("pull-up on without VBUS (USB 2.0 7.1.5.1)");
#endif
    u2ps_fake_attached = true;
    return;
  }
  /* A detach stops the stream and EP0 as a bus reset does, and reports
   * the stream's tail here */
  if (from() != U2PS_FAKE_FROM_MASKED)
    violation("connect(false) without irq_mask, or inside an upcall");
  u2ps_fake_attached = false;
  stream_tail();
  u2ps_fake_stream_state = U2PS_FAKE_ST_IDLE;
  stream_forget();
  u2ps_fake_ep0 = U2PS_FAKE_EP0_NAK;
  u2ps_fake_address = 0;
  pending_address = -1;
  u2ps_fake_test_selector = -1;
}

bool u2ps_fake_masked(void)
{
  return masked;
}

void u2ps_fake_vbus_set(bool present)
{
  u2ps_fake_vbus = present;
#if U2PS_VBUS == U2PS_VBUS_APP
  u2ps_dev_vbus(present);
  if (!present && u2ps_fake_attached)
    violation("pull-up left on after VBUS loss (USB 2.0 7.2.1)");
#endif
}

void u2ps_dport_irq_mask(bool m)
{
  log_call(U2PS_FAKE_IRQ_MASK, m);
  if (upcall != U2PS_FAKE_FROM_THREAD)
    violation("irq_mask inside an upcall");
  if (m == masked)
    violation(m ? "irq_mask(true) while masked" : "irq_mask(false) while unmasked");
  masked = m;
  if (m && u2ps_fake_on_mask != NULL) {
    void (*f)(void) = u2ps_fake_on_mask;
    u2ps_fake_on_mask = NULL;
    f();
  }
  if (m) {
    memcpy(ep0_buf_at_mask, u2ps_dev_ep0_buf, sizeof ep0_buf_at_mask);
  } else {
    unsigned changed = 0;
    for (unsigned i = 0; i < sizeof ep0_buf_at_mask; i++)
      changed += ep0_buf_at_mask[i] != u2ps_dev_ep0_buf[i];
    if (changed > u2ps_fake_masked_bytes_max)
      u2ps_fake_masked_bytes_max = changed;
  }
  if (m && setup_on_mask && setup_mask_skip > 0u) {
    setup_mask_skip--;
  } else if (m && setup_on_mask) {
    /* The SETUP arrives now: it aborts any armed stage (u2ps_dport.h) */
    setup_on_mask = false;
    setup_held = true;
    u2ps_fake_ep0 = U2PS_FAKE_EP0_NAK;
    pending_address = -1;
  }
  if (!m && setup_held) {
    /* The vector, unmasked, takes the held SETUP at once */
    setup_held = false;
    status_in_this_setup = false;
    setup_gen++;
    upcall = U2PS_FAKE_FROM_SETUP;
    u2ps_dev_on_setup(held_setup);
    upcall = U2PS_FAKE_FROM_THREAD;
  }
}

/* An EP0 call from thread while a SETUP is held arms nothing */
static bool drop_held(u2ps_fake_call_t *c)
{
  if (!setup_held || upcall != U2PS_FAKE_FROM_THREAD)
    return false;
  c->dropped = true;
  return true;
}

void u2ps_dport_set_address(uint8_t addr)
{
  log_call(U2PS_FAKE_SET_ADDRESS, addr);
  if (upcall != U2PS_FAKE_FROM_SETUP)
    violation("set_address outside on_setup");
  if (status_in_this_setup)
    violation("set_address after ep0_status");
  if (addr > 127u)
    violation("set_address above 127");
  pending_address = addr;
}

void u2ps_dport_test_mode(uint8_t selector)
{
  log_call(U2PS_FAKE_TEST_MODE, selector);
  if (upcall != U2PS_FAKE_FROM_STATUS)
    violation("test_mode outside on_ep0_status");
  u2ps_fake_test_selector = selector;
}

void u2ps_dport_ep0_in(const void *data, uint16_t len, bool zlp)
{
  u2ps_fake_call_t *c = log_call(U2PS_FAKE_EP0_IN, 0);
  c->len = len;
  c->zlp = zlp;
  check_ep0_context("ep0_in");
  if (drop_held(c))
    return;
  check_ep0_idle("ep0_in");
  check_dma("ep0_in", data, len);
  ep0_in_data = data;
  ep0_len = len;
  ep0_zlp = zlp;
  u2ps_fake_ep0 = U2PS_FAKE_EP0_IN_DATA;
}

void u2ps_dport_ep0_out(void *data, uint16_t len)
{
  u2ps_fake_call_t *c = log_call(U2PS_FAKE_EP0_OUT, 0);
  c->len = len;
  check_ep0_context("ep0_out");
  if (drop_held(c))
    return;
  check_ep0_idle("ep0_out");
  check_dma("ep0_out", data, len);
  ep0_out_buf = data;
  ep0_len = len;
  u2ps_fake_ep0 = U2PS_FAKE_EP0_OUT_DATA;
}

void u2ps_dport_ep0_status(bool in)
{
  u2ps_fake_call_t *c = log_call(U2PS_FAKE_EP0_STATUS, in);
  check_ep0_context("ep0_status");
  if (drop_held(c))
    return;
  check_ep0_idle("ep0_status");
  if (upcall == U2PS_FAKE_FROM_SETUP)
    status_in_this_setup = true;
  u2ps_fake_ep0 = in ? U2PS_FAKE_EP0_STATUS_IN : U2PS_FAKE_EP0_STATUS_OUT;
}

void u2ps_dport_ep0_stall(void)
{
  u2ps_fake_call_t *c = log_call(U2PS_FAKE_EP0_STALL, 0);
  check_ep0_context("ep0_stall");
  if (drop_held(c))
    return;
  u2ps_fake_ep0 = U2PS_FAKE_EP0_STALLED;
}

/* Stream calls come from an upcall or from thread with the vectors masked;
 * release and fb_set from any context */
static void check_stream_context(const char *call)
{
  if (from() == U2PS_FAKE_FROM_THREAD) {
    char msg[96];
    snprintf(msg, sizeof msg, "%s from thread without irq_mask", call);
    violation(msg);
  }
}

/* u2ps_stream_t against the port's limits (u2ps_dport.h) */
static bool stream_ok(const u2ps_stream_t *s)
{
  return s->ep_out >= 1u && s->ep_out <= U2PS_DPORT_EP_MAX &&
         s->ep_fb <= U2PS_DPORT_EP_MAX && s->fb_size == 4u && s->fb_interval >= 1u &&
         s->fb_interval <= 16u && s->mps >= 1u && s->mps <= U2PS_ISO_OUT_MPS_MAX &&
         s->stride >= U2PS_DPORT_STRIDE(s->mps) && s->count >= U2PS_DPORT_ISO_OUT_MIN &&
#if U2PS_ISO_OUT_COUNT_MAX < 0xFFFFu
         s->count <= U2PS_ISO_OUT_COUNT_MAX &&
#endif
         s->mem != NULL &&
         (uintptr_t)s->mem % U2PS_DPORT_ALIGN == 0u;
}

/* n microframes for 'why', in on_missed calls of at most 65,535 each */
static void missed(uint32_t n, u2ps_miss_t why)
{
  for (; n > 0xFFFFu; n -= 0xFFFFu)
    u2ps_dev_on_missed(0xFFFFu, why);
  if (n != 0u)
    u2ps_dev_on_missed((uint16_t)n, why);
}

/* The holes after the last delivered packet, up to the last packet
 * received: before the next packet, at the stream's end, or when the
 * device stack asks (u2ps_dport_report_holes) */
static void stream_tail(void)
{
  if (u2ps_fake_stream_state == U2PS_FAKE_ST_IDLE)
    return;
  missed(tail_no_buffer, U2PS_MISS_NO_BUFFER);
  missed(tail_no_packet, U2PS_MISS_NO_PACKET);
  tail_no_buffer = tail_no_packet = 0;
}

bool u2ps_dport_stream_open(const u2ps_stream_t *s)
{
  log_call(U2PS_FAKE_STREAM_OPEN, s != NULL ? s->fb_value : 0u);
  check_stream_context("stream_open");
  if (u2ps_fake_stream_state != U2PS_FAKE_ST_IDLE)
    violation("stream_open before the last stream's on_closed");
  if (s == NULL || !stream_ok(s) || !u2ps_fake_open_result)
    return false;
  u2ps_fake_stream = *s;
  u2ps_fake_stream_state = U2PS_FAKE_ST_OPEN;
  u2ps_fake_fb = s->fb_value;
  stream_forget();
  ring_next = 0;
  return true;
}

void u2ps_dport_stream_close(void)
{
  log_call(U2PS_FAKE_STREAM_CLOSE, 0);
  check_stream_context("stream_close");
  if (u2ps_fake_stream_state != U2PS_FAKE_ST_OPEN)
    violation("stream_close without an open stream");
  u2ps_fake_stream_state = U2PS_FAKE_ST_CLOSING;
}

void u2ps_dport_release(uint16_t n)
{
  log_call(U2PS_FAKE_RELEASE, n);
  if (n > u2ps_fake_held) {
    violation("release of more packets than are held");
    n = (uint16_t)u2ps_fake_held;
  }
  u2ps_fake_held -= n;
}

/* The holes held back, at a tick in a long gap: as the tail, and the
 * stream goes on */
void u2ps_dport_report_holes(void)
{
  log_call(U2PS_FAKE_REPORT_HOLES, 0);
  if (from() != U2PS_FAKE_FROM_MASKED)
    violation("report_holes without irq_mask, or inside an upcall");
  if (u2ps_fake_stream_state != U2PS_FAKE_ST_OPEN) {
    violation("report_holes without an open stream");
    return;
  }
  stream_tail();
}

void u2ps_dport_fb_set(uint32_t value)
{
  log_call(U2PS_FAKE_FB_SET, value);
  u2ps_fake_fb = value;
}

const u2ps_dport_stats_t *u2ps_dport_stats(void)
{
  log_call(U2PS_FAKE_STATS, 0);
  return &u2ps_fake_stats;
}

/* ---- Injections, as the port's interrupt ------------------------------------ */

static void enter(u2ps_fake_from_t u)
{
  if (masked)
    violation("interrupt taken while the vectors are masked");
  if (upcall != U2PS_FAKE_FROM_THREAD)
    violation("nested upcall");
  upcall = u;
}

static void leave(void)
{
  upcall = U2PS_FAKE_FROM_THREAD;
}

void u2ps_fake_bus_reset(u2ps_speed_t speed)
{
  /* A reset aborts EP0 and returns the port to address 0; the port stops a
   * stream itself, reports its tail, and all class memory returns */
  u2ps_fake_ep0 = U2PS_FAKE_EP0_NAK;
  u2ps_fake_address = 0;
  pending_address = -1;
  enter(U2PS_FAKE_FROM_RESET);
  stream_tail();
  u2ps_fake_stream_state = U2PS_FAKE_ST_IDLE;
  stream_forget();
  u2ps_dev_on_reset();
  leave();
  enter(U2PS_FAKE_FROM_OTHER);
  u2ps_dev_on_speed(speed);
  leave();
}

void u2ps_fake_suspend(void)
{
  enter(U2PS_FAKE_FROM_OTHER);
  u2ps_dev_on_suspend();
  leave();
}

void u2ps_fake_resume(void)
{
  enter(U2PS_FAKE_FROM_OTHER);
  u2ps_dev_on_resume();
  leave();
}

void u2ps_fake_setup(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                     uint16_t wLength)
{
  const uint8_t setup[8] = {bmRequestType,         bRequest,     (uint8_t)wValue,
                            (uint8_t)(wValue >> 8), (uint8_t)wIndex, (uint8_t)(wIndex >> 8),
                            (uint8_t)wLength,       (uint8_t)(wLength >> 8)};
  /* A SETUP ends any stage or STALL; both controllers NAK until armed */
  u2ps_fake_ep0 = U2PS_FAKE_EP0_NAK;
  pending_address = -1;
  status_in_this_setup = false;
  cur.bmRequestType = bmRequestType;
  cur.wLength = wLength;
  cur.data_done = false;
  setup_gen++;
  enter(U2PS_FAKE_FROM_SETUP);
  u2ps_dev_on_setup(setup);
  leave();
}

void u2ps_fake_ep0_in_done(void)
{
  if (u2ps_fake_ep0 != U2PS_FAKE_EP0_IN_DATA) {
    violation("test: IN data completed while not armed");
    return;
  }
  u2ps_fake_ep0 = U2PS_FAKE_EP0_NAK;
  enter(U2PS_FAKE_FROM_DATA);
  u2ps_dev_on_ep0_data(ep0_len);
  leave();
}

void u2ps_fake_ep0_out_done(const void *data, uint16_t len)
{
  if (u2ps_fake_ep0 != U2PS_FAKE_EP0_OUT_DATA || len > ep0_len) {
    violation("test: OUT data completed while not armed, or too long");
    return;
  }
  memcpy(ep0_out_buf, data, len);
  u2ps_fake_ep0 = U2PS_FAKE_EP0_NAK;
  enter(U2PS_FAKE_FROM_DATA);
  u2ps_dev_on_ep0_data(len);
  leave();
}

void u2ps_fake_ep0_status_done(void)
{
  if (u2ps_fake_ep0 != U2PS_FAKE_EP0_STATUS_IN && u2ps_fake_ep0 != U2PS_FAKE_EP0_STATUS_OUT) {
    violation("test: status completed while not armed");
    return;
  }
  u2ps_fake_ep0 = U2PS_FAKE_EP0_NAK;
  if (pending_address >= 0) {
    u2ps_fake_address = (uint8_t)pending_address;
    pending_address = -1;
  }
  enter(U2PS_FAKE_FROM_STATUS);
  u2ps_dev_on_ep0_status();
  leave();
}

void u2ps_fake_ep0_status_done_unreported(void)
{
  if (u2ps_fake_ep0 != U2PS_FAKE_EP0_STATUS_IN && u2ps_fake_ep0 != U2PS_FAKE_EP0_STATUS_OUT) {
    violation("test: status completed while not armed");
    return;
  }
  u2ps_fake_ep0 = U2PS_FAKE_EP0_NAK;
  if (pending_address >= 0) {
    u2ps_fake_address = (uint8_t)pending_address;
    pending_address = -1;
  }
  unreported = true;
  unreported_gen = setup_gen;
}

void u2ps_fake_report_status(void)
{
  if (!unreported) {
    violation("test: no status completion to report");
    return;
  }
  unreported = false;
  if (unreported_gen != setup_gen) {
    /* The stage was armed before the last delivered SETUP: drop it */
    if (!u2ps_fake_deliver_stale) {
      u2ps_fake_stale_dropped++;
      return;
    }
    violation("completion of a stage armed before the last SETUP delivered");
  }
  enter(U2PS_FAKE_FROM_STATUS);
  u2ps_dev_on_ep0_status();
  leave();
}

void u2ps_fake_ep0_in_ack_lost(void)
{
  /* The host has all the data; its status OUT stands for the lost ACK */
  u2ps_fake_ep0_in_done();
  if (u2ps_fake_ep0 != U2PS_FAKE_EP0_STATUS_OUT) {
    violation("lost ACK: the stack did not arm the status OUT in on_ep0_data");
    return;
  }
  u2ps_fake_ep0_status_done();
}

void u2ps_fake_setup_on_mask(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                             uint16_t wIndex, uint16_t wLength)
{
  u2ps_fake_setup_on_mask_nth(1, bmRequestType, bRequest, wValue, wIndex, wLength);
}

void u2ps_fake_setup_on_mask_nth(unsigned nth, uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex, uint16_t wLength)
{
  const uint8_t setup[8] = {bmRequestType,         bRequest,     (uint8_t)wValue,
                            (uint8_t)(wValue >> 8), (uint8_t)wIndex, (uint8_t)(wIndex >> 8),
                            (uint8_t)wLength,       (uint8_t)(wLength >> 8)};
  memcpy(held_setup, setup, sizeof held_setup);
  setup_on_mask = true;
  setup_mask_skip = nth > 1u ? nth - 1u : 0u;
  cur.bmRequestType = bmRequestType;
  cur.wLength = wLength;
  cur.data_done = false;
}

/* ---- Stream injections, as the port's interrupt ------------------------------ */

/* A packet into the next buffer: the bytes at src, or the running counter */
static void packet(const uint8_t *src, uint16_t len, u2ps_uframe_t uframe)
{
  if (u2ps_fake_stream_state == U2PS_FAKE_ST_IDLE || len > u2ps_fake_stream.mps) {
    violation("test: packet without a stream, or longer than mps");
    return;
  }
  enter(U2PS_FAKE_FROM_STREAM);
  if (u2ps_fake_held + U2PS_DPORT_ISO_OUT_RESERVE >= u2ps_fake_stream.count) {
    /* A reserve packet: NO_BUFFER in its own microframe, the microframes
     * since the last packet without one NO_PACKET; not delivered */
    if (last_rx != U2PS_UFRAME_UNKNOWN && uframe != U2PS_UFRAME_UNKNOWN &&
        u2ps_uframe_dist(last_rx, uframe) > 0)
      tail_no_packet += (uint32_t)u2ps_uframe_dist(last_rx, uframe) - 1u;
    tail_no_buffer++;
  } else {
    stream_tail();
    uint8_t *data = u2ps_fake_stream.mem + (size_t)ring_next * u2ps_fake_stream.stride;
    for (unsigned i = 0; i < len; i++)
      data[i] = src != NULL ? src[i] : seq++;
    ring_next = (ring_next + 1u) % u2ps_fake_stream.count;
    u2ps_fake_held++;
    u2ps_fake_last_data = data;
    u2ps_dev_on_packet(data, len, uframe);
  }
  last_rx = uframe;
  leave();
}

void u2ps_fake_packet(uint16_t len, u2ps_uframe_t uframe)
{
  packet(NULL, len, uframe);
}

void u2ps_fake_packet_bytes(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  packet(data, len, uframe);
}

void u2ps_fake_missed(uint16_t uframes, u2ps_miss_t why)
{
  if (u2ps_fake_stream_state == U2PS_FAKE_ST_IDLE) {
    violation("test: hole without a stream");
    return;
  }
  enter(U2PS_FAKE_FROM_STREAM);
  u2ps_dev_on_missed(uframes, why);
  leave();
}

void u2ps_fake_closed(void)
{
  if (u2ps_fake_stream_state != U2PS_FAKE_ST_CLOSING) {
    violation("test: on_closed without a close");
    return;
  }
  enter(U2PS_FAKE_FROM_STREAM);
  stream_tail();
  u2ps_fake_stream_state = U2PS_FAKE_ST_IDLE;
  stream_forget();
  u2ps_dev_on_closed();
  leave();
}

/* ---- The host ------------------------------------------------------------------ */

static u2ps_fake_host_result_t fake_host_error(const char *why)
{
  printf("host: %s\n", why);
  return U2PS_FAKE_HOST_ERROR;
}

/* The IN data stage, packet by packet. The device sends max-size packets,
 * the rest in a short one, and a zero-length packet if armed with zlp (or
 * for len 0). The host takes packets until it has wLength bytes or gets a
 * short packet (USB 2.0 5.5.3); a packet after that, or none when the host
 * still waits, is a fault. */
static u2ps_fake_host_result_t fake_host_in_data(uint8_t *data, uint16_t *actual)
{
  uint16_t sent = 0, got = 0;
  bool zero = ep0_len == 0u || ep0_zlp;
  bool ended = false;

  if (ep0_len > cur.wLength)
    return fake_host_error("IN data stage longer than wLength");
  for (;;) {
    int size;
    if (sent < ep0_len) {
      int left = ep0_len - sent;
      size = left > (int)U2PS_EP0_MPS ? (int)U2PS_EP0_MPS : left;
    } else if (zero) {
      size = 0;
      zero = false;
    } else {
      size = -1; /* the device has nothing more and NAKs */
    }
    if (ended) {
      if (size >= 0)
        return fake_host_error("device sends a packet after the data stage ended");
      break;
    }
    if (size < 0)
      return fake_host_error("host waits for data that never comes (missing short packet)");
    if (data != NULL)
      memcpy(data + got, ep0_in_data + sent, (size_t)size);
    sent = (uint16_t)(sent + size);
    got = (uint16_t)(got + size);
    if (size < (int)U2PS_EP0_MPS || got == cur.wLength)
      ended = true;
  }
  if (actual != NULL)
    *actual = got;
  u2ps_fake_ep0_in_done();
  return U2PS_FAKE_HOST_OK;
}

u2ps_fake_host_result_t u2ps_fake_host_resume(void *data, uint16_t *actual)
{
  bool in = (cur.bmRequestType & 0x80u) != 0u && cur.wLength > 0u;
  bool out = (cur.bmRequestType & 0x80u) == 0u && cur.wLength > 0u;

  if (u2ps_fake_ep0 == U2PS_FAKE_EP0_STALLED)
    return U2PS_FAKE_HOST_STALL;
  if (u2ps_fake_ep0 == U2PS_FAKE_EP0_NAK)
    return U2PS_FAKE_HOST_NAK;

  if (!cur.data_done && (in || out)) {
    if (in && u2ps_fake_ep0 == U2PS_FAKE_EP0_IN_DATA) {
      u2ps_fake_host_result_t r = fake_host_in_data(data, actual);
      if (r != U2PS_FAKE_HOST_OK)
        return r;
    } else if (out && u2ps_fake_ep0 == U2PS_FAKE_EP0_OUT_DATA) {
      if (ep0_len != cur.wLength)
        return fake_host_error("OUT data stage armed for a length other than wLength");
      u2ps_fake_ep0_out_done(data, cur.wLength);
      if (actual != NULL)
        *actual = cur.wLength;
    } else {
      return fake_host_error("device armed a stage other than the data stage");
    }
    cur.data_done = true;
    if (u2ps_fake_ep0 == U2PS_FAKE_EP0_STALLED)
      return U2PS_FAKE_HOST_STALL;
    if (u2ps_fake_ep0 == U2PS_FAKE_EP0_NAK)
      return U2PS_FAKE_HOST_NAK;
  }

  /* Status stage: opposite to the data stage, IN if there was none */
  u2ps_fake_ep0_t want = in ? U2PS_FAKE_EP0_STATUS_OUT : U2PS_FAKE_EP0_STATUS_IN;
  if (u2ps_fake_ep0 != want)
    return fake_host_error("device armed the wrong stage for the status stage");
  if (!in && !out && actual != NULL)
    *actual = 0;
  u2ps_fake_ep0_status_done();
  return U2PS_FAKE_HOST_OK;
}

u2ps_fake_host_result_t u2ps_fake_host_control(uint8_t bmRequestType, uint8_t bRequest,
                                               uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                               void *data, uint16_t *actual)
{
  if (actual != NULL)
    *actual = 0;
  u2ps_fake_setup(bmRequestType, bRequest, wValue, wIndex, wLength);
  return u2ps_fake_host_resume(data, actual);
}
