/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_host_contract.c: the host stack's contract checks, against the
 * library built with U2PS_CONTRACT_CHECKS=1. Each violation of
 * u2ps_contract_t that the host stack checks is provoked, and reported once
 * with its code; the stack then goes on as without the checks. The nearby
 * uses the header allows report nothing:
 * - u2ps_host_connect(true) before a successful init (first, while the
 *   process has never initialised the stack);
 * - a thread call inside a callback, in the interrupt and in thread with
 *   the vectors masked, for each thread call; a thread call, and a queue
 *   from outside a callback, while another thread call holds the mask;
 * - a tick more than 10 ms after the last while a wait or a control
 *   transfer is timed, and not otherwise;
 * - queues for a number never returned, for a stream that does not run
 *   inside a callback (not from thread), with data NULL or misaligned,
 *   into a full queue, and without u2ps_host_slots_next();
 * - a queued packet outside the DMA ranges the port reported at init
 *   (u2ps_hport_dma_ranges), or across a range's end: reported, and
 *   queued as without the checks.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "host_support.h"

#if !U2PS_CONTRACT_CHECKS
#error "test_host_contract: link a library built with U2PS_CONTRACT_CHECKS=1"
#endif

#define C(code) contract_log.count[U2PS_CONTRACT_##code]

static bool stream_decided(void)
{
  return happ_log.stream_starts > 0u || happ_log.stream_stops > 0u;
}

/* Microframes until on_stream_stop, at most 100 ms */
static bool wait_stopped(void)
{
  for (unsigned i = 0; i < 800u && happ_log.stream_stops == 0u; i++)
    u2ps_hfake_uframes(1);
  return happ_log.stream_stops > 0u;
}

/* The device configured, a stream started at alternate setting 1 and
 * 48 kHz with the player off; reports expected from here on. Returns the
 * stream's number, 0 if it did not start. */
static uint32_t streaming(void)
{
  if (!hstart("uac2-hs-default.txt") || !henumerate() || happ_log.devices != 1u) {
    printf("  not configured\n");
    return 0;
  }
  hplayer_start();
  hplayer.on = false;
  uint32_t s = u2ps_host_stream_start(1, 48000);
  if (s == 0u || !u2ps_hfake_wait(stream_decided, 100) || happ_log.stream_starts != 1u) {
    printf("  no stream\n");
    return 0;
  }
  contract_expect(true);
  return s;
}

/* A full queue and two more; test_queue_unsized uses five */
static _Alignas(32) uint8_t pk[(U2PS_HOST_QUEUE_MAX > 3u ? U2PS_HOST_QUEUE_MAX : 3u) + 2u][256];

/* ---- u2ps_host_connect(true) before a successful u2ps_host_init() ---------- */

static void test_connect_before_init(void)
{
  u2ps_hfake_reset_all();
  contract_expect(true);
  u2ps_host_vbus(true);
  u2ps_host_connect(false);
  CHECK_EQ(contract_log.total, 0);
  u2ps_host_connect(true);
  CHECK_EQ(C(HOST_CONNECT_BEFORE_INIT), 1);
  CHECK_EQ(contract_log.total, 1);
  u2ps_hfake_init_err = U2PS_HOST_ERR_PHY;
  CHECK_EQ(u2ps_host_init(), U2PS_HOST_ERR_PHY);
  u2ps_host_connect(true);
  CHECK_EQ(C(HOST_CONNECT_BEFORE_INIT), 2);
  CHECK_EQ(contract_log.total, 2);
  u2ps_hfake_init_err = U2PS_HOST_OK;
  contract_expect(true);
  CHECK_EQ(u2ps_host_init(), U2PS_HOST_OK);
  u2ps_host_connect(true);
  u2ps_host_connect(false);
  CHECK_EQ(contract_log.total, 0);
  contract_expect(false);
}

/* ---- Thread calls in the wrong context ----------------------------------------- */

