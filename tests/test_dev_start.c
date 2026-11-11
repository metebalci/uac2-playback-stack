/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_start.c: the stream start (u2ps_dev.h, "Stream start").
 * SET_INTERFACE's status stage is held until the stream is open: after the
 * port's open in the same interrupt, at on_closed after a pending close, or
 * at u2ps_dev_stream_open() after a deferred start (USB 2.0 9.2.6); for at
 * most U2PS_START_MAX, after which it completes, the start stays pending
 * and tick returns U2PS_FAULT_START once. A refused start stalls
 * SET_INTERFACE and leaves alternate setting 0 (UAC2 5.2.6.1.2). A new
 * SETUP ends the hold but not the start. Every start not refused gets one
 * on_stream_stop, also when cancelled while pending.
 */
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "dev_support.h"

static const char *events(void)
{
  app_log.events[app_log.nevents] = '\0';
  return app_log.events;
}

#define CHECK_EVENTS(want)                                         \
  do {                                                             \
    if (strcmp(events(), want) != 0)                               \
      printf("  events \"%s\", want \"%s\"\n", events(), want);    \
    CHECK(strcmp(events(), want) == 0);                            \
  } while (0)

static void clear(void)
{
  app_log.nevents = 0;
  u2ps_fake_clear_log();
}

/* GET_INTERFACE of the AudioStreaming interface, or -1 */
static int get_alt(void)
{
  uint8_t b = 0xFF;
  uint16_t n = 0;
  if (u2ps_fake_host_control(0x81, U2PS_GET_INTERFACE, 0, 1, 1, &b, &n) != U2PS_FAKE_HOST_OK || n != 1u)
    return -1;
  return b;
}

static void start_configured(void)
{
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  clear();
}

/* The SETUP of SET_INTERFACE of the AudioStreaming interface, without the
 * host's status stage */
static void setup_set_interface(uint8_t alt)
{
  u2ps_fake_setup(0x01, U2PS_SET_INTERFACE, alt, 1, 0);
}

/* OPEN: the status is armed in the same interrupt, after the port's open */
static void test_immediate(void)
{
  start_configured();
  setup_set_interface(1);
  int o = u2ps_fake_find(U2PS_FAKE_STREAM_OPEN, 0), st = u2ps_fake_find(U2PS_FAKE_EP0_STATUS, 0);
  CHECK(o >= 0);
  CHECK(st > o);
  if (st >= 0)
    CHECK_EQ(u2ps_fake_calls[st].from, U2PS_FAKE_FROM_SETUP);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_alt(), 1);
  CHECK_EVENTS("S");
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* With the last stream's close pending, the status waits for on_closed and
 * the open there */
static void test_after_close(void)
{
  start_configured();
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear();
  CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_STATUS), 0);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_CLOSING);
  u2ps_fake_closed();
  CHECK_EVENTS("XS");
  int o = u2ps_fake_find(U2PS_FAKE_STREAM_OPEN, 0), st = u2ps_fake_find(U2PS_FAKE_EP0_STATUS, 0);
  CHECK(o >= 0);
  CHECK(st > o);
  if (st >= 0)
    CHECK_EQ(u2ps_fake_calls[st].from, U2PS_FAKE_FROM_STREAM);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_alt(), 2);
  /* Alternate setting 0 is answered at once, whatever the close does */
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_CLOSING);
  CHECK_EQ(get_alt(), 0);
  u2ps_fake_closed();
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* DEFER: the status waits for u2ps_dev_stream_open(), which opens the
 * stream and arms it with the vectors masked */
static void test_deferred(void)
{
  start_configured();
  app_cfg.defer = true;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), 0);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_STATUS), 0);
  CHECK_EQ(u2ps_dev_stream_open(app_log.info.stream + 1u, &app_log.cfg), U2PS_OPEN_NOT_PENDING);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), 0);
  u2ps_fake_clear_log();
  CHECK_EQ(app_open_deferred(), U2PS_OPEN_DONE);
  int o = u2ps_fake_find(U2PS_FAKE_STREAM_OPEN, 0), st = u2ps_fake_find(U2PS_FAKE_EP0_STATUS, 0);
  CHECK(o >= 0);
  CHECK(st > o);
  if (o >= 0 && st >= 0) {
    CHECK_EQ(u2ps_fake_calls[o].from, U2PS_FAKE_FROM_MASKED);
    CHECK_EQ(u2ps_fake_calls[st].from, U2PS_FAKE_FROM_MASKED);
  }
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_OPEN);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_alt(), 1);
  /* Open already: nothing pending */
  CHECK_EQ(app_open_deferred(), U2PS_OPEN_NOT_PENDING);
  u2ps_fake_packet(28, 0x0100);
  CHECK_EVENTS("SP");
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Past U2PS_START_MAX from the first tick after the SETUP the status
 * completes, tick returns U2PS_FAULT_START once, and the start stays
 * pending: it still opens, with no second status */
