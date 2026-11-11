/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_session.c: the device's session around the stream: start-up
 * with the port's reasons for a failure, re-init, attach and detach, the
 * configuration callbacks, the speed getter, and suspend and resume
 * (u2ps_dev.h). A detach ends a stream and the configuration as a bus
 * reset does; suspend counts only after a bus reset since the attach (an
 * idle bus after the pull-up is not a suspend of an enumerated device,
 * USB 2.0 7.1.7.6, 9.1.1.6).
 */
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "dev_support.h"

/* The app's events so far, as a string */
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

static void clear_events(void)
{
  app_log.nevents = 0;
}

/* Runs first: no init has succeeded yet in this process. A failed start
 * gives the port's reason and leaves the port unstarted, so the next init
 * starts it again; a refused description never reaches the port; once
 * started, an init is a re-init (u2ps_dport_reinit), from thread with
 * the vectors masked, never a second start. Nothing attaches before a
 * successful start. */
static void test_init_errors(void)
{
  static const u2ps_dev_err_t reasons[] = {
    U2PS_DEV_ERR_CLOCK, U2PS_DEV_ERR_POWER,  U2PS_DEV_ERR_SECURITY,
    U2PS_DEV_ERR_PHY,   U2PS_DEV_ERR_RESET,  U2PS_DEV_ERR_MODE,
    U2PS_DEV_ERR_MEMORY, U2PS_DEV_ERR_PORT,  U2PS_DEV_ERR_TIMER};

  /* Each attach below comes before a successful init, a contract violation */
  contract_expect(true);
  for (unsigned i = 0; i < sizeof reasons / sizeof reasons[0]; i++) {
    u2ps_fake_reset_all();
    u2ps_fake_init_err = reasons[i];
    CHECK_EQ(u2ps_dev_init(&test_desc), reasons[i]);
    CHECK_EQ(u2ps_dev_description_error(), U2PS_DESCRIPTION_OK);
    CHECK_EQ(u2ps_fake_count(U2PS_FAKE_INIT), 1);
    CHECK_EQ(u2ps_fake_count(U2PS_FAKE_REINIT), 0);
    /* Not started: an attach does nothing */
    u2ps_dev_connect(true);
    CHECK_EQ(u2ps_fake_count(U2PS_FAKE_CONNECT), 0);
    CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_NONE);
  }
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_DEV_CONNECT_BEFORE_INIT], 9);
  CHECK_EQ(contract_log.total, 9);
  contract_expect(false);
  u2ps_fake_reset_all();
  CHECK_EQ(u2ps_dev_init(NULL), U2PS_DEV_ERR_DESCRIPTION);
  CHECK_EQ(u2ps_dev_description_error(), U2PS_DESCRIPTION_NULL);
  u2ps_dev_description_t bad = test_desc;
  bad.format_count = 0;
  CHECK_EQ(u2ps_dev_init(&bad), U2PS_DEV_ERR_DESCRIPTION);
  CHECK_EQ(u2ps_dev_description_error(), U2PS_DESCRIPTION_FORMAT_COUNT);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_INIT), 0);

  u2ps_fake_reset_all();
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_INIT), 1);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_REINIT), 0);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_IRQ_MASK), 0);
  /* Started: a re-init does not start the hardware again */
  u2ps_fake_reset_all();
  u2ps_fake_init_err = U2PS_DEV_ERR_PHY;
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_INIT), 0);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_REINIT), 1);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_REINIT, 0).from, U2PS_FAKE_FROM_MASKED);
  /* The device stays detached until the app attaches */
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_CONNECT), 0);
  CHECK(!u2ps_fake_attached);
  u2ps_dev_connect(true);
  CHECK(u2ps_fake_attached);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* on_configured(true) when the device enters the Configured state, at
 * SET_CONFIGURATION 1, with the speed known; false when it leaves it.
 * Calls alternate: selecting configuration 1 again, or 0 while
 * unconfigured, gives none. At a bus reset the stream stops first, then
 * the configuration ends, then on_reset. */
