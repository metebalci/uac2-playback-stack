/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_vbus.c: VBUS (u2ps_dev.h), built once per VBUS mode.
 * - Mode app: the pull-up is on only while the attach is allowed and VBUS
 *   is present (USB 2.0 7.1.5.1, 7.2.1); reports before the first init
 *   count and a re-init keeps the level; repeated levels change nothing; a
 *   VBUS loss ends the session as a detach does, in the order
 *   on_stream_stop, on_configured(false, VBUS), on_vbus(false), with
 *   nothing else called; it cancels a deferred request and a pending
 *   close's on_closed, and gives no on_resume to a suspended device; while
 *   VBUS is absent every port event is ignored; each pull-up starts a new
 *   wait for the host's first reset.
 * - Mode bus: VBUS is always present, the pull-up comes at once, and a
 *   self-powered description is refused (a bus-powered device cannot be
 *   self-powered).
 */
#include <string.h>

#include "check.h"
#include "dev_support.h"

#if U2PS_VBUS == U2PS_VBUS_APP

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

/* Runs first: no report and no init yet in this process. VBUS is absent
 * until the first report, which counts although it comes before the first
 * init; on_vbus comes for it, unmasked, as no vector runs yet. */
static void test_before_init(void)
{
  CHECK(!u2ps_dev_vbus_present());
  u2ps_fake_reset_all();
  u2ps_fake_vbus_set(true);
  CHECK(u2ps_dev_vbus_present());
  CHECK_EVENTS("V");
  CHECK(!app_log.vbus_masked);
  CHECK_EQ(u2ps_fake_ncalls, 0);

  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  CHECK(u2ps_dev_vbus_present());
  u2ps_dev_connect(true);
  CHECK(u2ps_fake_attached);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* The pull-up follows "allowed and VBUS present", whichever changes */
static void test_pullup(void)
{
  dev_start(U2PS_SPEED_HIGH);
  CHECK(u2ps_fake_attached);

  /* Not allowed: a loss and a return only report */
  u2ps_dev_connect(false);
  clear();
  u2ps_fake_vbus_set(false);
  CHECK_EVENTS("v");
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_CONNECT), 0);
  CHECK(app_log.vbus_masked);
  CHECK(!u2ps_dev_vbus_present());
  /* Allowed without VBUS: no pull-up */
  u2ps_dev_connect(true);
  CHECK(!u2ps_fake_attached);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_CONNECT), 0);
  /* VBUS returns: the pull-up first, with the vectors masked, then on_vbus */
  u2ps_fake_vbus_set(true);
  CHECK(u2ps_fake_attached);
  CHECK_EVENTS("vV");
  int c = u2ps_fake_find(U2PS_FAKE_CONNECT, 0);
  CHECK(c >= 0);
  if (c >= 0) {
    CHECK_EQ(u2ps_fake_calls[c].arg, 1);
    CHECK_EQ(u2ps_fake_calls[c].from, U2PS_FAKE_FROM_MASKED);
    CHECK((unsigned)c < app_log.calls_at_vbus);
  }
  /* Not allowed: VBUS comes and goes without a pull-up */
  u2ps_dev_connect(false);
  CHECK(!u2ps_fake_attached);
  clear();
  u2ps_fake_vbus_set(false);
  u2ps_fake_vbus_set(true);
  CHECK_EVENTS("vV");
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_CONNECT), 0);
  CHECK(!u2ps_fake_attached);
  u2ps_dev_connect(true);
  CHECK(u2ps_fake_attached);

  /* The same level again changes nothing: no callback, no port call */
  clear();
  u2ps_fake_vbus_set(true);
  u2ps_fake_vbus_set(true);
  CHECK_EVENTS("");
  CHECK_EQ(u2ps_fake_ncalls, 0);
  u2ps_fake_vbus_set(false);
  clear();
  u2ps_fake_vbus_set(false);
  CHECK_EVENTS("");
  CHECK_EQ(u2ps_fake_ncalls, 0);
  u2ps_fake_vbus_set(true);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static int defer_all(const u2ps_request_t *req)
{
  (void)req;
  return U2PS_REQ_DEFER;
}

