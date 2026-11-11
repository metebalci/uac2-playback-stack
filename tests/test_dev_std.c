/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_std.c: the standard requests GET_STATUS (USB 2.0 9.4.5),
 * SET_FEATURE and CLEAR_FEATURE (9.4.9, 9.4.1) including TEST_MODE (Table
 * 9-7, 7.1.20), GET/SET_CONFIGURATION (9.4.2, 9.4.7) and GET/SET_INTERFACE
 * (9.4.4, 9.4.10), in the Default, Address and Configured states.
 */
#include <string.h>

#include "check.h"
#include "dev_support.h"

static u2ps_fake_host_result_t get_status(uint8_t rcpt, uint16_t windex, uint16_t *status)
{
  uint8_t buf[2] = {0xFF, 0xFF};
  uint16_t n = 0;
  u2ps_fake_host_result_t r =
    u2ps_fake_host_control((uint8_t)(0x80u | rcpt), U2PS_GET_STATUS, 0, windex, 2, buf, &n);
  *status = r == U2PS_FAKE_HOST_OK && n == 2 ? (uint16_t)(buf[0] | buf[1] << 8) : 0xFFFFu;
  return r;
}

static u2ps_fake_host_result_t feature(bool set, uint8_t rcpt, uint16_t selector, uint16_t windex)
{
  return u2ps_fake_host_control(rcpt, set ? U2PS_SET_FEATURE : U2PS_CLEAR_FEATURE, selector, windex, 0,
                      NULL, NULL);
}

/* Start with the description bus-powered or self-powered */
static void start_powered(bool self)
{
  test_desc.self_powered = self;
  dev_start(U2PS_SPEED_HIGH);
}