static void test_configured(void)
{
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EVENTS("B");
  clear_events();
  CHECK_EQ(set_address(5), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_configuration(0), U2PS_FAKE_HOST_OK);
  CHECK_EVENTS("");
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  CHECK_EVENTS("C");
  CHECK_EQ(app_log.speed_at_configured, U2PS_SPEED_HIGH);
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  CHECK_EVENTS("C");
  CHECK_EQ(set_configuration(0), U2PS_FAKE_HOST_OK);
  CHECK_EVENTS("CD");
  CHECK_EQ(set_configuration(0), U2PS_FAKE_HOST_OK);
  CHECK_EVENTS("CD");
  /* A refused SET_CONFIGURATION changes nothing */
  CHECK_EQ(set_configuration(2), U2PS_FAKE_HOST_STALL);
  CHECK_EVENTS("CD");
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  CHECK_EVENTS("CDC");

  /* Bus reset with a stream: stop, unconfigured, reset */
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EVENTS("XDB");
  /* A reset while unconfigured: only on_reset */
  clear_events();
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EVENTS("B");

  /* SET_CONFIGURATION 0 with a stream: the configuration ends at once, the
   * stream when its close ends */
  dev_configure();
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  CHECK_EQ(set_configuration(0), U2PS_FAKE_HOST_OK);
  CHECK_EVENTS("D");
  u2ps_fake_closed();
  CHECK_EVENTS("DX");

  /* At full speed the app sees the speed it configures at */
  dev_start(U2PS_SPEED_FULL);
  dev_configure();
  CHECK_EQ(app_log.configured, 1);
  CHECK_EQ(app_log.speed_at_configured, U2PS_SPEED_FULL);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* on_configured names why the configuration ended: a bus reset,
 * SET_CONFIGURATION 0, a detach or a re-init (VBUS loss: test_dev_vbus);
 * NONE when it begins */
static void test_config_end(void)
{
  dev_start(U2PS_SPEED_HIGH);
  clear_events();
  dev_configure();
  CHECK_EVENTS("C");
  CHECK_EQ(app_log.config_end, U2PS_CONFIG_END_NONE);
  CHECK_EQ(set_configuration(0), U2PS_FAKE_HOST_OK);
  CHECK_EVENTS("CD");
  CHECK_EQ(app_log.config_end, U2PS_CONFIG_END_SET_CONFIGURATION);

  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.config_end, U2PS_CONFIG_END_NONE);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EVENTS("CDCDB");
  CHECK_EQ(app_log.config_end, U2PS_CONFIG_END_BUS_RESET);

  dev_configure();
  u2ps_dev_connect(false);
  CHECK_EVENTS("CDCDBCD");
  CHECK_EQ(app_log.config_end, U2PS_CONFIG_END_DETACH);

  u2ps_dev_connect(true);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  clear_events();
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  CHECK_EVENTS("D");
  CHECK_EQ(app_log.config_end, U2PS_CONFIG_END_REINIT);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* u2ps_dev_speed(): none while detached and until a reset has ended */
static void test_speed(void)
{
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_HIGH);
  u2ps_dev_connect(false);
  CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_NONE);
  u2ps_dev_connect(true);
  CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_NONE);
  u2ps_fake_bus_reset(U2PS_SPEED_FULL);
  CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_FULL);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_HIGH);
  /* A re-init detaches */
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_NONE);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static int defer_all(const u2ps_request_t *req)
{
  (void)req;
  return U2PS_REQ_DEFER;
}

/* A detach ends the session as a bus reset would: the port stops the
 * stream and EP0 itself, with the vectors masked; the app hears of the
 * stream's stop, then of the configuration's end; a deferred request is
 * cancelled; the device is in the Default state at address 0 */
