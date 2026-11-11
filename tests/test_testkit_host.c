/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_testkit_host.c: the PC test kit's host side (u2ps_testkit_host.h),
 * through its public API only, with a small player app of its own.
 * Checked:
 * - the scripted device enumerates: host_on_device with its VID, PID,
 *   alternate settings and sampling frequency;
 * - a stream start: the device's sampling frequency and alternate setting
 *   set, host_on_stream_start with its arguments; every packet the app
 *   queued arrives with its bytes, in the microframe of its on_sent;
 * - an injected LATE hole of 3 before the next on_sent, a CORRUPTED hole
 *   of 1 counted by the device, a NO_BUFFER hole when the app stops
 *   queueing; the scripted feedback;
 * - a refused alternate setting: host_on_stream_stop(REFUSED,
 *   SET_INTERFACE); the app's stop: host_on_stream_stop(APP);
 * - the poll at each millisecond, and the time;
 * - a contract violation (a queue with NULL data): recorded, passed to the
 *   app's hook, fails the result until taken; a port violation (an
 *   injection inside a callback) the same;
 * - a disconnect: host_on_end(DISCONNECT);
 * - the device at full and low speed: host_on_end(SPEED), with
 *   U2PS_HOST_REFUSE_FS on or off; as the test
 *   device: host_on_test_device with its PID and bcdDevice, and for 0x0104
 *   host_on_end(TEST_MODE) with the port in test mode 4;
 * - the app's suspend: the port suspended; a remote wakeup:
 *   host_on_resumed(1), counted in wakeups; the app's resume:
 *   host_on_resumed(0).
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "u2ps_testkit_host.h"

/* ---- The app ------------------------------------------------------------------ */

static _Alignas(U2PS_HPORT_ALIGN) uint8_t buf[U2PS_HOST_QUEUE_MAX + 1u][1024];

static struct {
  bool device;           /* host_on_device came */
  bool queueing;         /* queue in on_sent (default true) */
  uint32_t stream;
  uint16_t slot_bytes;
  unsigned in_flight;    /* queued, not yet sent */
  unsigned next;         /* the next buffer */
  uint64_t bytes;        /* bytes queued */
  u2ps_uframe_t sent_uframe;
  unsigned contracts;
  u2ps_contract_t contract;
  unsigned polls;
  bool plug_in_upcall; /* the script's mistake: an injection inside a callback */
} app;

static uint8_t pattern(uint64_t i)
{
  return (uint8_t)(i * 7u + (i >> 8));
}

static void app_init(void)
{
  CHECK(u2ps_host_init() == U2PS_HOST_OK);
  u2ps_host_vbus(true);
  u2ps_host_connect(true);
}

static void app_poll(uint32_t now_ms)
{
  app.polls++;
  u2ps_host_tick(now_ms);
}

static void fill(void)
{
  while (app.queueing && app.in_flight < 4u) {
    uint16_t n = u2ps_host_slots_next(app.stream);
    uint8_t *p = buf[app.next];
    for (unsigned i = 0; i < (unsigned)n * app.slot_bytes; i++)
      p[i] = pattern(app.bytes + i);
    if (!u2ps_host_queue(app.stream, p))
      return;
    app.bytes += (uint64_t)n * app.slot_bytes;
    app.next = (app.next + 1u) % (U2PS_HOST_QUEUE_MAX + 1u);
    app.in_flight++;
  }
}

void u2ps_app_host_on_device(const u2ps_host_device_t *dev)
{
  (void)dev;
  app.device = true;
}

void u2ps_app_host_on_end(u2ps_host_end_t why, u2ps_host_refusal_t refusal)
{
  (void)why;
  (void)refusal;
  app.device = false;
}

void u2ps_app_host_on_stream_start(const u2ps_host_stream_info_t *info)
{
  app.stream = info->stream;
  app.slot_bytes = (uint16_t)(info->nr_channels * info->format.subslot_size);
  app.in_flight = 0;
  fill();
}