typedef enum {
  CALL_INIT,
  CALL_CONNECT,
  CALL_VBUS,
  CALL_TICK,
  CALL_TEST_MODE,
  CALL_STREAM_START,
  CALL_STREAM_STOP,
  CALL_STATS,
  CALLS
} call_t;
static const char *const call_names[CALLS] = {"init",         "connect",     "vbus",
                                              "tick",         "test_mode",   "stream_start",
                                              "stream_stop",  "stats"};

static call_t call_kind;

static void thread_call(void)
{
  u2ps_host_stats_t st;
  switch (call_kind) {
  case CALL_INIT:
    (void)u2ps_host_init();
    break;
  case CALL_CONNECT:
    u2ps_host_connect(true);
    break;
  case CALL_VBUS:
    u2ps_host_vbus(true);
    break;
  case CALL_TICK:
    u2ps_host_tick(u2ps_hfake_now);
    break;
  case CALL_TEST_MODE:
    (void)u2ps_host_test_mode(0);
    break;
  case CALL_STREAM_START:
    (void)u2ps_host_stream_start(0, 0);
    break;
  case CALL_STREAM_STOP:
    u2ps_host_stream_stop(0);
    break;
  case CALL_STATS:
    u2ps_host_stats(&st);
    break;
  default:
    break;
  }
}

static bool once;

static void call_in_sent(u2ps_uframe_t uframe)
{
  (void)uframe;
  if (once) {
    once = false;
    thread_call();
  }
}

static void call_in_stop(u2ps_host_stop_t why)
{
  (void)why;
  if (once) {
    once = false;
    thread_call();
  }
}

static void call_in_end(u2ps_host_end_t why)
{
  (void)why;
  if (once) {
    once = false;
    thread_call();
  }
}

/* Each thread call inside on_sent, in the port's interrupt, and inside
 * on_stream_stop(END) and on_end under u2ps_host_connect(false), in thread
 * with the vectors masked */
static void test_call_in_callback(void)
{
  for (call_kind = CALL_INIT; call_kind < CALLS; call_kind++) {
    uint32_t s = streaming();
    uint16_t n = u2ps_host_slots_next(s);
    (void)n;
    CHECK(u2ps_host_queue(s, pk[0]));
    once = true;
    happ_sent_hook = call_in_sent;
    u2ps_hfake_uframes(2);
    if (C(HOST_CALL_IN_CALLBACK) != 1u || contract_log.total != 1u)
      printf("  in on_sent: %s\n", call_names[call_kind]);
    CHECK(!once);
    CHECK_EQ(C(HOST_CALL_IN_CALLBACK), 1);
    CHECK_EQ(contract_log.total, 1);
    happ_sent_hook = NULL;

    streaming();
    once = true;
    happ_stream_stop_hook = call_in_stop;
    u2ps_host_connect(false);
    if (C(HOST_CALL_IN_CALLBACK) != 1u || contract_log.total != 1u)
      printf("  in on_stream_stop: %s\n", call_names[call_kind]);
    CHECK(!once);
    CHECK_EQ(C(HOST_CALL_IN_CALLBACK), 1);
    CHECK_EQ(contract_log.total, 1);
    happ_stream_stop_hook = NULL;

    streaming();
    once = true;
    happ_end_hook = call_in_end;
    u2ps_host_connect(false);
    if (C(HOST_CALL_IN_CALLBACK) != 1u || contract_log.total != 1u)
      printf("  in on_end: %s\n", call_names[call_kind]);
    CHECK(!once);
    CHECK_EQ(C(HOST_CALL_IN_CALLBACK), 1);
    CHECK_EQ(contract_log.total, 1);
    happ_end_hook = NULL;
  }
  contract_expect(false);
}

/* Each thread call while another thread call holds the mask, as an
 * interrupt of another source, or a second thread, would make it */