/* A VBUS loss while streaming: the pull-up goes off as at a detach, then
 * the stream stops, the configuration ends for VBUS and on_vbus(false)
 * comes, all masked; nothing else is called. A deferred request is
 * cancelled, the stream is gone, the speed is unknown. */
static void test_loss(void)
{
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  uint32_t stream = app_log.info.stream;
  u2ps_fake_packet(28, 0x0100);
  app_handler = defer_all;
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, NULL, NULL), U2PS_FAKE_HOST_NAK);
  uint32_t id = app_log.last.id;
  clear();

  u2ps_fake_vbus_set(false);
  CHECK_EVENTS("XDv");
  CHECK_EQ(app_log.config_end, U2PS_CONFIG_END_VBUS);
  CHECK(app_log.vbus_masked);
  CHECK_EQ(u2ps_fake_ncalls, 3);
  CHECK_EQ(u2ps_fake_calls[0].kind, U2PS_FAKE_IRQ_MASK);
  CHECK_EQ(u2ps_fake_calls[0].arg, 1);
  CHECK_EQ(u2ps_fake_calls[1].kind, U2PS_FAKE_CONNECT);
  CHECK_EQ(u2ps_fake_calls[1].arg, 0);
  CHECK_EQ(u2ps_fake_calls[1].from, U2PS_FAKE_FROM_MASKED);
  CHECK_EQ(u2ps_fake_calls[2].kind, U2PS_FAKE_IRQ_MASK);
  CHECK_EQ(u2ps_fake_calls[2].arg, 0);
  CHECK(!u2ps_fake_attached);
  CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_NONE);
  CHECK(!u2ps_dev_reply(id, "abcd", 4));
  u2ps_dev_stats_t st;
  u2ps_dev_stats(&st);
  uint32_t release_errors = st.release_errors;
  u2ps_dev_release(stream, 1);
  u2ps_dev_stats(&st);
  CHECK_EQ(st.release_errors, release_errors + 1u);

  /* The return: the host resets, addresses and configures anew */
  clear();
  u2ps_fake_vbus_set(true);
  CHECK_EVENTS("V");
  CHECK(u2ps_fake_attached);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  app_handler = NULL;
  dev_configure();
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK(app_log.info.stream != stream);
  CHECK_EVENTS("VBCS");
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A VBUS loss while a close is pending: on_stream_stop still comes, at the
 * loss, and no on_closed later; at SET_CONFIGURATION 0 the configuration
 * has ended before */
static void test_loss_closing(void)
{
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_CLOSING);
  clear();
  u2ps_fake_vbus_set(false);
  CHECK_EVENTS("XDv");
  u2ps_dev_on_closed(); /* a late report: ignored */
  CHECK_EVENTS("XDv");
  CHECK_EQ(app_log.stops, 1);

  u2ps_fake_vbus_set(true);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear();
  CHECK_EQ(set_configuration(0), U2PS_FAKE_HOST_OK);
  u2ps_fake_vbus_set(false);
  CHECK_EVENTS("DXv");
  CHECK_EQ(app_log.config_end, U2PS_CONFIG_END_SET_CONFIGURATION);
  u2ps_fake_vbus_set(true);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A suspended device that loses VBUS gets no on_resume, neither while VBUS
 * is absent nor after it returns; suspend counts again only after the next
 * bus reset */
