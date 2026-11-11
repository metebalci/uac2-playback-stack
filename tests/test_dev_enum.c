/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_enum.c: the device stack through enumeration at high speed in the
 * order a Linux host used at a device's first attach, as recorded on the
 * bus (two resets around the first 64-byte device descriptor read, then
 * SET_ADDRESS, the descriptors, SET_CONFIGURATION and the audio driver's
 * requests), SET_ADDRESS's order of port calls (USB 2.0 9.4.6), and bus
 * reset in every state (9.1.1.3).
 */
#include <string.h>

#include "check.h"
#include "dev_support.h"

static void test_linux_order(void)
{
  uint8_t buf[512];
  uint16_t n;

  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(u2ps_fake_find(U2PS_FAKE_DMA_CHECK, 0), 0);
  CHECK_EQ(u2ps_fake_find(U2PS_FAKE_INIT, 0), 1);
  CHECK_EQ(u2ps_fake_find(U2PS_FAKE_CONNECT, 0), 2);
  CHECK_EQ(app_log.resets, 1);

  /* The first read at address 0 asks for 64 bytes: one short packet */
  u2ps_fake_clear_log();
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 64, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 18);
  CHECK(memcmp(buf, test_device, 18) == 0);
  int i = u2ps_fake_find(U2PS_FAKE_EP0_IN, 0);
  CHECK(i >= 0);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_IN, 0).len, 18);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_IN, 0).zlp, false);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_IN, 0).from, U2PS_FAKE_FROM_SETUP);
  /* Status: the host's OUT, armed after the data stage */
  int s = u2ps_fake_find(U2PS_FAKE_EP0_STATUS, 0);
  CHECK(s > i);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_STATUS, 0).arg, 0);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_STATUS, 0).from, U2PS_FAKE_FROM_DATA);

  /* Second reset, then SET_ADDRESS */
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EQ(app_log.resets, 2);
  u2ps_fake_clear_log();
  u2ps_fake_setup(0x00, U2PS_SET_ADDRESS, 9, 0, 0);
  int a = u2ps_fake_find(U2PS_FAKE_SET_ADDRESS, 0);
  s = u2ps_fake_find(U2PS_FAKE_EP0_STATUS, 0);
  CHECK(a >= 0);
  CHECK(s >= 0);
  CHECK(a < s); /* set_address before ep0_status, both in on_setup */
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_SET_ADDRESS, 0).arg, 9);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_SET_ADDRESS, 0).from, U2PS_FAKE_FROM_SETUP);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_STATUS, 0).arg, 1); /* IN status: no data stage */
  CHECK_EQ(u2ps_fake_address, 0);      /* not before the status stage */
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_address, 9);

  /* Device descriptor, then the configuration: 9 bytes, then all of it */
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 18, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 18);
  CHECK(memcmp(buf, test_device, 18) == 0);
  CHECK_EQ(get_descriptor(U2PS_DESC_CONFIGURATION, 0, 0, 9, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 9);
  CHECK(memcmp(buf, test_config_hs, 9) == 0);
  uint16_t total = (uint16_t)(buf[2] | buf[3] << 8);
  CHECK_EQ(total, test_config_hs_len);
  CHECK_EQ(get_descriptor(U2PS_DESC_CONFIGURATION, 0, 0, total, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, test_config_hs_len);
  CHECK(memcmp(buf, test_config_hs, test_config_hs_len) == 0);

  /* Strings: the LANGIDs, then product, manufacturer, serial number */
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 0, 0, 255, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 4);
  CHECK_EQ(buf[2] | buf[3] << 8, TEST_LANGID);
  static const uint8_t order[] = {2, 1, 3};
  static const uint16_t lens[] = {64, 16, 128};
  for (unsigned k = 0; k < 3; k++) {
    CHECK_EQ(get_descriptor(U2PS_DESC_STRING, order[k], TEST_LANGID, 255, buf, &n), U2PS_FAKE_HOST_OK);
    CHECK_EQ(n, lens[k]);
    CHECK(memcmp(buf, test_strings[order[k]], lens[k]) == 0);
  }

  /* SET_CONFIGURATION, then what Linux's audio driver sends at the first
   * attach: the product and manufacturer strings again; for each
   * alternate setting k of 1 to 4, the clock's RANGE (2 bytes, then all
   * of it), SET_INTERFACE alt 0, a look at the clock (validity, frequency,
   * a SET CUR to the highest rate if it differs, then frequency again,
   * then validity), and SET_INTERFACE alt k; at the end alt 0. */
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 2, TEST_LANGID, 255, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, lens[0]);
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 1, TEST_LANGID, 255, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, lens[1]);
  const uint32_t top = test_rates[TEST_RATE_COUNT - 1u];
  for (uint8_t alt = 1; alt <= 4; alt++) {
    /* RANGE of the Sampling Frequency Control: wNumSubRanges, then one
     * (MIN, MAX, RES) triple per rate, with MIN = MAX and RES = 0 */
    CHECK_EQ(u2ps_fake_host_control(0xA1, 0x02, 0x0100, 0x0400, 2, buf, &n), U2PS_FAKE_HOST_OK);
    CHECK_EQ(n, 2);
    CHECK_EQ(buf[0] | buf[1] << 8, TEST_RATE_COUNT);
    uint16_t range = (uint16_t)(2u + 12u * TEST_RATE_COUNT);
    CHECK_EQ(u2ps_fake_host_control(0xA1, 0x02, 0x0100, 0x0400, range, buf, &n), U2PS_FAKE_HOST_OK);
    CHECK_EQ(n, range);
    for (unsigned r = 0; r < TEST_RATE_COUNT; r++) {
      const uint8_t *t = buf + 2u + 12u * r;
      CHECK_EQ(t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24, test_rates[r]);
      CHECK_EQ(t[4] | t[5] << 8 | t[6] << 16 | (uint32_t)t[7] << 24, test_rates[r]);
      CHECK_EQ(t[8] | t[9] << 8 | t[10] << 16 | (uint32_t)t[11] << 24, 0);
    }

    CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
    if (alt > 1) {
      /* The stream of the last alternate setting closes */
      CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_CLOSING);
      u2ps_fake_closed();
    }
    CHECK_EQ(u2ps_fake_host_control(0xA1, 0x01, 0x0200, 0x0400, 1, buf, &n), U2PS_FAKE_HOST_OK);
    CHECK_EQ(n, 1);
    CHECK_EQ(buf[0], 1); /* Clock Validity Control: valid */
    uint32_t rate = 0;
    CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
    CHECK_EQ(rate, alt == 1 ? test_rates[0] : top); /* the first rate until SET CUR */
    if (rate != top) {
      CHECK_EQ(set_rate(top), U2PS_FAKE_HOST_OK);
      CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
      CHECK_EQ(rate, top);
    }
    CHECK_EQ(u2ps_fake_host_control(0xA1, 0x01, 0x0200, 0x0400, 1, buf, &n), U2PS_FAKE_HOST_OK);
    CHECK_EQ(buf[0], 1);

    CHECK_EQ(set_interface(1, alt), U2PS_FAKE_HOST_OK);
    CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_OPEN);
    CHECK_EQ(app_log.info.alternate_setting, alt);
    CHECK_EQ(app_log.info.sampling_frequency, top);
  }
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  u2ps_fake_closed();
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_IDLE);
  CHECK_EQ(app_log.rates, 1);
  CHECK_EQ(app_log.rate, top);
  CHECK_EQ(app_log.starts, 4);
  CHECK_EQ(app_log.stops, 4);
  CHECK_EQ(app_log.requests, 0);

  /* Four streams, no stray port call */
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), 4);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_CLOSE), 4);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_TEST_MODE), 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* SET_ADDRESS at FS too, and to 0, which returns the device to Default */
static void test_set_address(void)
{
  uint8_t buf[2];

  dev_start(U2PS_SPEED_FULL);
  u2ps_fake_clear_log();
  CHECK_EQ(set_address(127), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_address, 127);
  int a = u2ps_fake_find(U2PS_FAKE_SET_ADDRESS, 0);
  int s = u2ps_fake_find(U2PS_FAKE_EP0_STATUS, 0);
  CHECK(a >= 0 && a < s);

  /* Address 0: Default state, where SET_CONFIGURATION is refused */
  CHECK_EQ(set_address(0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_address, 0);
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(set_address(3), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);

  /* Not when configured (9.4.6 leaves it unspecified), not above 127, and
   * not with wIndex or wLength set; none of these calls set_address */
  u2ps_fake_clear_log();
  CHECK_EQ(set_address(4), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(set_configuration(0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_address(128), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x00, U2PS_SET_ADDRESS, 4, 1, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x00, U2PS_SET_ADDRESS, 4, 0, 2, buf, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_ADDRESS, 4, 0, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_SET_ADDRESS), 0);
  CHECK_EQ(u2ps_fake_address, 3);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static int defer(const u2ps_request_t *req)
{
  (void)req;
  return U2PS_REQ_DEFER;
}

/* A bus reset returns the device to Default from every state, and cancels
 * whatever EP0 was doing */
static void test_bus_reset(void)
{
  uint8_t buf[512];
  uint16_t n;

  /* Configured, with an alternate setting and a pending reply */
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, 2, 1, 0, NULL, NULL), U2PS_FAKE_HOST_OK);
  app_handler = defer;
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0x0100, 0x0400, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  uint32_t id = app_log.last.id;

  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EQ(app_log.resets, 2);
  CHECK_EQ(u2ps_fake_address, 0);
  /* The pending reply is dropped and touches no port call but the mask */
  u2ps_fake_clear_log();
  CHECK_EQ(u2ps_dev_reply(id, "abcd", 4), false);
  CHECK_EQ(u2ps_fake_ncalls, 2);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_IRQ_MASK), 2);
  /* Default: not configured, interfaces gone */
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_GET_CONFIGURATION, 0, 0, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[0], 0);
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_INTERFACE, 0, 1, 1, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0x0100, 0x0400, 4, buf, &n), U2PS_FAKE_HOST_STALL);
  /* Configured again: alternate settings start at 0 */
  dev_configure();
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_INTERFACE, 0, 1, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[0], 0);

  /* Reset in the middle of a data stage: the next SETUP works */
  u2ps_fake_setup(0x80, U2PS_GET_DESCRIPTOR, U2PS_DESC_CONFIGURATION << 8, 0, 512);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_IN_DATA);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 64, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 18);

  /* Reset between SET_ADDRESS's SETUP and its status stage: the address
   * never applies */
  u2ps_fake_setup(0x00, U2PS_SET_ADDRESS, 7, 0, 0);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EQ(u2ps_fake_address, 0);
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_STALL); /* still Default */

  /* Reset in the Address state */
  CHECK_EQ(set_address(8), U2PS_FAKE_HOST_OK);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(app_log.resets, 5);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A probe: SET_ADDRESS's status completes on the bus, but the
 * port serves the next SETUP before it reports the completion (ChipIdea
 * serves SETUPs before completions, IMXRT1060RM Rev. 3 42.5.6.7.1). The
 * device must be in the Address state anyway. */