static void test_call_nested(void)
{
  for (call_kind = CALL_INIT; call_kind < CALLS; call_kind++) {
    streaming();
    u2ps_hfake_on_mask = thread_call;
    u2ps_host_stats(&(u2ps_host_stats_t){0});
    if (C(HOST_CALL_NESTED) != 1u || contract_log.total != 1u)
      printf("  nested: %s\n", call_names[call_kind]);
    CHECK_EQ(C(HOST_CALL_NESTED), 1);
    CHECK_EQ(contract_log.total, 1);
  }
  contract_expect(false);
}

static uint32_t queue_stream;
static const uint8_t *queue_data;
static bool queue_ok;

static void queue_only(void)
{
  queue_ok = u2ps_host_queue(queue_stream, queue_data);
}

static void queue_now(void)
{
  (void)u2ps_host_slots_next(queue_stream);
  queue_only();
}

static void queue_in_sent(u2ps_uframe_t uframe)
{
  (void)uframe;
  if (once) {
    once = false;
    queue_now();
  }
}

/* u2ps_host_queue() from outside a callback while a thread call holds the
 * mask; inside a callback it is fine, in the interrupt and in thread */
static void test_queue_nested(void)
{
  queue_stream = streaming();
  queue_data = pk[0];
  (void)u2ps_host_slots_next(queue_stream);
  u2ps_hfake_on_mask = queue_only;
  u2ps_host_stats(&(u2ps_host_stats_t){0});
  CHECK_EQ(C(HOST_CALL_NESTED), 1);
  CHECK_EQ(contract_log.total, 1);
  /* The stack went on as without the check: the packet was queued */
  CHECK(queue_ok);
  CHECK_EQ(u2ps_hfake_queued, 1);

  contract_expect(true);
  queue_data = pk[1];
  once = true;
  happ_sent_hook = queue_in_sent;
  u2ps_hfake_uframes(2);
  CHECK(!once);
  CHECK(queue_ok);
  CHECK_EQ(contract_log.total, 0);
  happ_sent_hook = NULL;
  contract_expect(false);
}

/* ---- Tick period ------------------------------------------------------------------- */

static void late_step(uint32_t skip)
{
  u2ps_hfake_now += skip;
  u2ps_hfake_step();
}

static void test_tick_late(void)
{
  /* The debounce: timed from its first tick */
  CHECK(hstart("uac2-hs-default.txt"));
  contract_expect(true);
  u2ps_hfake_step();
  u2ps_hfake_connect();
  late_step(30); /* its first tick: nothing was timed before it */
  CHECK_EQ(contract_log.total, 0);
  late_step(9); /* 10 ms after the last */
  CHECK_EQ(contract_log.total, 0);
  late_step(10); /* 11 ms */
  CHECK_EQ(C(HOST_TICK_LATE), 1);
  CHECK_EQ(contract_log.total, 1);
  CHECK(u2ps_hfake_wait(happ_done, 20000));
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(contract_log.total, 1);
  /* Configured, nothing timed: no report */
  late_step(500);
  CHECK_EQ(contract_log.total, 1);

  /* A control transfer that hangs: timed from the first tick after it */
  CHECK(hstart("uac2-hs-default.txt"));
  u2ps_hmodel.hang_at = 0;
  contract_expect(true);
  u2ps_hfake_connect();
  while (!u2ps_hfake_control_pending() && u2ps_hfake_now < 1000u)
    u2ps_hfake_step();
  CHECK(u2ps_hfake_control_pending());
  u2ps_hfake_step();
  CHECK_EQ(contract_log.total, 0);
  late_step(100);
  CHECK_EQ(C(HOST_TICK_LATE), 1);
  CHECK_EQ(contract_log.total, 1);
  contract_expect(false);
  u2ps_host_connect(false);
}

/* ---- Queue ------------------------------------------------------------------------- */

static void queue_in_stop(u2ps_host_stop_t why)
{
  (void)why;
  queue_now();
}

static void queue_in_start(const u2ps_host_stream_info_t *info)
{
  (void)info;
  queue_now();
}