static void test_get_status(void)
{
  uint16_t st;

  /* Device: bus-powered, remote wakeup disabled */
  start_powered(false);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_DEVICE, 0, &st), U2PS_FAKE_HOST_OK);
  CHECK_EQ(st, 0x0000);
  start_powered(true);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_DEVICE, 0, &st), U2PS_FAKE_HOST_OK);
  CHECK_EQ(st, U2PS_STATUS_SELF_POWERED);
  start_powered(false);

  /* Address state: EP0 only; interfaces and other endpoints are errors */
  CHECK_EQ(set_address(5), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x00, &st), U2PS_FAKE_HOST_OK);
  CHECK_EQ(st, 0);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x80, &st), U2PS_FAKE_HOST_OK);
  CHECK_EQ(st, 0);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x01, &st), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_ITF, 0, &st), U2PS_FAKE_HOST_STALL);

  /* Configured: the configuration's interfaces and endpoints */
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_ITF, 0, &st), U2PS_FAKE_HOST_OK);
  CHECK_EQ(st, 0);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_ITF, 1, &st), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_ITF, 2, &st), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_ITF, 0x0100, &st), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, 1, 1, 0, NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x01, &st), U2PS_FAKE_HOST_OK);
  CHECK_EQ(st, 0);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x81, &st), U2PS_FAKE_HOST_OK);
  CHECK_EQ(st, 0);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x02, &st), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x82, &st), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x0101, &st), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_status(3, 0, &st), U2PS_FAKE_HOST_STALL); /* recipient Other */
  CHECK_EQ(get_status(U2PS_REQ_RCPT_DEVICE, 0, &st), U2PS_FAKE_HOST_OK);
  CHECK_EQ(st, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static void test_features(void)
{
  uint16_t st;

  /* Remote wakeup is not supported: no configuration may declare it
   * (test_dev_speed checks init), and the feature is a request error */
  start_powered(false);
  dev_configure();
  CHECK_EQ(feature(true, U2PS_REQ_RCPT_DEVICE, U2PS_FEATURE_REMOTE_WAKEUP, 0), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(feature(false, U2PS_REQ_RCPT_DEVICE, U2PS_FEATURE_REMOTE_WAKEUP, 0), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_DEVICE, 0, &st), U2PS_FAKE_HOST_OK);
  CHECK_EQ(st, 0);
  dev_start(U2PS_SPEED_HIGH);

  /* ENDPOINT_HALT: cannot be set; clearing an existing endpoint succeeds */
  CHECK_EQ(feature(true, U2PS_REQ_RCPT_EP, U2PS_FEATURE_ENDPOINT_HALT, 0x00), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(feature(false, U2PS_REQ_RCPT_EP, U2PS_FEATURE_ENDPOINT_HALT, 0x00), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_address(5), U2PS_FAKE_HOST_OK);
  CHECK_EQ(feature(false, U2PS_REQ_RCPT_EP, U2PS_FEATURE_ENDPOINT_HALT, 0x01), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, 1, 1, 0, NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(feature(true, U2PS_REQ_RCPT_EP, U2PS_FEATURE_ENDPOINT_HALT, 0x01), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(feature(false, U2PS_REQ_RCPT_EP, U2PS_FEATURE_ENDPOINT_HALT, 0x01), U2PS_FAKE_HOST_OK);
  CHECK_EQ(feature(false, U2PS_REQ_RCPT_EP, U2PS_FEATURE_ENDPOINT_HALT, 0x81), U2PS_FAKE_HOST_OK);
  CHECK_EQ(feature(false, U2PS_REQ_RCPT_EP, U2PS_FEATURE_ENDPOINT_HALT, 0x02), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x01, &st), U2PS_FAKE_HOST_OK);
  CHECK_EQ(st, 0);

  /* Selectors that do not belong to the recipient, and unknown ones */
  CHECK_EQ(feature(true, U2PS_REQ_RCPT_ITF, 0, 0), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(feature(false, U2PS_REQ_RCPT_ITF, 0, 0), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(feature(true, U2PS_REQ_RCPT_EP, U2PS_FEATURE_REMOTE_WAKEUP, 0x01), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(feature(true, U2PS_REQ_RCPT_DEVICE, U2PS_FEATURE_ENDPOINT_HALT, 0), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(feature(true, U2PS_REQ_RCPT_DEVICE, 7, 0), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(feature(true, 3, 0, 0), U2PS_FAKE_HOST_STALL);
  /* Test mode cannot be cleared (9.4.1) */
  CHECK_EQ(feature(false, U2PS_REQ_RCPT_DEVICE, U2PS_FEATURE_TEST_MODE, 0x0100), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_TEST_MODE), 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* One accepted SET_FEATURE(TEST_MODE): the status stage first, then
 * test_mode in its completion */
static void check_test_mode(uint8_t selector)
{
  u2ps_fake_clear_log();
  u2ps_fake_setup(0x00, U2PS_SET_FEATURE, U2PS_FEATURE_TEST_MODE, (uint16_t)(selector << 8), 0);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_TEST_MODE), 0); /* not before the status stage */
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_STATUS_IN);
  CHECK_EQ(u2ps_fake_test_selector, -1);
  u2ps_fake_ep0_status_done();
  int s = u2ps_fake_find(U2PS_FAKE_EP0_STATUS, 0);
  int t = u2ps_fake_find(U2PS_FAKE_TEST_MODE, 0);
  CHECK(s >= 0);
  CHECK(t > s);
  if (t >= 0) {
    CHECK_EQ(u2ps_fake_calls[t].from, U2PS_FAKE_FROM_STATUS);
    CHECK_EQ(u2ps_fake_calls[t].arg, selector);
  }
  CHECK_EQ(u2ps_fake_test_selector, selector);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_TEST_MODE), 1);
}

static void test_test_mode(void)
{
  /* Selectors 1..4, in the Default, Address and Configured states */
  dev_start(U2PS_SPEED_HIGH);
  check_test_mode(U2PS_TEST_J);
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(set_address(5), U2PS_FAKE_HOST_OK);
  check_test_mode(U2PS_TEST_K);
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  check_test_mode(U2PS_TEST_SE0_NAK);
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  check_test_mode(U2PS_TEST_PACKET);
  CHECK_EQ(u2ps_fake_violations, 0);

  /* Everything else is a request error, and test_mode is never called:
   * reserved, hub-only and vendor selectors, a non-zero low byte of wIndex,
   * an interface or endpoint recipient, a data stage */
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  u2ps_fake_clear_log();
  static const uint16_t bad_windex[] = {0x0000, 0x0500, 0x0600, 0x3F00, 0x4000, 0xBF00,
                                        0xC000, 0xFF00, 0x0101, 0x0180, 0x01FF};
  for (unsigned i = 0; i < sizeof bad_windex / sizeof bad_windex[0]; i++)
    CHECK_EQ(feature(true, U2PS_REQ_RCPT_DEVICE, U2PS_FEATURE_TEST_MODE, bad_windex[i]),
             U2PS_FAKE_HOST_STALL);
  CHECK_EQ(feature(true, U2PS_REQ_RCPT_ITF, U2PS_FEATURE_TEST_MODE, 0x0100), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(feature(true, U2PS_REQ_RCPT_EP, U2PS_FEATURE_TEST_MODE, 0x0100), U2PS_FAKE_HOST_STALL);
  uint8_t data[1] = {0};
  CHECK_EQ(u2ps_fake_host_control(0x00, U2PS_SET_FEATURE, U2PS_FEATURE_TEST_MODE, 0x0100, 1, data,
                        NULL),
           U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_TEST_MODE), 0);
  CHECK_EQ(u2ps_fake_test_selector, -1);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static void test_configuration(void)
{
  uint8_t buf[1];
  uint16_t n;

  /* Default state: unconfigured, SET_CONFIGURATION refused */
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_GET_CONFIGURATION, 0, 0, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[0], 0);
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_STALL);

  /* Address state */
  CHECK_EQ(set_address(5), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_configuration(2), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x00, U2PS_SET_CONFIGURATION, 0x0101, 0, 0, NULL, NULL),
           U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x00, U2PS_SET_CONFIGURATION, 1, 1, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(set_configuration(0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_GET_CONFIGURATION, 0, 0, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[0], 0);
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_INTERFACE, 0, 0, 1, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, 0, 1, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);

  /* Configured */
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_GET_CONFIGURATION, 0, 0, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 1);
  CHECK_EQ(buf[0], 1);
  CHECK_EQ(set_configuration(2), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_GET_CONFIGURATION, 0, 0, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[0], 1);

  /* Alternate settings: interface 1 has 0..4, interface 0 only 0 */
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_INTERFACE, 0, 1, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 1);
  CHECK_EQ(buf[0], 0);
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, 4, 1, 0, NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_INTERFACE, 0, 1, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[0], 4);
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, 5, 1, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, 0x0101, 1, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, 1, 0, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, 0, 2, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_INTERFACE, 0, 2, 1, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_INTERFACE, 0, 1, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[0], 4);
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, 0, 0, 0, NULL, NULL), U2PS_FAKE_HOST_OK);
  /* SET_CONFIGURATION, even to the same value, resets them (9.1.1.5) */
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_INTERFACE, 0, 1, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[0], 0);
  /* Back to the Address state */
  CHECK_EQ(set_configuration(0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_INTERFACE, 0, 1, 1, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Endpoints exist only in a streaming alternate setting of their interface,
 * and the direction bit counts (9.4.5, 9.3.4 Figure 9-2): 0x01 and 0x81
 * exist in alternate settings 1..4, 0x02 and 0x82 never. */
static void test_endpoint_alt(void)
{
  uint16_t st;

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  /* alt 0: no endpoint of interface 1 exists, in either direction */
  static const uint16_t eps[] = {0x01, 0x81, 0x82, 0x02};
  for (unsigned i = 0; i < 4; i++) {
    CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, eps[i], &st), U2PS_FAKE_HOST_STALL);
    CHECK_EQ(feature(false, U2PS_REQ_RCPT_EP, U2PS_FEATURE_ENDPOINT_HALT, eps[i]),
             U2PS_FAKE_HOST_STALL);
  }
  /* alt 1 and alt 4: 0x01 and 0x81 */
  static const uint8_t alts[] = {1, 4};
  for (unsigned a = 0; a < 2; a++) {
    /* From alternate setting 1, the status waits for the close and the open */
    u2ps_fake_host_result_t r = u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, alts[a], 1, 0, NULL, NULL);
    if (a == 1u) {
      CHECK_EQ(r, U2PS_FAKE_HOST_NAK);
      u2ps_fake_closed();
      r = u2ps_fake_host_resume(NULL, NULL);
    }
    CHECK_EQ(r, U2PS_FAKE_HOST_OK);
    CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x01, &st), U2PS_FAKE_HOST_OK);
    CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x81, &st), U2PS_FAKE_HOST_OK);
    CHECK_EQ(feature(false, U2PS_REQ_RCPT_EP, U2PS_FEATURE_ENDPOINT_HALT, 0x81), U2PS_FAKE_HOST_OK);
    CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x82, &st), U2PS_FAKE_HOST_STALL);
    CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x02, &st), U2PS_FAKE_HOST_STALL);
    CHECK_EQ(feature(false, U2PS_REQ_RCPT_EP, U2PS_FEATURE_ENDPOINT_HALT, 0x82), U2PS_FAKE_HOST_STALL);
  }
  /* back to alt 0 */
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, 0, 1, 0, NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x01, &st), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x81, &st), U2PS_FAKE_HOST_STALL);
  /* EP0 in both directions, whatever the alternate setting */
  CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x00, &st), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_status(U2PS_REQ_RCPT_EP, 0x80, &st), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static int defer(const u2ps_request_t *req)
{
  (void)req;
  return U2PS_REQ_DEFER;
}

