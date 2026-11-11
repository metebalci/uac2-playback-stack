/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_clock.c: the Clock Source's requests (UAC2 5.2.5.1.1-2): GET CUR
 * and RANGE of the Sampling Frequency Control (Layout 3, 5.2.3.3), SET CUR
 * with the app's hook accepting, refusing or deferring a rate (it sees the
 * value the host asked for and the rate chosen) and with a data stage
 * shorter than wLength (a request error), the description's initial
 * rate, GET CUR of
 * the Clock Validity Control (Layout 1, 5.2.3.1), and the requests that
 * must stall (5.2.2): other Controls, attributes, channels, Entities and
 * interfaces, and every class request at full speed.
 */
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "dev_support.h"

static uint8_t buf[512];
static uint16_t n;

static uint32_t get32(const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A class request to the clock: entity 4, interface 0 */
static u2ps_fake_host_result_t clock(uint8_t type, uint8_t request, uint16_t wvalue, uint16_t wlength,
                           void *data)
{
  return u2ps_fake_host_control(type, request, wvalue, 0x0400, wlength, data, &n);
}

static void test_get(void)
{
  uint32_t rate;

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();

  /* CUR: the first rate until the host sets one */
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 44100);
  /* Fewer bytes than the block: its first bytes (5.2.2) */
  CHECK_EQ(clock(0xA1, 0x01, 0x0100, 2, buf), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 2);
  CHECK_EQ(buf[0] | buf[1] << 8, 44100 & 0xFFFF);

  /* RANGE: as hosts read it, the count first, then all of it */
  CHECK_EQ(clock(0xA1, 0x02, 0x0100, 2, buf), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 2);
  CHECK_EQ(buf[0] | buf[1] << 8, TEST_RATE_COUNT);
  memset(buf, 0xEE, sizeof buf);
  CHECK_EQ(clock(0xA1, 0x02, 0x0100, 0x0200, buf), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 2 + 12 * TEST_RATE_COUNT);
  CHECK_EQ(buf[0] | buf[1] << 8, TEST_RATE_COUNT);
  for (unsigned i = 0; i < TEST_RATE_COUNT; i++) {
    /* each rate a subrange [rate, rate, 0], ascending (5.2.1) */
    CHECK_EQ(get32(buf + 2 + 12 * i), test_rates[i]);
    CHECK_EQ(get32(buf + 6 + 12 * i), test_rates[i]);
    CHECK_EQ(get32(buf + 10 + 12 * i), 0);
  }

  /* Clock Validity: valid */
  CHECK_EQ(clock(0xA1, 0x01, 0x0200, 1, buf), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 1);
  CHECK_EQ(buf[0], 1);
  CHECK_EQ(clock(0xA1, 0x01, 0x0200, 4, buf), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 1);

  /* None of these reach the app */
  CHECK_EQ(app_log.requests, 0);
  CHECK_EQ(app_log.rates, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static void test_set(void)
{
  uint32_t rate;

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();

  /* Accepted: the app sees the rate once, then GET CUR returns it */
  CHECK_EQ(set_rate(768000), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.rates, 1);
  CHECK_EQ(app_log.rate, 768000);
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 768000);
  CHECK_EQ(set_rate(48000), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 48000);

  /* A rate not in the description: the closest listed rate, the lower on
   * a tie (UAC2 5.2.2, p. 91); the app is offered that one, and GET CUR
   * returns it */
  static const struct {
    uint32_t asked, got;
  } near[] = {{0, 44100},       {32000, 44100},  {44101, 44100},       {46049, 44100},
              {46050, 44100},   {46051, 48000},  {47999, 48000},       {736800, 705600},
              {736801, 768000}, {768001, 768000}, {0xFFFFFFFFu, 768000}};
  for (unsigned i = 0; i < sizeof near / sizeof near[0]; i++) {
    CHECK_EQ(set_rate(near[i].asked), U2PS_FAKE_HOST_OK);
    CHECK_EQ(app_log.rate_requested, near[i].asked);
    CHECK_EQ(app_log.rate, near[i].got);
    CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
    CHECK_EQ(rate, near[i].got);
  }
  CHECK_EQ(set_rate(48000), U2PS_FAKE_HOST_OK);
  unsigned rates = app_log.rates;

  /* Refused by the app */
  app_cfg.rate_result = U2PS_REQ_STALL;
  CHECK_EQ(set_rate(96000), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(app_log.rates, rates + 1);
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 48000);
  /* Another negative result is a refusal too, and a contract violation */
  contract_expect(true);
  app_cfg.rate_result = -7;
  CHECK_EQ(set_rate(96000), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_DEV_CALLBACK_RESULT], 1);
  CHECK_EQ(contract_log.total, 1);
  contract_expect(false);
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 48000);

  /* Wrong lengths of the Layout 3 block: request error */
  app_cfg.rate_result = 0;
  uint8_t b8[8] = {0x00, 0x77, 0x01, 0x00};
  CHECK_EQ(clock(0x21, 0x01, 0x0100, 2, b8), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(clock(0x21, 0x01, 0x0100, 8, b8), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(clock(0x21, 0x01, 0x0100, 0, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(app_log.rates, rates + 2);
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 48000);

  /* SET CUR with wLength 8 and a data stage of only 4 bytes: wLength is
   * the exact length of the host's data (USB 2.0 9.3.5), so a request
   * error, whatever the 4 bytes say; the app sees no rate */
  static const uint8_t r96k[4] = {0x00, 0x77, 0x01, 0x00};
  u2ps_fake_clear_log();
  u2ps_fake_setup(0x21, 0x01, 0x0100, 0x0400, 8);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_OUT_DATA);
  u2ps_fake_ep0_out_done(r96k, 4);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_STALLED);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_STALL), 1);
  CHECK_EQ(app_log.rates, rates + 2);
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 48000);

  /* The rate stays across a bus reset */
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 48000);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static void test_deferred(void)
{
  uint32_t rate;

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  app_cfg.rate_result = U2PS_REQ_DEFER;
  u2ps_fake_clear_log();

  /* The status stage NAKs until the reply; the rate applies with it */
  CHECK_EQ(set_rate(192001), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(app_log.rate, 192000);
  CHECK_EQ(app_log.rate_requested, 192001);
  uint32_t id = app_log.rate_id;
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_STATUS), 0);
  CHECK_EQ(u2ps_dev_reply(id, NULL, 0), true);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 192000);

  /* Refused later: request error, no change */
  CHECK_EQ(set_rate(384000), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.rate_id, NULL, U2PS_REQ_STALL), true);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 192000);

  /* Cancelled by the next SETUP: the late reply changes nothing, and a
   * later reply to another request does not apply the old rate */
  CHECK_EQ(set_rate(384000), U2PS_FAKE_HOST_NAK);
  id = app_log.rate_id;
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 192000);
  CHECK_EQ(u2ps_dev_reply(id, NULL, 0), false);
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 192000);
  CHECK_EQ(set_rate(384000), U2PS_FAKE_HOST_NAK);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(u2ps_dev_reply(app_log.rate_id, NULL, 0), false);
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 192000);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* The description's initial rate is the clock's rate until the host sets
 * one: GET CUR returns it, and a stream started before any SET CUR runs at
 * it with its nominal feedback. 0 means the first rate. A bus reset keeps
 * the rate the host set; dev_start() re-inits so that the clock starts at
 * the initial rate again (a re-init keeps a rate the description lists:
 * test_dev_session). */