static void test_loss_suspended(void)
{
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  clear();
  u2ps_fake_suspend();
  u2ps_fake_vbus_set(false);
  u2ps_fake_resume();
  u2ps_fake_vbus_set(true);
  u2ps_fake_resume();
  u2ps_fake_suspend();
  CHECK_EVENTS("UDvV");
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  u2ps_fake_suspend();
  u2ps_fake_resume();
  CHECK_EVENTS("UDvVBUW");
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* While VBUS is absent the stack ignores every port event and counts none */
static void test_guard(void)
{
  uint8_t buf[64];
  u2ps_dev_stats_t before, after;

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  u2ps_fake_vbus_set(false);
  u2ps_dev_stats(&before);
  clear();

  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_NONE);
  u2ps_fake_suspend();
  u2ps_fake_resume();
  u2ps_fake_setup(0x80, U2PS_GET_DESCRIPTOR, U2PS_DESC_DEVICE << 8, 0, 64);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_NAK);
  u2ps_dev_on_ep0_data(0);
  u2ps_dev_on_ep0_status();
  u2ps_dev_on_packet(app_mem, 0, 0x0100);
  u2ps_dev_on_missed(5, U2PS_MISS_LATE);
  u2ps_dev_on_closed();
  CHECK_EVENTS("");
  CHECK_EQ(u2ps_fake_ncalls, 0);
  CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_NONE);
  u2ps_dev_stats(&after);
  CHECK(memcmp(&before, &after, sizeof before) == 0);

  /* VBUS returns: all as at an attach */
  u2ps_fake_vbus_set(true);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  uint16_t n = 0;
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 64, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 18);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Each pull-up starts a new wait for the host's first reset: a reset the
 * port reported while detached does not make the idle bus after the next
 * pull-up a suspend */
static void test_reset_seen(void)
{
  dev_start(U2PS_SPEED_HIGH);
  u2ps_dev_connect(false);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  u2ps_fake_vbus_set(false);
  u2ps_dev_connect(true);
  clear();
  u2ps_fake_vbus_set(true);
  CHECK(u2ps_fake_attached);
  u2ps_fake_suspend();
  CHECK_EVENTS("V");
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  u2ps_fake_suspend();
  CHECK_EVENTS("VBU");
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A re-init keeps the VBUS level: present, the pull-up comes at once;
 * absent, only once VBUS returns */
static void test_reinit(void)
{
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  CHECK(u2ps_dev_vbus_present());
  u2ps_dev_connect(true);
  CHECK(u2ps_fake_attached);
  u2ps_fake_vbus_set(false);
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  CHECK(!u2ps_dev_vbus_present());
  clear();
  u2ps_dev_connect(true);
  CHECK(!u2ps_fake_attached);
  u2ps_fake_vbus_set(true);
  CHECK(u2ps_fake_attached);
  CHECK_EVENTS("V");
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A VBUS loss in test mode: the port's detach ends test mode */
static void test_test_mode(void)
{
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(set_address(5), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_host_control(0x00, U2PS_SET_FEATURE, U2PS_FEATURE_TEST_MODE, U2PS_TEST_J << 8, 0,
                        NULL, NULL),
           U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_test_selector, U2PS_TEST_J);
  u2ps_fake_vbus_set(false);
  CHECK_EQ(u2ps_fake_test_selector, -1);
  CHECK(!u2ps_fake_attached);
  u2ps_fake_vbus_set(true);
  CHECK_EQ(u2ps_fake_violations, 0);
}

#else /* U2PS_VBUS_BUS */

/* VBUS is present before and after the first init; the pull-up comes at
 * the attach */
static void test_bus(void)
{
  CHECK(u2ps_dev_vbus_present());
  u2ps_fake_reset_all();
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  CHECK(u2ps_dev_vbus_present());
  u2ps_dev_connect(true);
  CHECK(u2ps_fake_attached);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_CONNECT, 0).arg, 1);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(app_log.configured, 1);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A self-powered description is refused, and nothing changes */
static void test_bus_self_powered(void)
{
  dev_start(U2PS_SPEED_HIGH);
  u2ps_fake_clear_log();
  test_desc.self_powered = true;
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_ERR_DESCRIPTION);
  CHECK_EQ(u2ps_dev_description_error(), U2PS_DESCRIPTION_SELF_POWERED);
  CHECK_EQ(u2ps_fake_ncalls, 0);
  CHECK(u2ps_fake_attached);
  test_desc.self_powered = false;
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  CHECK_EQ(u2ps_dev_description_error(), U2PS_DESCRIPTION_OK);
  CHECK_EQ(u2ps_fake_violations, 0);
}

#endif

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
#if U2PS_VBUS == U2PS_VBUS_APP
  test_before_init();
  test_pullup();
  test_loss();
  test_loss_closing();
  test_loss_suspended();
  test_guard();
  test_reset_seen();
  test_reinit();
  test_test_mode();
#else
  test_bus();
  test_bus_self_powered();
#endif
  return check_done();
}