void u2ps_app_host_on_sent(u2ps_uframe_t uframe)
{
  app.sent_uframe = uframe;
  app.in_flight--;
  fill();
  if (app.plug_in_upcall) {
    app.plug_in_upcall = false;
    u2ps_testkit_host_connect();
  }
}

void u2ps_app_host_on_hole(uint16_t uframes, u2ps_miss_t why)
{
  (void)uframes;
  (void)why;
}

void u2ps_app_host_on_stream_stop(u2ps_host_stop_t why, u2ps_host_stop_request_t request)
{
  (void)why;
  (void)request;
}

void u2ps_app_host_on_test_device(uint16_t id_product, uint16_t bcd_device)
{
  (void)id_product;
  (void)bcd_device;
}

void u2ps_app_host_on_resumed(bool remote_wakeup)
{
  (void)remote_wakeup;
}

void u2ps_app_on_contract(u2ps_contract_t violation)
{
  app.contracts++;
  app.contract = violation;
}

/* ---- The device's side ------------------------------------------------------------ */

static struct {
  uint64_t bytes;
  unsigned bad;
  u2ps_uframe_t uframe;
} rx;

static void on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  for (unsigned i = 0; i < len; i++)
    rx.bad += data[i] != pattern(rx.bytes + i);
  rx.bytes += len;
  rx.uframe = uframe;
}

/* ---- Helpers ------------------------------------------------------------------- */

static bool device_found(void)
{
  return app.device;
}

static bool call_is(unsigned i, u2ps_testkit_callback_t callback, uint32_t a0, uint32_t a1)
{
  const u2ps_testkit_call_t *c = u2ps_testkit_trace_get(i);
  if (c == NULL || c->callback != callback || c->arg[0] != a0 || c->arg[1] != a1) {
    printf("  trace %u: want %s %u %u\n", i, u2ps_testkit_callback_name(callback), (unsigned)a0,
           (unsigned)a1);
    u2ps_testkit_trace_print();
    return false;
  }
  return true;
}

/* The index of the first call of 'callback' at or after 'from', or -1 */
static int find(u2ps_testkit_callback_t callback, unsigned from)
{
  for (unsigned i = from; i < u2ps_testkit_trace_count(); i++) {
    const u2ps_testkit_call_t *c = u2ps_testkit_trace_get(i);
    if (c != NULL && c->callback == callback)
      return (int)i;
  }
  return -1;
}

static void start(void)
{
  memset(&app, 0, sizeof app);
  memset(&rx, 0, sizeof rx);
  app.queueing = true;
  u2ps_testkit_host_reset();
  u2ps_testkit_set_poll(NULL);
  u2ps_testkit_host_on_packet(on_packet);
  app_init();
  u2ps_testkit_host_connect();
  CHECK(u2ps_testkit_host_run_until(device_found, 2000));
}

/* ---- Tests ------------------------------------------------------------------------ */

static void test_enumerate(void)
{
  printf("enumerate\n");
  start();
  CHECK(u2ps_testkit_trace_match("host_on_device"));
  const u2ps_testkit_call_t *c = u2ps_testkit_trace_get(0);
  CHECK(c != NULL && c->arg[0] == 0x1234u && c->arg[1] == 0x5678u && c->arg[2] == 4u &&
        c->arg[3] == 48000u);
}