static void test_detach(void)
{
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  uint32_t stream = app_log.info.stream;
  u2ps_fake_packet(28, 0x0100);
  app_handler = defer_all;
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, NULL, NULL), U2PS_FAKE_HOST_NAK);
  uint32_t id = app_log.last.id;
  clear_events();
  u2ps_fake_clear_log();

  u2ps_dev_connect(false);
  CHECK_EVENTS("XD");
  int c = u2ps_fake_find(U2PS_FAKE_CONNECT, 0);
  CHECK(c >= 0);
  if (c >= 0) {
    CHECK_EQ(u2ps_fake_calls[c].arg, 0);
    CHECK_EQ(u2ps_fake_calls[c].from, U2PS_FAKE_FROM_MASKED);
  }
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_IDLE);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_CLOSE), 0);
  CHECK(!u2ps_fake_attached);
  /* The stream is gone: a release for it is ignored and counted */
  u2ps_dev_stats_t st;
  u2ps_dev_stats(&st);
  uint32_t release_errors = st.release_errors;
  u2ps_dev_release(stream, 1);
  u2ps_dev_stats(&st);
  CHECK_EQ(st.release_errors, release_errors + 1u);
  /* The deferred request is cancelled */
  CHECK(!u2ps_dev_reply(id, "abcd", 4));
  /* A second detach changes nothing */
  clear_events();
  u2ps_dev_connect(false);
  CHECK_EVENTS("");

  /* Attached again: the host resets, addresses and configures anew */
  u2ps_dev_connect(true);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  app_handler = NULL;
  dev_configure();
  CHECK_EQ(u2ps_fake_address, 5);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK(app_log.info.stream != stream);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_OPEN);

  /* A detach during a close: the close ends there, no on_closed follows */
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_CLOSING);
  clear_events();
  u2ps_dev_connect(false);
  CHECK_EVENTS("XD");
  /* Detached while unconfigured: no callback */
  u2ps_dev_connect(true);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  clear_events();
  u2ps_dev_connect(false);
  CHECK_EVENTS("");
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A detach while detached, a second u2ps_dev_connect(false) or one before
 * any u2ps_dev_connect(true), is harmless: no callback, no port call, no
 * change of state; the next attach works as ever */
static void test_detach_twice(void)
{
  u2ps_dev_stats_t before, after;

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  u2ps_dev_connect(false);
  clear_events();
  u2ps_fake_clear_log();
  u2ps_dev_stats(&before);
  u2ps_dev_connect(false);
  CHECK_EVENTS("");
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_CONNECT), 0);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_IRQ_MASK), 2); /* the two stats calls only */
  u2ps_dev_stats(&after);
  CHECK(memcmp(&before, &after, sizeof before) == 0);
  CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_NONE);
  CHECK(!u2ps_fake_attached);

  /* Before any attach since the (re-)init */
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  clear_events();
  u2ps_fake_clear_log();
  u2ps_dev_connect(false);
  u2ps_dev_connect(false);
  CHECK_EVENTS("");
  CHECK_EQ(u2ps_fake_ncalls, 0);
  CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_NONE);

  u2ps_dev_connect(true);
  CHECK(u2ps_fake_attached);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EVENTS("BC");
  CHECK_EQ(u2ps_fake_address, 5);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Suspend reaches the app only after a bus reset since the attach, once
 * per suspend; a resume only after a suspend the app heard of */
static void test_suspend_after_reset(void)
{
  dev_start(U2PS_SPEED_HIGH);
  u2ps_dev_connect(false);
  u2ps_dev_connect(true);
  clear_events();
  /* The idle bus after the pull-up */
  u2ps_fake_suspend();
  u2ps_fake_resume();
  CHECK_EVENTS("");
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  u2ps_fake_suspend();
  u2ps_fake_suspend();
  u2ps_fake_resume();
  u2ps_fake_resume();
  CHECK_EVENTS("BUW");
  CHECK_EQ(app_log.suspends, 1);
  CHECK_EQ(app_log.resumes, 1);
  /* A detach while suspended: nothing; then the same after the new attach */
  u2ps_fake_suspend();
  u2ps_dev_connect(false);
  u2ps_dev_connect(true);
  clear_events();
  u2ps_fake_suspend();
  u2ps_fake_resume();
  CHECK_EVENTS("");
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  u2ps_fake_suspend();
  CHECK_EVENTS("BU");
  /* A bus reset ends a suspend without a resume */
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  u2ps_fake_resume();
  u2ps_fake_suspend();
  CHECK_EVENTS("BUBU");
  CHECK_EQ(u2ps_fake_violations, 0);
}