static void test_late_status(void)
{
  uint8_t buf[18];
  uint16_t n;

  dev_start(U2PS_SPEED_HIGH);
  u2ps_fake_setup(0x00, U2PS_SET_ADDRESS, 9, 0, 0);
  u2ps_fake_ep0_status_done_unreported();
  CHECK_EQ(u2ps_fake_address, 9);
  u2ps_fake_setup(0x80, U2PS_GET_DESCRIPTOR, U2PS_DESC_DEVICE << 8, 0, 18);
  u2ps_fake_report_status(); /* late: a conforming port drops it */
  CHECK_EQ(u2ps_fake_stale_dropped, 1);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 18);
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_violations, 0);

  /* The completion is never reported at all: the next SETUP replaced it */
  dev_start(U2PS_SPEED_HIGH);
  u2ps_fake_setup(0x00, U2PS_SET_ADDRESS, 10, 0, 0);
  u2ps_fake_ep0_status_done_unreported();
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_GET_CONFIGURATION, 0, 0, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[0], 1);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* The ACK of the last IN data packet is lost; the host's status OUT stands
 * for it (USB 2.0 8.5.3.3). The stack arms the status OUT inside
 * on_ep0_data, so the port can complete it. */
static void test_lost_ack(void)
{
  uint8_t buf[18];
  uint16_t n;

  dev_start(U2PS_SPEED_HIGH);
  u2ps_fake_clear_log();
  u2ps_fake_setup(0x80, U2PS_GET_DESCRIPTOR, U2PS_DESC_CONFIGURATION << 8, 0, 9);
  u2ps_fake_ep0_in_ack_lost();
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_STATUS, 0).arg, 0);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_STATUS, 0).from, U2PS_FAKE_FROM_DATA);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_NAK);
  /* EP0 goes on normally */
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 18, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 18);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Suspend and resume reach the app and keep address and configuration
 * (9.1.1.6) */
static void test_suspend(void)
{
  uint8_t buf[1];
  uint16_t n;

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  u2ps_fake_suspend();
  CHECK_EQ(app_log.suspends, 1);
  u2ps_fake_resume();
  CHECK_EQ(app_log.resumes, 1);
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_GET_CONFIGURATION, 0, 0, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[0], 1);
  CHECK_EQ(u2ps_fake_address, 5);
  CHECK_EQ(app_log.resets, 1);
  CHECK_EQ(u2ps_fake_violations, 0);
}

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
  test_linux_order();
  test_set_address();
  test_bus_reset();
  test_late_status();
  test_lost_ack();
  test_suspend();
  return check_done();
}