static void test_stream(void)
{
  printf("stream\n");
  start();
  u2ps_testkit_trace_clear();
  CHECK(u2ps_host_stream_start(2, 96000) != 0u);
  u2ps_testkit_host_run_ms(20);
  CHECK(u2ps_testkit_host_sampling_frequency() == 96000u);
  CHECK(u2ps_testkit_host_alternate_setting() == 2u);
  const u2ps_testkit_call_t *c = u2ps_testkit_trace_get(0);
  CHECK(c != NULL && c->callback == U2PS_TESTKIT_HOST_ON_STREAM_START && c->arg[0] == app.stream &&
        c->arg[1] == 2u && c->arg[2] == 96000u && c->arg[3] > 0u);
  const u2ps_testkit_received_t *r = u2ps_testkit_host_received();
  CHECK(r->packets > 100u);
  CHECK(r->packets == u2ps_testkit_calls(U2PS_TESTKIT_HOST_ON_SENT));
  CHECK(r->bytes == rx.bytes && rx.bad == 0u);
  CHECK(r->last_uframe == app.sent_uframe && rx.uframe == app.sent_uframe);
  CHECK(r->last_len == 12u * 2u * 4u);
  CHECK(u2ps_testkit_host_queued() > 0u);

  /* LATE: a hole of 3 before the next on_sent */
  u2ps_testkit_trace_clear();
  u2ps_testkit_host_inject_late(3);
  u2ps_testkit_host_run_uframes(8);
  int h = find(U2PS_TESTKIT_HOST_ON_HOLE, 0);
  CHECK(h >= 0 && call_is((unsigned)h, U2PS_TESTKIT_HOST_ON_HOLE, 3, U2PS_MISS_LATE));
  CHECK(h >= 0 && find(U2PS_TESTKIT_HOST_ON_SENT, 0) == h + 1);
  /* CORRUPTED: a hole of 1, counted by the device */
  u2ps_testkit_trace_clear();
  uint32_t corrupted = r->corrupted;
  u2ps_testkit_host_inject_corrupted();
  u2ps_testkit_host_run_uframes(8);
  h = find(U2PS_TESTKIT_HOST_ON_HOLE, 0);
  CHECK(h >= 0 && call_is((unsigned)h, U2PS_TESTKIT_HOST_ON_HOLE, 1, U2PS_MISS_CORRUPTED));
  CHECK(r->corrupted == corrupted + 1u);
  /* NO_BUFFER: the app stops queueing, the queue runs empty, the app
   * queues again */
  app.queueing = false;
  u2ps_testkit_host_run_uframes(16);
  CHECK(u2ps_testkit_host_queued() == 0u);
  u2ps_testkit_trace_clear();
  app.queueing = true;
  app.in_flight = 0;
  fill();
  u2ps_testkit_host_run_uframes(8);
  h = find(U2PS_TESTKIT_HOST_ON_HOLE, 0);
  CHECK(h >= 0 && u2ps_testkit_trace_get((unsigned)h)->arg[1] == U2PS_MISS_NO_BUFFER);

  /* The app's stop */
  u2ps_testkit_trace_clear();
  u2ps_host_stream_stop(app.stream);
  u2ps_testkit_host_run_ms(5);
  h = find(U2PS_TESTKIT_HOST_ON_STREAM_STOP, 0);
  CHECK(h >= 0 && call_is((unsigned)h, U2PS_TESTKIT_HOST_ON_STREAM_STOP, U2PS_HOST_STOP_APP,
                          U2PS_HOST_STOP_REQUEST_NONE));
}

static void test_feedback(void)
{
  printf("feedback\n");
  start();
  /* 96 kHz nominal is 12 slots per microframe; the device asks for 12.5 */
  u2ps_testkit_host_feedback(12u << 16 | 0x8000u);
  CHECK(u2ps_host_stream_start(2, 96000) != 0u);
  u2ps_testkit_host_run_ms(100);
  const u2ps_testkit_received_t *r = u2ps_testkit_host_received();
  /* 12.5 slots of 8 bytes per packet on average, where the nominal rate
   * gives 12 */
  u2ps_host_stats_t st;
  u2ps_host_stats(&st);
  uint64_t centi_slots = r->packets > 0u ? r->bytes * 100u / (8u * r->packets) : 0u;
  printf("  feedback 12.5: %u packets, %.2f slots each, %u polls, %u rejected\n",
         (unsigned)r->packets, (double)centi_slots / 100.0, (unsigned)st.fb_polls,
         (unsigned)st.fb_rejected);
  CHECK(r->packets > 700u);
  CHECK(centi_slots >= 1245u && centi_slots <= 1250u);
  CHECK(st.fb_polls > 50u && st.fb_rejected == 0u);
  CHECK(rx.bad == 0u);
}