static void test_queue_stream(void)
{
  uint32_t s = streaming();
  /* A number never returned, from thread */
  CHECK(!u2ps_host_queue(0, pk[0]));
  CHECK_EQ(C(HOST_QUEUE_STREAM), 1);
  CHECK(!u2ps_host_queue(s + 1u, pk[0]));
  CHECK_EQ(C(HOST_QUEUE_STREAM), 2);
  CHECK_EQ(contract_log.total, 2);

  /* Inside on_stream_stop: the stream no longer runs */
  contract_expect(true);
  queue_stream = s;
  queue_data = pk[0];
  happ_stream_stop_hook = queue_in_stop;
  u2ps_host_stream_stop(s);
  CHECK(wait_stopped());
  CHECK(!queue_ok);
  CHECK_EQ(C(HOST_QUEUE_STREAM), 1);
  CHECK_EQ(contract_log.total, 1);
  happ_stream_stop_hook = NULL;
  /* From thread, after the stop: not reported (the app cannot tell a stop
   * the interrupt has just made) */
  contract_expect(true);
  CHECK(!u2ps_host_queue(s, pk[0]));
  CHECK_EQ(contract_log.total, 0);
  /* Inside the next stream's on_stream_start, for the old one */
  happ_log.stream_starts = happ_log.stream_stops = 0;
  happ_stream_start_hook = queue_in_start;
  uint32_t s2 = u2ps_host_stream_start(1, 48000);
  CHECK(s2 > s);
  CHECK(u2ps_hfake_wait(stream_decided, 100));
  CHECK_EQ(happ_log.stream_starts, 1);
  CHECK(!queue_ok);
  CHECK_EQ(C(HOST_QUEUE_STREAM), 1);
  CHECK_EQ(contract_log.total, 1);
  /* ... and for itself, fine */
  contract_expect(true);
  queue_stream = s2;
  happ_stream_start_hook = NULL;
  queue_now();
  CHECK(queue_ok);
  CHECK_EQ(contract_log.total, 0);
  contract_expect(false);
}

static void test_queue_refusals(void)
{
  uint32_t s = streaming();
  /* Data NULL, or off U2PS_HPORT_ALIGN */
  (void)u2ps_host_slots_next(s);
  CHECK(!u2ps_host_queue(s, NULL));
  CHECK_EQ(C(HOST_QUEUE_DATA), 1);
  CHECK(!u2ps_host_queue(s, pk[0] + 1));
  CHECK_EQ(C(HOST_QUEUE_DATA), 2);
  CHECK_EQ(contract_log.total, 2);
  /* A full queue */
  contract_expect(true);
  for (unsigned i = 0; i < U2PS_HOST_QUEUE_MAX; i++) {
    (void)u2ps_host_slots_next(s);
    CHECK(u2ps_host_queue(s, pk[i]));
  }
  CHECK_EQ(contract_log.total, 0);
  (void)u2ps_host_slots_next(s);
  CHECK(!u2ps_host_queue(s, pk[U2PS_HOST_QUEUE_MAX]));
  CHECK_EQ(C(HOST_QUEUE_FULL), 1);
  CHECK_EQ(contract_log.total, 1);
  /* After an on_sent, the next one goes in */
  u2ps_hfake_uframes(1);
  CHECK(u2ps_host_queue(s, pk[U2PS_HOST_QUEUE_MAX]));
  CHECK_EQ(contract_log.total, 1);
  contract_expect(false);
}

