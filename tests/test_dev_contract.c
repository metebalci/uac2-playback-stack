/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_contract.c: the device stack's contract checks, against the
 * library built with U2PS_CONTRACT_CHECKS=1. Each violation of
 * u2ps_contract_t that the device stack checks is provoked, and reported
 * once with its code; the stack then goes on as without the checks. The
 * nearby uses the header allows report nothing:
 * - a thread call inside a callback, in the interrupt and in thread with
 *   the vectors masked, for each thread call; a thread call, and a release
 *   from thread, while another thread call holds the mask;
 * - u2ps_dev_connect(true) before a successful init (first, while the
 *   process has never initialised the stack);
 * - a tick more than 10 ms after the last while a close or a held
 *   SET_INTERFACE is timed, and not otherwise;
 * - a description changed after init: a field, a format, a rate, a
 *   string; once per change;
 * - releases of more than held, for a number never given, for a stopped
 *   stream inside a callback; not from thread for a stream just stopped;
 * - replies with a negative result other than STALL, with data NULL or in
 *   the control buffer;
 * - callback results the header does not list.
 */
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "dev_support.h"

#if !U2PS_CONTRACT_CHECKS
#error "test_dev_contract: link a library built with U2PS_CONTRACT_CHECKS=1"
#endif

#define C(code) contract_log.count[U2PS_CONTRACT_##code]

/* The device configured at high speed, a stream open on alternate setting
 * 1, the app holding what it receives; reports expected from here on */
static uint32_t streaming(void)
{
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  app_cfg.release_each = false;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  contract_expect(true);
  return app_log.info.stream;
}

/* ---- u2ps_dev_connect(true) before a successful u2ps_dev_init() ------------ */

static void test_connect_before_init(void)
{
  u2ps_fake_reset_all();
  contract_expect(true);
  /* VBUS before init counts, and is no violation */
  u2ps_fake_vbus_set(true);
  u2ps_dev_connect(false);
  CHECK_EQ(contract_log.total, 0);
  u2ps_dev_connect(true);
  CHECK_EQ(C(DEV_CONNECT_BEFORE_INIT), 1);
  CHECK_EQ(contract_log.total, 1);
  CHECK(!u2ps_fake_attached);
  /* An init that failed is no successful one */
  u2ps_fake_init_err = U2PS_DEV_ERR_CLOCK;
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_ERR_CLOCK);
  u2ps_dev_connect(true);
  CHECK_EQ(C(DEV_CONNECT_BEFORE_INIT), 2);
  CHECK_EQ(contract_log.total, 2);
  CHECK(!u2ps_fake_attached);
  u2ps_fake_init_err = U2PS_DEV_OK;
  /* After a successful one, nothing */
  contract_expect(true);
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  u2ps_dev_connect(true);
  CHECK(u2ps_fake_attached);
  u2ps_dev_connect(false);
  CHECK_EQ(contract_log.total, 0);
  contract_expect(false);
}

/* ---- Thread calls in the wrong context --------------------------------------- */

typedef enum {
  CALL_INIT,
  CALL_CONNECT,
  CALL_VBUS,
  CALL_REPLY,
  CALL_TICK,
  CALL_STREAM_OPEN,
  CALL_STATS,
  CALLS
} call_t;
static const char *const call_names[CALLS] = {"init", "connect", "vbus", "reply",
                                              "tick", "stream_open", "stats"};

static call_t call_kind;

static void thread_call(void)
{
  u2ps_dev_stats_t st;
  switch (call_kind) {
  case CALL_INIT:
    (void)u2ps_dev_init(&test_desc);
    break;
  case CALL_CONNECT:
    u2ps_dev_connect(true);
    break;
  case CALL_VBUS:
    u2ps_dev_vbus(true);
    break;
  case CALL_REPLY:
    (void)u2ps_dev_reply(0, NULL, 0);
    break;
  case CALL_TICK:
    (void)u2ps_dev_tick(0);
    break;
  case CALL_STREAM_OPEN:
    (void)u2ps_dev_stream_open(0, NULL);
    break;
  case CALL_STATS:
    u2ps_dev_stats(&st);
    break;
  default:
    break;
  }
}

static void call_in_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  (void)data;
  (void)len;
  (void)uframe;
  thread_call();
}