static const uint32_t other_rates[2] = {96000, 192000};

/* A re-init while attached detaches first, ends the stream and the
 * configuration, skips the port's start-up and takes the new description;
 * stream numbers go on. A refused description changes nothing. While
 * detached, a re-init only resets. The clock keeps its sampling frequency
 * if the new description lists it, and takes the new initial one
 * otherwise. */
static void test_reinit(void)
{
  uint32_t rate;

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  uint32_t stream = app_log.info.stream;
  clear_events();
  u2ps_fake_clear_log();

  u2ps_dev_description_t bad = test_desc;
  bad.initial_sampling_frequency = 50000;
  CHECK_EQ(u2ps_dev_init(&bad), U2PS_DEV_ERR_DESCRIPTION);
  CHECK_EQ(u2ps_dev_description_error(), U2PS_DESCRIPTION_INITIAL_SAMPLING_FREQUENCY);
  CHECK_EVENTS("");
  CHECK_EQ(u2ps_fake_ncalls, 0);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_OPEN);

  u2ps_dev_description_t other = test_desc;
  other.sampling_frequencies = other_rates;
  other.sampling_frequency_count = 2;
  other.initial_sampling_frequency = 192000;
  CHECK_EQ(u2ps_dev_init(&other), U2PS_DEV_OK);
  CHECK_EQ(u2ps_dev_description_error(), U2PS_DESCRIPTION_OK);
  CHECK_EVENTS("XD");
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_INIT), 0);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_REINIT), 1);
  CHECK(u2ps_fake_find(U2PS_FAKE_CONNECT, 0) < u2ps_fake_find(U2PS_FAKE_REINIT, 0));
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_IDLE);
  CHECK(!u2ps_fake_attached);

  u2ps_dev_connect(true);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 192000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK(app_log.info.stream > stream);
  CHECK_EQ(app_log.info.sampling_frequency, 192000);

  /* Detached first: the re-init calls nothing else */
  u2ps_dev_connect(false);
  clear_events();
  u2ps_fake_clear_log();
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  CHECK_EVENTS("");
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_CONNECT), 0);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_REINIT), 1);
  u2ps_dev_connect(true);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  /* test_desc lists 192 kHz too: kept */
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 192000);

  /* The host's sampling frequency, listed in the new description, survives
   * a re-init while attached; one the new description lacks gives way to
   * its initial one */
  CHECK_EQ(set_rate(96000), U2PS_FAKE_HOST_OK);
  other.initial_sampling_frequency = 192000;
  CHECK_EQ(u2ps_dev_init(&other), U2PS_DEV_OK);
  u2ps_dev_connect(true);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 96000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.info.sampling_frequency, 96000);
  CHECK_EQ(set_rate(48000), U2PS_FAKE_HOST_OK); /* closest of 96 and 192 kHz: 96 kHz */
  CHECK_EQ(set_rate(44100), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 96000);
  u2ps_dev_description_t third = test_desc;
  third.sampling_frequencies = other_rates + 1; /* 192 kHz only */
  third.sampling_frequency_count = 1;
  CHECK_EQ(u2ps_dev_init(&third), U2PS_DEV_OK);
  u2ps_dev_connect(true);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 192000);
  CHECK_EQ(u2ps_fake_violations, 0);
}

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
  /* No rule broken before the first init */
  CHECK_EQ(u2ps_dev_description_error(), U2PS_DESCRIPTION_OK);
  /* VBUS is present throughout; a report before the first init counts */
  u2ps_fake_vbus_set(true);
  test_init_errors();
  test_configured();
  test_config_end();
  test_speed();
  test_detach();
  test_detach_twice();
  test_suspend_after_reset();
  test_reinit();
  return check_done();
}