static void test_refused(void)
{
  printf("refused\n");
  start();
  u2ps_testkit_host_refuse_alternate_setting(3);
  u2ps_testkit_trace_clear();
  CHECK(u2ps_host_stream_start(3, 48000) != 0u);
  u2ps_testkit_host_run_ms(20);
  CHECK(u2ps_testkit_trace_match("host_on_stream_stop"));
  CHECK(call_is(0, U2PS_TESTKIT_HOST_ON_STREAM_STOP, U2PS_HOST_STOP_REFUSED,
                U2PS_HOST_STOP_REQUEST_SET_INTERFACE));
  u2ps_testkit_host_refuse_alternate_setting(-1);
  u2ps_testkit_host_refuse_sampling_frequency(true);
  u2ps_testkit_trace_clear();
  CHECK(u2ps_host_stream_start(1, 44100) != 0u);
  u2ps_testkit_host_run_ms(20);
  CHECK(call_is(0, U2PS_TESTKIT_HOST_ON_STREAM_STOP, U2PS_HOST_STOP_REFUSED,
                U2PS_HOST_STOP_REQUEST_SET_SAMPLING_FREQUENCY));
}

static void test_time(void)
{
  printf("time\n");
  start();
  uint32_t t = u2ps_testkit_now_ms();
  u2ps_testkit_set_poll(app_poll);
  u2ps_testkit_host_run_ms(30);
  CHECK(app.polls == 30u);
  CHECK(u2ps_testkit_now_ms() == t + 30u);
  u2ps_testkit_host_run_uframes(4);
  CHECK(app.polls == 30u);
  u2ps_testkit_host_run_uframes(4);
  CHECK(app.polls == 31u);
  u2ps_testkit_set_poll(NULL);
}

static void test_violations(void)
{
  printf("violations\n");
  start();
  CHECK(u2ps_host_stream_start(1, 48000) != 0u);
  u2ps_testkit_host_run_ms(5);
  CHECK(u2ps_testkit_result() == 0);
  (void)u2ps_host_slots_next(app.stream);
  CHECK(!u2ps_host_queue(app.stream, NULL));
  CHECK(u2ps_testkit_contract_count() == 1u);
  CHECK(app.contracts == 1u && app.contract == U2PS_CONTRACT_HOST_QUEUE_DATA);
  int c = find(U2PS_TESTKIT_ON_CONTRACT, 0);
  CHECK(c >= 0 && call_is((unsigned)c, U2PS_TESTKIT_ON_CONTRACT, U2PS_CONTRACT_HOST_QUEUE_DATA, 1));
  CHECK(u2ps_testkit_result() == 1);
  CHECK(u2ps_testkit_contract_take(U2PS_CONTRACT_HOST_QUEUE_DATA) == 1u);
  CHECK(u2ps_testkit_result() == 0);
  /* An injection inside a callback, the script's mistake: a port
   * violation, which fails the result until taken */
  app.plug_in_upcall = true;
  u2ps_testkit_host_run_ms(1);
  CHECK(u2ps_testkit_port_violations() == 1u);
  CHECK(u2ps_testkit_result() == 1);
  CHECK(u2ps_testkit_port_violations_take() == 1u);
  CHECK(u2ps_testkit_result() == 0);
}

static void test_disconnect(void)
{
  printf("disconnect\n");
  start();
  u2ps_testkit_trace_clear();
  u2ps_testkit_host_disconnect();
  u2ps_testkit_host_run_ms(2);
  CHECK(u2ps_testkit_trace_match("host_on_end"));
  CHECK(call_is(0, U2PS_TESTKIT_HOST_ON_END, U2PS_HOST_END_DISCONNECT, U2PS_HOST_REFUSAL_NONE));
}