/* Each thread call inside on_packet, in the port's interrupt */
static void test_call_in_interrupt(void)
{
  for (call_kind = CALL_INIT; call_kind < CALLS; call_kind++) {
    streaming();
    app_packet_hook = call_in_packet;
    u2ps_fake_packet(64, 100);
    if (C(DEV_CALL_IN_CALLBACK) != 1u || contract_log.total != 1u)
      printf("  in the interrupt: %s\n", call_names[call_kind]);
    CHECK_EQ(C(DEV_CALL_IN_CALLBACK), 1);
    CHECK_EQ(contract_log.total, 1);
    app_packet_hook = NULL;
  }
  contract_expect(false);
}

static char call_at; /* the event letter whose callback makes the call */

static void call_at_event(char e)
{
  if (e == call_at) {
    call_at = 0;
    thread_call();
  }
}

/* Each thread call inside a callback that runs in thread with the vectors
 * masked: on_stream_stop under u2ps_dev_connect(false), on_vbus(false)
 * under u2ps_dev_vbus(false) */
static void test_call_in_masked_callback(void)
{
  for (call_kind = CALL_INIT; call_kind < CALLS; call_kind++) {
    streaming();
    app_event_hook = call_at_event;
    call_at = 'X';
    u2ps_dev_connect(false);
    if (C(DEV_CALL_IN_CALLBACK) != 1u || contract_log.total != 1u)
      printf("  in on_stream_stop: %s\n", call_names[call_kind]);
    CHECK_EQ(C(DEV_CALL_IN_CALLBACK), 1);
    CHECK_EQ(contract_log.total, 1);

    streaming();
    app_event_hook = call_at_event;
    call_at = 'v';
    u2ps_fake_vbus_set(false);
    if (C(DEV_CALL_IN_CALLBACK) != 1u || contract_log.total != 1u)
      printf("  in on_vbus: %s\n", call_names[call_kind]);
    CHECK_EQ(C(DEV_CALL_IN_CALLBACK), 1);
    CHECK_EQ(contract_log.total, 1);
    app_event_hook = NULL;
    u2ps_fake_vbus_set(true);
  }
  contract_expect(false);
}

/* Each thread call while another thread call holds the mask: as an
 * interrupt of another source, or a second thread, would make it */
static void test_call_nested(void)
{
  for (call_kind = CALL_INIT; call_kind < CALLS; call_kind++) {
    streaming();
    u2ps_fake_on_mask = thread_call;
    u2ps_dev_stats(&(u2ps_dev_stats_t){0});
    if (C(DEV_CALL_NESTED) != 1u || contract_log.total != 1u)
      printf("  nested: %s\n", call_names[call_kind]);
    CHECK_EQ(C(DEV_CALL_NESTED), 1);
    CHECK_EQ(contract_log.total, 1);
    /* The stack went on as without the check: the port saw the mask nest */
    if (call_kind == CALL_STATS)
      CHECK(u2ps_fake_violations > 0u);
  }
  contract_expect(false);
}

static uint32_t release_stream;
static uint16_t release_n;

static void release_now(void)
{
  u2ps_dev_release(release_stream, release_n);
}

static void release_at_stop(char e)
{
  if (e == 'X')
    u2ps_dev_release(release_stream, 0);
}

/* u2ps_dev_release() from thread masks too: nested inside another thread
 * call's mask it is a violation; inside a callback it masks nothing */
static void test_release_nested(void)
{
  release_stream = streaming();
  release_n = 0;
  u2ps_fake_on_mask = release_now;
  (void)u2ps_dev_tick(0);
  CHECK_EQ(C(DEV_CALL_NESTED), 1);
  CHECK_EQ(contract_log.total, 1);

  /* Inside on_stream_stop under u2ps_dev_connect(false): no mask, so not
   * nested; the stream has stopped, though (test_release_at_stop) */
  release_stream = streaming();
  app_event_hook = release_at_stop;
  u2ps_dev_connect(false);
  CHECK_EQ(C(DEV_CALL_NESTED), 0);
  CHECK_EQ(C(DEV_RELEASE_STREAM), 1);
  CHECK_EQ(u2ps_fake_violations, 0);
  contract_expect(false);
}