static void test_bound(void)
{
  start_configured();
  /* The bounds are passed in jumps longer than the 10 ms tick period, each
   * reported (U2PS_CONTRACT_DEV_TICK_LATE) */
  contract_expect(true);
  app_cfg.defer = true;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_tick(1000), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_dev_tick(1000 + U2PS_START_MAX), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_tick(1001 + U2PS_START_MAX), U2PS_FAULT_START);
  CHECK_EQ(u2ps_dev_tick(1002 + U2PS_START_MAX), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_alt(), 1);
  u2ps_fake_clear_log();
  CHECK_EQ(app_open_deferred(), U2PS_OPEN_DONE);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_STATUS), 0);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_NAK);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_OPEN);

  /* A close and a start bound both passed for one SETUP: each fault once,
   * on separate calls */
  app_cfg.defer = false;
  CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_tick(5000), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_dev_tick(5100), U2PS_FAULT_CLOSE);
  CHECK_EQ(u2ps_dev_tick(5100), U2PS_FAULT_START);
  CHECK_EQ(u2ps_dev_tick(5100), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  u2ps_fake_clear_log();
  u2ps_fake_closed();
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_OPEN);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_STATUS), 0);
  CHECK_EQ(get_alt(), 2);

  /* A close, then a deferred start: the bound still counts from the first
   * tick after the SETUP, not from the start at on_closed */
  app_cfg.defer = true;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_tick(7000), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_dev_tick(7009), U2PS_FAULT_NONE);
  u2ps_fake_closed();
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_IDLE);
  CHECK_EQ(u2ps_dev_tick(7031), U2PS_FAULT_START);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_open_deferred(), U2PS_OPEN_DONE);
  app_cfg.defer = false;
  CHECK_EQ(u2ps_fake_violations, 0);
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_DEV_TICK_LATE], 3);
  CHECK_EQ(contract_log.total, 3);
  contract_expect(false);
}

/* A refused start stalls SET_INTERFACE and the interface keeps alternate
 * setting 0: the app's REFUSE, a NULL cfg, and the port refusing the
 * memory, at once or deferred (then on_fault and on_stream_stop come from
 * thread with the vectors masked) */
static void test_refused(void)
{
  start_configured();
  app_cfg.decline = true;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_alt(), 0);
  CHECK_EVENTS("S");
  app_cfg.decline = false;

  app_cfg.defer = true;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_stream_open(app_log.info.stream, NULL), U2PS_OPEN_REFUSED);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_alt(), 0);
  CHECK_EQ(u2ps_dev_stream_open(app_log.info.stream, &app_log.cfg), U2PS_OPEN_NOT_PENDING);
  CHECK_EVENTS("SS");

  u2ps_fake_open_result = false;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(app_open_deferred(), U2PS_OPEN_FAULT);
  CHECK(app_log.fault_masked);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_alt(), 0);
  CHECK_EVENTS("SSSFX");
  app_cfg.defer = false;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_alt(), 0);
  CHECK_EVENTS("SSSFXSFX");
  CHECK_EQ(app_log.faults[U2PS_FAULT_OPEN], 2);
  u2ps_fake_open_result = true;

  /* From streaming alternate setting 1 to a refused 2: the old stream
   * closes, then the stall */
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_alt(), 1);
  clear();
  app_cfg.decline = true;
  CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_NAK);
  u2ps_fake_closed();
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_alt(), 0);
  CHECK_EVENTS("XS");
  app_cfg.decline = false;
  CHECK_EQ(u2ps_fake_violations, 0);
}

typedef enum {
  CANCEL_RESET,
  CANCEL_DETACH,
  CANCEL_VBUS,
  CANCEL_REINIT,
  CANCEL_SET_CONFIGURATION,
  CANCEL_ALT_0,
  CANCEL_ALT_2,
  CANCELS
} cancel_t;

/* Each cause cancels a pending start with one on_stream_stop; the open
 * then finds nothing pending */