/* The device enumerated at another speed, or as another device */
static void connect_as(u2ps_speed_t speed, uint16_t vid, uint16_t pid, uint16_t bcd)
{
  memset(&app, 0, sizeof app);
  u2ps_testkit_host_reset();
  u2ps_testkit_set_poll(NULL);
  u2ps_testkit_host_speed(speed);
  u2ps_testkit_host_device_ids(vid, pid, bcd);
  app_init();
  /* The re-init ended the last scenario's session */
  u2ps_testkit_trace_clear();
  u2ps_testkit_host_connect();
  u2ps_testkit_host_run_ms(1000);
}

static void test_speeds_and_test_device(void)
{
  printf("speeds and the test device\n");
  connect_as(U2PS_SPEED_FULL, 0x1234, 0x5678, 0x0100);
  CHECK(u2ps_testkit_trace_match("host_on_end"));
  CHECK(call_is(0, U2PS_TESTKIT_HOST_ON_END, U2PS_HOST_END_SPEED, U2PS_HOST_REFUSAL_NONE));
  connect_as(U2PS_SPEED_LOW, 0x1234, 0x5678, 0x0100);
  CHECK(u2ps_testkit_trace_match("host_on_end"));
  CHECK(call_is(0, U2PS_TESTKIT_HOST_ON_END, U2PS_HOST_END_SPEED, U2PS_HOST_REFUSAL_NONE));
  connect_as(U2PS_SPEED_HIGH, 0x1A0A, 0x0200, 0x0001);
  CHECK(u2ps_testkit_trace_match("host_on_test_device"));
  CHECK(call_is(0, U2PS_TESTKIT_HOST_ON_TEST_DEVICE, 0x0200, 0x0001));
  CHECK_EQ(u2ps_testkit_host_test_mode(), 0);
  connect_as(U2PS_SPEED_HIGH, 0x1A0A, 0x0104, 0x0100);
  CHECK(u2ps_testkit_trace_match("host_on_test_device host_on_end"));
  CHECK(call_is(1, U2PS_TESTKIT_HOST_ON_END, U2PS_HOST_END_TEST_MODE, U2PS_HOST_REFUSAL_NONE));
  CHECK_EQ(u2ps_testkit_host_test_mode(), 4);
}

static void test_suspend(void)
{
  printf("suspend\n");
  start();
  CHECK(!u2ps_testkit_host_suspended());
  CHECK(u2ps_host_suspend());
  CHECK(u2ps_testkit_host_suspended());
  u2ps_testkit_trace_clear();
  u2ps_testkit_host_run_ms(100);
  u2ps_testkit_host_wakeup();
  u2ps_testkit_host_run_ms(100);
  CHECK(!u2ps_testkit_host_suspended());
  CHECK(u2ps_testkit_trace_match("host_on_resumed"));
  CHECK(call_is(0, U2PS_TESTKIT_HOST_ON_RESUMED, 1, 0));
  u2ps_host_stats_t st;
  u2ps_host_stats(&st);
  CHECK_EQ(st.wakeups, 1);
  CHECK(u2ps_host_suspend());
  CHECK(u2ps_host_resume());
  u2ps_testkit_host_run_ms(100);
  CHECK(u2ps_testkit_trace_match("host_on_resumed host_on_resumed"));
  CHECK(call_is(1, U2PS_TESTKIT_HOST_ON_RESUMED, 0, 0));
}

int main(void)
{
  test_enumerate();
  test_stream();
  test_feedback();
  test_refused();
  test_time();
  test_violations();
  test_disconnect();
  test_speeds_and_test_device();
  test_suspend();
  /* Nothing left in the record: the kit's own verdict passes */
  CHECK_EQ(u2ps_testkit_result(), 0);
  return check_done();
}