/* ---- Tick period ---------------------------------------------------------------- */

static void test_tick_late(void)
{
  /* A close: timed from its first tick; late ticks before that, and after
   * its fault, are not reported */
  streaming();
  CHECK_EQ(u2ps_dev_tick(100), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_dev_tick(5000), U2PS_FAULT_NONE); /* nothing timed */
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_CLOSING);
  CHECK_EQ(u2ps_dev_tick(6000), U2PS_FAULT_NONE); /* the close's first tick */
  CHECK_EQ(u2ps_dev_tick(6010), U2PS_FAULT_NONE);
  CHECK_EQ(contract_log.total, 0);
  CHECK_EQ(u2ps_dev_tick(6021), U2PS_FAULT_CLOSE);
  CHECK_EQ(C(DEV_TICK_LATE), 1);
  CHECK_EQ(contract_log.total, 1);
  CHECK_EQ(u2ps_dev_tick(7000), U2PS_FAULT_NONE);
  CHECK_EQ(contract_log.total, 1);
  u2ps_fake_closed();

  /* A held SET_INTERFACE */
  contract_expect(true);
  app_cfg.defer = true;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_tick(8000), U2PS_FAULT_NONE); /* its first tick */
  CHECK_EQ(u2ps_dev_tick(8010), U2PS_FAULT_NONE);
  CHECK_EQ(contract_log.total, 0);
  CHECK_EQ(u2ps_dev_tick(8025), U2PS_FAULT_NONE);
  CHECK_EQ(C(DEV_TICK_LATE), 1);
  CHECK_EQ(contract_log.total, 1);
  CHECK_EQ(u2ps_dev_tick(8031), U2PS_FAULT_START);
  CHECK_EQ(contract_log.total, 1);
  CHECK_EQ(app_open_deferred(), U2PS_OPEN_DONE);
  contract_expect(false);
}

/* ---- The description ------------------------------------------------------------- */

static u2ps_format_t formats[4];
static uint32_t rates[TEST_RATE_COUNT];
static char manufacturer[16], product[64], serial_number[128];

static void tick_reports(uint32_t now, unsigned expected)
{
  (void)u2ps_dev_tick(now);
  CHECK_EQ(C(DEV_DESCRIPTION_CHANGED), expected);
  CHECK_EQ(contract_log.total, expected);
}

static void test_description_changed(void)
{
  u2ps_dev_description_t saved = test_desc;
  memcpy(formats, test_desc.formats, sizeof formats);
  memcpy(rates, test_desc.sampling_frequencies, sizeof rates);
  strcpy(manufacturer, test_desc.manufacturer);
  strcpy(product, test_desc.product);
  strcpy(serial_number, test_desc.serial_number);
  test_desc.formats = formats;
  test_desc.sampling_frequencies = rates;
  test_desc.manufacturer = manufacturer;
  test_desc.product = product;
  test_desc.serial_number = serial_number;
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  contract_expect(true);
  uint32_t t = 0;
  tick_reports(t += 5, 0);

  /* A field, once per change: changed, then changed back */
  test_desc.id_vendor ^= 1u;
  tick_reports(t += 5, 1);
  tick_reports(t += 5, 1);
  test_desc.id_vendor ^= 1u;
  tick_reports(t += 5, 2);
  test_desc.self_powered = !test_desc.self_powered;
  tick_reports(t += 5, 3);
  test_desc.self_powered = !test_desc.self_powered;
  tick_reports(t += 5, 4);
  /* What it points to: a format, a rate, each string */
  formats[3].bit_resolution--;
  tick_reports(t += 5, 5);
  formats[3].bit_resolution++;
  tick_reports(t += 5, 6);
  formats[2].audio_data_format = U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT;
  tick_reports(t += 5, 7);
  formats[2].audio_data_format = U2PS_AUDIO_DATA_FORMAT_PCM;
  tick_reports(t += 5, 8);
  rates[TEST_RATE_COUNT - 1u]++;
  tick_reports(t += 5, 9);
  rates[TEST_RATE_COUNT - 1u]--;
  tick_reports(t += 5, 10);
  char *strings[3] = {manufacturer, product, serial_number};
  unsigned n = 10;
  for (unsigned i = 0; i < 3u; i++) {
    size_t last = strlen(strings[i]) - 1u;
    strings[i][last]++;
    tick_reports(t += 5, ++n);
    strings[i][last]--;
    tick_reports(t += 5, ++n);
  }
  /* The counts and the initial rate are fields too */
  test_desc.format_count--;
  tick_reports(t += 5, ++n);
  test_desc.format_count++;
  tick_reports(t += 5, ++n);
  test_desc.initial_sampling_frequency = rates[2];
  tick_reports(t += 5, ++n);

  /* A re-init takes the description as it is now */
  contract_expect(true);
  u2ps_dev_connect(false);
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  tick_reports(t += 5, 0);
  test_desc = saved;
  contract_expect(false);
}