static void test_queue_unsized(void)
{
  uint32_t s = streaming();
  /* Without slots_next: queued, at the size computed now, and reported */
  CHECK(u2ps_host_queue(s, pk[0]));
  CHECK_EQ(C(HOST_QUEUE_UNSIZED), 1);
  CHECK_EQ(contract_log.total, 1);
  CHECK_EQ(u2ps_hfake_queued, 1);
  /* slots_next once per packet: fine, also called twice */
  (void)u2ps_host_slots_next(s);
  (void)u2ps_host_slots_next(s);
  CHECK(u2ps_host_queue(s, pk[1]));
  CHECK_EQ(contract_log.total, 1);
  /* One slots_next does not size two packets */
  (void)u2ps_host_slots_next(s);
  CHECK(u2ps_host_queue(s, pk[2]));
  CHECK(u2ps_host_queue(s, pk[3]));
  CHECK_EQ(C(HOST_QUEUE_UNSIZED), 2);
  CHECK_EQ(contract_log.total, 2);
  /* slots_next of another number sizes nothing */
  (void)u2ps_host_slots_next(s + 1u);
  CHECK(u2ps_host_queue(s, pk[4]));
  CHECK_EQ(C(HOST_QUEUE_UNSIZED), 3);
  CHECK_EQ(contract_log.total, 3);
  contract_expect(false);
}

/* The fake's ranges until the test restores them: they last across
 * u2ps_hfake_reset_all(), as the port's memory does */
static void ranges_all(void)
{
  u2ps_hfake_ranges[0] = (u2ps_hport_range_t){0u, UINTPTR_MAX};
  u2ps_hfake_range_count = 1;
}

static void test_queue_reach(void)
{
  /* The port's DMA reaches pk[1] and pk[2] only */
  u2ps_hfake_ranges[0] = (u2ps_hport_range_t){(uintptr_t)pk[1], (uintptr_t)pk[3]};
  u2ps_hfake_range_count = 1;
  uint32_t s = streaming();
  CHECK_EQ(u2ps_hfake_range_calls, 1);
  /* Inside: fine */
  (void)u2ps_host_slots_next(s);
  CHECK(u2ps_host_queue(s, pk[1]));
  CHECK_EQ(contract_log.total, 0);
  /* Outside: reported, and queued as without the checks */
  (void)u2ps_host_slots_next(s);
  CHECK(u2ps_host_queue(s, pk[0]));
  CHECK_EQ(C(HOST_QUEUE_DATA), 1);
  CHECK_EQ(contract_log.total, 1);
  CHECK_EQ(u2ps_hfake_queued, 2);
  /* Its start inside, its end past the range: reported */
  (void)u2ps_host_slots_next(s);
  CHECK(u2ps_host_queue(s, pk[3] - 16));
  CHECK_EQ(C(HOST_QUEUE_DATA), 2);
  /* Ending right at the range's end: fine (4 bytes a slot: 2 channels of
   * 2 bytes, alternate setting 1) */
  uint16_t n = u2ps_host_slots_next(s);
  CHECK(n > 0u);
  CHECK(u2ps_host_queue(s, pk[3] - 4u * n));
  CHECK_EQ(C(HOST_QUEUE_DATA), 2);
  /* The ranges are the ones of the init: a change of the port's after it
   * changes nothing */
  ranges_all();
  (void)u2ps_host_slots_next(s);
  CHECK(u2ps_host_queue(s, pk[0]));
  CHECK_EQ(C(HOST_QUEUE_DATA), 3);
  CHECK_EQ(contract_log.total, 3);
  CHECK_EQ(u2ps_hfake_queued, 5);
  CHECK_EQ(u2ps_hfake_violations, 0);
  contract_expect(false);
  /* No range at all: every packet reported */
  u2ps_hfake_range_count = 0;
  s = streaming();
  (void)u2ps_host_slots_next(s);
  CHECK(u2ps_host_queue(s, pk[1]));
  CHECK_EQ(C(HOST_QUEUE_DATA), 1);
  CHECK_EQ(contract_log.total, 1);
  contract_expect(false);
  ranges_all();
}

int main(void)
{
  test_connect_before_init();
  test_call_in_callback();
  test_call_nested();
  test_queue_nested();
  test_tick_late();
  test_queue_stream();
  test_queue_refusals();
  test_queue_unsized();
  test_queue_reach();
  return check_done();
}