static void test_cancel(void)
{
  static const char *const want[CANCELS] = {"SXDB", "SXD", "SXDv", "SXD", "SX", "SX", "SXS"};
  for (unsigned c = 0; c < CANCELS; c++) {
    start_configured();
    app_cfg.defer = true;
    CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_NAK);
    uint32_t stream = app_log.info.stream;
    switch ((cancel_t)c) {
    case CANCEL_RESET:
      u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
      break;
    case CANCEL_DETACH:
      u2ps_dev_connect(false);
      break;
    case CANCEL_VBUS:
      u2ps_fake_vbus_set(false);
      break;
    case CANCEL_REINIT:
      CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
      break;
    case CANCEL_SET_CONFIGURATION:
      CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
      break;
    case CANCEL_ALT_0:
      CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
      break;
    case CANCEL_ALT_2:
      CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_NAK);
      break;
    default:
      break;
    }
    CHECK_EVENTS(want[c]);
    if (strcmp(events(), want[c]) != 0)
      printf("  cancel %u\n", c);
    u2ps_stream_cfg_t cfg = app_log.cfg;
    CHECK_EQ(u2ps_dev_stream_open(stream, &cfg), U2PS_OPEN_NOT_PENDING);
    CHECK_EQ(app_log.stops, 1);
    CHECK(u2ps_fake_stream_state != U2PS_FAKE_ST_OPEN);
    if (c == CANCEL_VBUS)
      u2ps_fake_vbus_set(true);
    if (c == CANCEL_ALT_2) {
      /* The new start is pending in its place */
      CHECK_EQ(app_open_deferred(), U2PS_OPEN_DONE);
      CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
      CHECK_EQ(get_alt(), 2);
    }
    app_cfg.defer = false;
    CHECK_EQ(u2ps_fake_violations, 0);
  }
}

/* A new SETUP while the status is held ends the hold: no status of the old
 * request is armed later, but the start still opens */
static void test_new_setup(void)
{
  uint8_t buf[2];
  uint16_t n;

  start_configured();
  app_cfg.defer = true;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_GET_STATUS, 0, 0, 2, buf, &n), U2PS_FAKE_HOST_OK);
  u2ps_fake_clear_log();
  CHECK_EQ(app_open_deferred(), U2PS_OPEN_DONE);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_STATUS), 0);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_NAK);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_OPEN);
  u2ps_fake_packet(28, 0x0100);
  CHECK_EQ(app_log.packets, 1);
  app_cfg.defer = false;

  /* The same with a close pending: the start comes at on_closed */
  CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_GET_STATUS, 0, 0, 2, buf, &n), U2PS_FAKE_HOST_OK);
  u2ps_fake_clear_log();
  u2ps_fake_closed();
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), 1);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_STATUS), 0);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_NAK);
  CHECK_EQ(app_log.info.alternate_setting, 2);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* The host changes the sampling frequency while a start is pending (past
 * the bound): the stream opens at the new one's nominal feedback */
static void test_frequency_change(void)
{
  start_configured();
  CHECK_EQ(set_rate(48000), U2PS_FAKE_HOST_OK);
  app_cfg.defer = true;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(app_log.fb_offered, 0x00060000); /* 6 samples per microframe */
  contract_expect(true);
  CHECK_EQ(u2ps_dev_tick(0), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_dev_tick(1000), U2PS_FAULT_START); /* a late tick */
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_DEV_TICK_LATE], 1);
  CHECK_EQ(contract_log.total, 1);
  contract_expect(false);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_rate(96000), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_open_deferred(), U2PS_OPEN_DONE);
  CHECK_EQ(u2ps_fake_stream.fb_value, 0x000C0000);
  CHECK_EQ(u2ps_fake_fb, 0x000C0000);
  app_cfg.defer = false;

  /* Unchanged: the app's own first value stands */
  start_configured();
  app_cfg.defer = true;
  app_cfg.fb_value = 0x00060010;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(app_open_deferred(), U2PS_OPEN_DONE);
  CHECK_EQ(u2ps_fake_stream.fb_value, 0x00060010);
  app_cfg.defer = false;
  app_cfg.fb_value = 0;
  CHECK_EQ(u2ps_fake_violations, 0);
}

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
  test_immediate();
  test_after_close();
  test_deferred();
  test_bound();
  test_refused();
  test_cancel();
  test_new_setup();
  test_frequency_change();
  return check_done();
}