/* ---- Releases ---------------------------------------------------------------------- */

static void release_in_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  (void)data;
  (void)len;
  (void)uframe;
  u2ps_dev_release(release_stream, release_n);
}

static unsigned release_errors(void)
{
  u2ps_dev_stats_t st;
  u2ps_dev_stats(&st);
  return st.release_errors;
}

static void test_release(void)
{
  uint32_t s = streaming();
  u2ps_fake_packet(64, 100);
  u2ps_fake_packet(64, 101);
  /* More than held, from thread: ignored, counted, reported */
  u2ps_dev_release(s, 3);
  CHECK_EQ(C(DEV_RELEASE_COUNT), 1);
  CHECK_EQ(contract_log.total, 1);
  CHECK_EQ(release_errors(), 1);
  CHECK_EQ(u2ps_fake_held, 2);
  u2ps_dev_release(s, 2);
  CHECK_EQ(u2ps_fake_held, 0);
  CHECK_EQ(contract_log.total, 1);
  /* Inside on_packet */
  release_stream = s;
  release_n = 2;
  app_packet_hook = release_in_packet;
  u2ps_fake_packet(64, 102);
  CHECK_EQ(C(DEV_RELEASE_COUNT), 2);
  release_n = 1;
  u2ps_fake_packet(64, 103);
  CHECK_EQ(C(DEV_RELEASE_COUNT), 2);
  app_packet_hook = NULL;
  CHECK_EQ(contract_log.total, 2);

  /* A number never given: 0, or after the last start's */
  contract_expect(true);
  u2ps_dev_release(0, 1);
  CHECK_EQ(C(DEV_RELEASE_STREAM), 1);
  u2ps_dev_release(s + 1u, 0);
  CHECK_EQ(C(DEV_RELEASE_STREAM), 2);
  CHECK_EQ(contract_log.total, 2);
  CHECK_EQ(release_errors(), 4);

  /* A stream that has stopped, from thread: the app cannot tell a stop
   * that the interrupt has just made, so nothing is reported */
  contract_expect(true);
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  u2ps_fake_closed();
  u2ps_dev_release(s, 1);
  CHECK_EQ(contract_log.total, 0);
  CHECK_EQ(release_errors(), 5);

  /* ... but inside a callback the app knows: here a packet of the next
   * stream */
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK(app_log.info.stream != s);
  release_stream = s;
  release_n = 0;
  app_packet_hook = release_in_packet;
  u2ps_fake_packet(64, 200);
  CHECK_EQ(C(DEV_RELEASE_STREAM), 1);
  CHECK_EQ(contract_log.total, 1);
  CHECK_EQ(release_errors(), 6);
  app_packet_hook = NULL;
  contract_expect(false);
}

/* Inside on_stream_stop, in the interrupt (on_closed) and in thread
 * (u2ps_dev_connect(false)) */
static void test_release_at_stop(void)
{
  release_stream = streaming();
  app_event_hook = release_at_stop;
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  u2ps_fake_closed();
  CHECK_EQ(C(DEV_RELEASE_STREAM), 1);
  CHECK_EQ(contract_log.total, 1);

  release_stream = streaming();
  app_event_hook = release_at_stop;
  u2ps_dev_connect(false);
  CHECK_EQ(C(DEV_RELEASE_STREAM), 1);
  CHECK_EQ(contract_log.total, 1);
  contract_expect(false);
}

/* ---- Replies ----------------------------------------------------------------------- */

static int defer(const u2ps_request_t *req)
{
  (void)req;
  return U2PS_REQ_DEFER;
}