/* A deferred request's status stage completes on the bus, the next SETUP is
 * SET_FEATURE(TEST_MODE), and only then the old completion would be
 * reported. A port drops it (u2ps_dport.h); test mode starts only after
 * the new request's own status stage. */
static void test_stale_completion(void)
{
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  app_handler = defer;
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x33, 0, 1, 0, NULL, NULL), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, 0), true);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_STATUS_IN);
  u2ps_fake_ep0_status_done_unreported();

  u2ps_fake_clear_log();
  u2ps_fake_setup(0x00, U2PS_SET_FEATURE, U2PS_FEATURE_TEST_MODE, 0x0400, 0);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_STATUS_IN);
  u2ps_fake_report_status(); /* the old completion */
  CHECK_EQ(u2ps_fake_stale_dropped, 1);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_TEST_MODE), 0);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_STATUS_IN);
  /* the new request's own status stage */
  u2ps_fake_ep0_status_done();
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_TEST_MODE), 1);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_TEST_MODE, 0).from, U2PS_FAKE_FROM_STATUS);
  CHECK(u2ps_fake_find(U2PS_FAKE_TEST_MODE, 0) > u2ps_fake_find(U2PS_FAKE_EP0_STATUS, 0));
  CHECK_EQ(u2ps_fake_test_selector, 4);
  CHECK_EQ(u2ps_fake_violations, 0);

  /* Told to deliver the stale completion, the fake reports the violation,
   * and the stack would enter test mode before the status stage */
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  app_handler = defer;
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x33, 0, 1, 0, NULL, NULL), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, 0), true);
  u2ps_fake_ep0_status_done_unreported();
  u2ps_fake_setup(0x00, U2PS_SET_FEATURE, U2PS_FEATURE_TEST_MODE, 0x0400, 0);
  u2ps_fake_deliver_stale = true;
  u2ps_fake_report_status();
  CHECK_EQ(u2ps_fake_violations, 1);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_TEST_MODE), 1); /* the hazard the rule prevents */
  u2ps_fake_violations = 0;
}

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
  test_get_status();
  test_features();
  test_test_mode();
  test_endpoint_alt();
  test_stale_completion();
  test_configuration();
  return check_done();
}