static void test_initial(void)
{
  uint32_t rate;

  test_desc.initial_sampling_frequency = 96000;
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 96000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.info.sampling_frequency, 96000);
  CHECK_EQ(u2ps_fake_stream.fb_value, 0x000C0000); /* 12 samples per microframe */
  CHECK_EQ(app_log.rates, 0);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(set_rate(48000), U2PS_FAKE_HOST_OK);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 48000);
  /* The last rate, and 0 */
  test_desc.initial_sampling_frequency = 768000;
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 768000);
  test_desc.initial_sampling_frequency = 0;
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(rate, 44100);
  CHECK_EQ(u2ps_fake_violations, 0);
  test_desc_load();
}

/* Every other class request is a request error (5.2.2) */
static void test_stalls(void)
{
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  uint8_t b4[4] = {0x80, 0xBB, 0x00, 0x00};

  /* Channel number other than 0: master Controls only */
  CHECK_EQ(clock(0xA1, 0x01, 0x0101, 4, buf), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(clock(0xA1, 0x02, 0x0101, 2, buf), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(clock(0x21, 0x01, 0x0101, 4, b4), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(clock(0xA1, 0x01, 0x0201, 1, buf), U2PS_FAKE_HOST_STALL);
  /* SET RANGE; SET of the Clock Validity; RANGE of the Clock Validity */
  CHECK_EQ(clock(0x21, 0x02, 0x0100, 4, b4), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(clock(0x21, 0x01, 0x0200, 1, b4), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(clock(0xA1, 0x02, 0x0200, 2, buf), U2PS_FAKE_HOST_STALL);
  /* Undefined Control selectors and request codes (A.14, A.17.1) */
  CHECK_EQ(clock(0xA1, 0x01, 0x0000, 4, buf), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(clock(0xA1, 0x01, 0x0300, 4, buf), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(clock(0xA1, 0x00, 0x0100, 4, buf), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(clock(0xA1, 0x03, 0x0100, 4, buf), U2PS_FAKE_HOST_STALL); /* MEM */
  /* Other Entities, the interfaces themselves, the clock's ID on the
   * streaming interface, the endpoint */
  static const uint16_t windex[] = {0x0100, 0x0300, 0x0500, 0x0000, 0x0001, 0x0401, 0xFF00};
  for (unsigned i = 0; i < sizeof windex / sizeof windex[0]; i++)
    CHECK_EQ(u2ps_fake_host_control(0xA1, 0x01, 0x0100, windex[i], 4, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0xA2, 0x01, 0x0100, 0x0001, 3, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x22, 0x01, 0x0100, 0x0001, 3, b4, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(app_log.requests, 0);
  CHECK_EQ(app_log.rates, 0);

  /* Not configured: no interface, no clock */
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(clock(0xA1, 0x01, 0x0100, 4, buf), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(set_address(3), U2PS_FAKE_HOST_OK);
  CHECK_EQ(clock(0xA1, 0x01, 0x0100, 4, buf), U2PS_FAKE_HOST_STALL);

  /* Full speed: the minimal FS configuration has no clock */
  dev_start(U2PS_SPEED_FULL);
  dev_configure();
  CHECK_EQ(clock(0xA1, 0x01, 0x0100, 4, buf), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(clock(0xA1, 0x02, 0x0100, 2, buf), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(clock(0x21, 0x01, 0x0100, 4, b4), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(clock(0xA1, 0x01, 0x0200, 1, buf), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(app_log.requests, 0);
  CHECK_EQ(app_log.rates, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
  test_get();
  test_set();
  test_deferred();
  test_initial();
  test_stalls();
  return check_done();
}