static void test_reply(void)
{
  uint8_t buf[64];
  uint16_t n;
  static const uint8_t own[4] = {1, 2, 3, 4};

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  app_handler = defer;
  contract_expect(true);

  /* A result below 0 other than STALL: a stall, reported */
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, U2PS_REQ_DEFER), true);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(C(DEV_REPLY_RESULT), 1);
  CHECK_EQ(contract_log.total, 1);
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0, 0, 0, NULL, NULL), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, -7), true);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(C(DEV_REPLY_RESULT), 2);
  /* Also for a request no longer pending */
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, -7), false);
  CHECK_EQ(C(DEV_REPLY_RESULT), 3);
  CHECK_EQ(contract_log.total, 3);
  /* STALL itself is fine */
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0, 0, 0, NULL, NULL), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, U2PS_REQ_STALL), true);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(contract_log.total, 3);

  /* Data to send but none given: a stall, reported */
  contract_expect(true);
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, 4), true);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(C(DEV_REPLY_DATA), 1);
  CHECK_EQ(contract_log.total, 1);
  /* The request's buf after its callback: sent, as without the check,
   * and reported, at its start and inside it */
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, app_log.last.buf, 4), true);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 4);
  CHECK_EQ(C(DEV_REPLY_DATA), 2);
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, app_log.last.buf + app_log.last.size - 1u, 1),
           true);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(C(DEV_REPLY_DATA), 3);
  CHECK_EQ(contract_log.total, 3);
  /* The app's own memory, a zero-length answer with NULL, and NULL for a
   * request without data: fine */
  contract_expect(true);
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, own, 4), true);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 4);
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, 0), true);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 0);
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0, 0, 0, NULL, NULL), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, 4), true);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(contract_log.total, 0);
  contract_expect(false);
}

/* ---- Callback results -------------------------------------------------------------- */

static int request_result;

static int answer(const u2ps_request_t *req)
{
  (void)req;
  return request_result;
}

static void test_callback_result(void)
{
  uint8_t buf[64];
  uint16_t n;

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  app_handler = answer;
  contract_expect(true);
  /* on_request: below 0, other than STALL and DEFER */
  request_result = -5;
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(C(DEV_CALLBACK_RESULT), 1);
  CHECK_EQ(contract_log.total, 1);
  request_result = U2PS_REQ_STALL;
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_STALL);
  request_result = 4;
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_OK);
  request_result = U2PS_REQ_DEFER;
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0, 0, 0, NULL, NULL), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, 0), true);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(contract_log.total, 1);
  /* Host-to-device with data: the callback runs after the data stage */
  request_result = -9;
  uint8_t out[4] = {0};
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0, 0, 4, out, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(C(DEV_CALLBACK_RESULT), 2);
  CHECK_EQ(contract_log.total, 2);

  /* on_sampling_frequency */
  contract_expect(true);
  app_cfg.rate_result = -9;
  CHECK_EQ(set_rate(48000), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(C(DEV_CALLBACK_RESULT), 1);
  app_cfg.rate_result = U2PS_REQ_STALL;
  CHECK_EQ(set_rate(48000), U2PS_FAKE_HOST_STALL);
  app_cfg.rate_result = 0;
  CHECK_EQ(set_rate(48000), U2PS_FAKE_HOST_OK);
  CHECK_EQ(contract_log.total, 1);

  /* on_stream_start: outside u2ps_stream_start_t, taken as REFUSE */
  contract_expect(true);
  app_cfg.start_result = 7;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(C(DEV_CALLBACK_RESULT), 1);
  CHECK_EQ(contract_log.total, 1);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), 0);
  app_cfg.start_result = (int)U2PS_STREAM_DEFER;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(app_open_deferred(), U2PS_OPEN_DONE);
  CHECK_EQ(contract_log.total, 1);
  contract_expect(false);
}

int main(void)
{
  if (!test_desc_load())
    return 1;
  test_connect_before_init();
  test_call_in_interrupt();
  test_call_in_masked_callback();
  test_call_nested();
  test_release_nested();
  test_tick_late();
  test_description_changed();
  test_release();
  test_release_at_stop();
  test_reply();
  test_callback_result();
  return check_done();
}
