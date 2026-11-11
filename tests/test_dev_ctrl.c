/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_ctrl.c: the control pipe: short packets and zero-length packets
 * at the end of a data stage (USB 2.0 9.4.3, 8.5.3.2, 5.5.3), requests the
 * device does not support (9.2.7, 9.4), the routing of vendor requests to
 * the app (class requests are the audio function's, test_dev_clock),
 * deferred replies, a new SETUP cancelling a pending reply, a data stage
 * shorter than wLength (9.3.5), and the app's results outside the listed
 * ones; also against the library without the contract checks
 * (test_dev_ctrl_unchecked).
 */
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "dev_support.h"

/* GET_DESCRIPTOR(type, index) with wLength: the ep0_in call's length and
 * zlp, and what the host received */
static void check_in(uint8_t type, uint8_t index, uint16_t windex, uint16_t wlength,
                     uint16_t want_len, bool want_zlp, int line)
{
  uint8_t buf[600];
  uint16_t n = 0xFFFF;

  u2ps_fake_clear_log();
  u2ps_fake_host_result_t r = get_descriptor(type, index, windex, wlength, buf, &n);
  int i = u2ps_fake_find(U2PS_FAKE_EP0_IN, 0);
  if (r != U2PS_FAKE_HOST_OK || i < 0 || u2ps_fake_calls[i].len != want_len ||
      u2ps_fake_calls[i].zlp != want_zlp || n != want_len)
    printf("  from line %d: result %d, ep0_in len %d zlp %d, host got %u\n", line, (int)r,
           i >= 0 ? u2ps_fake_calls[i].len : -1, i >= 0 ? u2ps_fake_calls[i].zlp : -1, n);
  CHECK_EQ(r, U2PS_FAKE_HOST_OK);
  CHECK(i >= 0);
  if (i >= 0) {
    CHECK_EQ(u2ps_fake_calls[i].len, want_len);
    CHECK_EQ(u2ps_fake_calls[i].zlp, want_zlp);
  }
  CHECK_EQ(n, want_len);
}

#define CHECK_IN(type, index, windex, wlength, len, zlp) \
  check_in(type, index, windex, wlength, len, zlp, __LINE__)

static void test_short_and_zlp(void)
{
  dev_start(U2PS_SPEED_HIGH);

  /* Shorter than wLength, not a whole number of packets: the last packet is
   * short, no ZLP */
  CHECK_IN(U2PS_DESC_DEVICE, 0, 0, 64, 18, false);
  CHECK_IN(U2PS_DESC_DEVICE, 0, 0, 255, 18, false);
  CHECK_IN(U2PS_DESC_STRING, 1, TEST_LANGID, 255, 16, false);
  CHECK_IN(U2PS_DESC_CONFIGURATION, 0, 0, 0xFFFF, test_config_hs_len, false);
  /* Exactly wLength: no ZLP, also on a packet boundary */
  CHECK_IN(U2PS_DESC_DEVICE, 0, 0, 18, 18, false);
  CHECK_IN(U2PS_DESC_STRING, 2, TEST_LANGID, 64, 64, false);
  CHECK_IN(U2PS_DESC_STRING, 3, TEST_LANGID, 128, 128, false);
  CHECK_IN(U2PS_DESC_CONFIGURATION, 0, 0, test_config_hs_len, test_config_hs_len, false);
  /* Longer than wLength: the first wLength bytes */
  CHECK_IN(U2PS_DESC_DEVICE, 0, 0, 8, 8, false);
  CHECK_IN(U2PS_DESC_STRING, 3, TEST_LANGID, 100, 100, false);
  CHECK_IN(U2PS_DESC_STRING, 3, TEST_LANGID, 64, 64, false);
  CHECK_IN(U2PS_DESC_CONFIGURATION, 0, 0, 9, 9, false);
  CHECK_IN(U2PS_DESC_CONFIGURATION, 0, 0, 256, 256, false);
  /* Shorter than wLength and a whole number of packets: a ZLP ends it */
  CHECK_IN(U2PS_DESC_STRING, 2, TEST_LANGID, 65, 64, true);
  CHECK_IN(U2PS_DESC_STRING, 2, TEST_LANGID, 255, 64, true);
  CHECK_IN(U2PS_DESC_STRING, 3, TEST_LANGID, 255, 128, true);
  CHECK_IN(U2PS_DESC_STRING, 3, TEST_LANGID, 192, 128, true);

  /* wLength 0: no data stage, IN status */
  u2ps_fake_clear_log();
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 0, NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_IN), 0);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_STATUS, 0).arg, 1);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static uint8_t answer[256];
static int answer_len;

static int answer_in(const u2ps_request_t *req)
{
  memcpy(req->buf, answer, (size_t)answer_len);
  return answer_len;
}

static int defer(const u2ps_request_t *req)
{
  (void)req;
  return U2PS_REQ_DEFER;
}

static int accept(const u2ps_request_t *req)
{
  (void)req;
  return 0;
}

/* Vendor requests reach the app only addressed to an interface of the
 * current configuration, in the Configured state; class requests never */
static void test_routing(void)
{
  uint8_t buf[64];
  uint16_t n;
  static uint8_t data4[4] = {0x80, 0xBB, 0x00, 0x00};

  dev_start(U2PS_SPEED_HIGH);
  app_handler = accept;
  /* Not configured: request error, the app sees nothing */
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0x0100, 0x0400, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(set_address(5), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0x0100, 0x0400, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(app_log.requests, 0);

  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  /* Interfaces 0 and 1, both directions */
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0x0100, 0x0400, 0, NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x10, 0, 1, 0, NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.requests, 2);
  CHECK_EQ(app_log.last.size, U2PS_EP0_BUF);
  CHECK_EQ(app_log.last.setup.bmRequestType, 0x41);
  CHECK_EQ(app_log.last.setup.bRequest, 0x10);
  CHECK_EQ(app_log.last.setup.wIndex, 1);
  /* Host-to-device with data: the app sees it once, with the data */
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0x0100, 0x0400, 4, data4, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.requests, 3);
  CHECK_EQ(app_log.last.len, 4);
  CHECK(memcmp(app_log.last_data, data4, 4) == 0);
  /* Device-to-host: the app's bytes, at most wLength */
  app_handler = answer_in;
  answer_len = 4;
  memcpy(answer, data4, 4);
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0x0100, 0x0400, 4, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 4);
  CHECK(memcmp(buf, data4, 4) == 0);
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0x0100, 0x0400, 2, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 2);
  /* An empty answer is a single zero-length packet */
  answer_len = 0;
  u2ps_fake_clear_log();
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x20, 0, 0, 8, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 0);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_IN, 0).len, 0);
  /* The app's request error: in the data stage, or after the host's data in
   * the status stage */
  app_handler = NULL;
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x02, 0x0100, 0x0400, 14, buf, &n), U2PS_FAKE_HOST_STALL);
  u2ps_fake_clear_log();
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0x0100, 0x0400, 4, data4, &n), U2PS_FAKE_HOST_STALL);
  CHECK(u2ps_fake_find(U2PS_FAKE_EP0_OUT, 0) >= 0);

  /* Not to the app: device, endpoint and other recipients, interfaces that
   * do not exist, reserved type, more data than the control buffer holds */
  app_handler = accept;
  unsigned before = app_log.requests;
  CHECK_EQ(u2ps_fake_host_control(0x20, 0x01, 0, 0, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0xC0, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x22, 0x01, 0x0100, 0x01, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0xA2, 0x01, 0x0100, 0x81, 4, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x23, 0x01, 0, 0, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0x0100, 0x0402, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x61, 0x01, 0, 0, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  u2ps_fake_clear_log();
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0, 0, U2PS_EP0_BUF + 1u, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_OUT), 0);
  /* Class requests to either interface: the audio function's (an Entity
   * without Controls, an interface, the clock) */
  CHECK_EQ(u2ps_fake_host_control(0x21, 0x01, 0x0100, 0x0100, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0xA1, 0x01, 0x0100, 0x0300, 4, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0xA1, 0x01, 0x0100, 0x0001, 4, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x21, 0x01, 0x0100, 0x0400, 4, data4, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_host_control(0xA1, 0x01, 0x0100, 0x0400, 4, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 4);
  CHECK(memcmp(buf, data4, 4) == 0);
  CHECK_EQ(app_log.requests, before);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Standard requests and descriptors the device does not have: a request
 * error, and EP0 works again at the next SETUP (9.2.7, 9.4) */
static void test_unsupported(void)
{
  uint8_t buf[64];
  uint16_t n;

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();

  /* Reserved and undefined request codes */
  static const uint8_t reserved[] = {2, 4, 13, 0x30, 0xFF};
  for (unsigned i = 0; i < sizeof reserved; i++) {
    CHECK_EQ(u2ps_fake_host_control(0x00, reserved[i], 0, 0, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
    CHECK_EQ(u2ps_fake_host_control(0x80, reserved[i], 0, 0, 2, buf, &n), U2PS_FAKE_HOST_STALL);
  }
  /* SET_DESCRIPTOR: stalled at the SETUP, no data taken */
  u2ps_fake_clear_log();
  CHECK_EQ(u2ps_fake_host_control(0x00, U2PS_SET_DESCRIPTOR, 0x0100, 0, 18, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_OUT), 0);
  /* SYNCH_FRAME on the data endpoint */
  CHECK_EQ(u2ps_fake_host_control(0x82, U2PS_SYNCH_FRAME, 0, 0x01, 2, buf, &n), U2PS_FAKE_HOST_STALL);
  /* Standard requests with the wrong direction or recipient */
  CHECK_EQ(u2ps_fake_host_control(0x00, U2PS_GET_DESCRIPTOR, 0x0100, 0, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_DESCRIPTOR, 0x0100, 0, 18, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_SET_CONFIGURATION, 1, 0, 1, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_CONFIGURATION, 1, 0, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_CONFIGURATION, 0, 0, 1, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_GET_INTERFACE, 0, 0, 1, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x00, U2PS_SET_INTERFACE, 0, 0, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  /* Descriptors: types that GET_DESCRIPTOR does not serve, indices beyond
   * the last, and a string in a language the device does not list */
  static const uint8_t types[] = {0, U2PS_DESC_INTERFACE, U2PS_DESC_ENDPOINT, 8, 0x0F,
                                  0x21, 0x24, 0xFF};
  for (unsigned i = 0; i < sizeof types; i++)
    CHECK_EQ(get_descriptor(types[i], 0, 0, 64, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 1, 0, 18, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_CONFIGURATION, 1, 0, 9, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE_QUALIFIER, 1, 0, 10, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_OTHER_SPEED, 1, 0, 9, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 4, TEST_LANGID, 255, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 1, 0x0407, 255, buf, &n), U2PS_FAKE_HOST_STALL);

  /* After all that, EP0 still works */
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 18, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 18);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Deferred replies from thread, in all three directions */
static void test_deferred(void)
{
  uint8_t buf[256];
  uint16_t n;
  static uint8_t data4[4] = {0x00, 0x77, 0x01, 0x00};

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  app_handler = defer;

  /* Device-to-host: EP0 NAKs until the reply */
  u2ps_fake_clear_log();
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0x0100, 0x0400, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_fake_ncalls, 0);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_NAK);
  uint32_t id = app_log.last.id;
  CHECK_EQ(u2ps_dev_reply(id, data4, 4), true);
  /* The reply copies its data with the vectors masked, then arms the data
   * stage in a masked region of its own */
  CHECK_EQ(u2ps_fake_ncalls, 5);
  CHECK_EQ(u2ps_fake_calls[0].kind, U2PS_FAKE_IRQ_MASK);
  CHECK_EQ(u2ps_fake_calls[0].arg, 1);
  CHECK_EQ(u2ps_fake_calls[1].kind, U2PS_FAKE_IRQ_MASK);
  CHECK_EQ(u2ps_fake_calls[1].arg, 0);
  CHECK_EQ(u2ps_fake_calls[2].kind, U2PS_FAKE_IRQ_MASK);
  CHECK_EQ(u2ps_fake_calls[2].arg, 1);
  CHECK_EQ(u2ps_fake_calls[3].kind, U2PS_FAKE_EP0_IN);
  CHECK_EQ(u2ps_fake_calls[3].from, U2PS_FAKE_FROM_MASKED);
  CHECK_EQ(u2ps_fake_calls[3].len, 4);
  CHECK_EQ(u2ps_fake_calls[4].kind, U2PS_FAKE_IRQ_MASK);
  CHECK_EQ(u2ps_fake_calls[4].arg, 0);
  /* The stack copied the reply: the app's memory is its own again */
  uint8_t sent4[4];
  memcpy(sent4, data4, 4);
  memset(data4, 0xEE, 4);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 4);
  CHECK(memcmp(buf, sent4, 4) == 0);
  memcpy(data4, sent4, 4);
  /* A second reply to the same request is dropped */
  u2ps_fake_clear_log();
  CHECK_EQ(u2ps_dev_reply(id, data4, 4), false);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_IN), 0);

  /* A deferred answer of whole packets, shorter than wLength, ends with a
   * ZLP like any other */
  memset(answer, 0x5A, 128);
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x02, 0x0100, 0x0400, 200, buf, &n), U2PS_FAKE_HOST_NAK);
  u2ps_fake_clear_log();
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, answer, 128), true);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_IN, 0).zlp, true);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 128);
  /* More than wLength is cut to wLength */
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x02, 0x0100, 0x0400, 6, buf, &n), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, answer, 128), true);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 6);

  /* Host-to-device: the data stage completes, the status stage NAKs until
   * the reply */
  u2ps_fake_clear_log();
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0x0100, 0x0400, 4, data4, &n), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(app_log.last.len, 4);
  CHECK(memcmp(app_log.last_data, data4, 4) == 0);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_STATUS), 0);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, 0), true);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_STATUS, 0).from, U2PS_FAKE_FROM_MASKED);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);

  /* No data, answered with a request error */
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x33, 0, 1, 0, NULL, NULL), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, U2PS_REQ_STALL), true);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_STALL);
  /* A reply cannot defer again: that is a request error (and a contract
   * violation) */
  contract_expect(true);
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x33, 0, 1, 0, NULL, NULL), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, U2PS_REQ_DEFER), true);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_STALL);
  /* Data promised but none given: a request error (and one) */
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0x0100, 0x0400, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, NULL, 4), true);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_DEV_REPLY_RESULT], CONTRACT_N(1));
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_DEV_REPLY_DATA], CONTRACT_N(1));
  CHECK_EQ(contract_log.total, CONTRACT_N(2));
  contract_expect(false);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A new SETUP cancels a pending reply; the late reply is dropped and leaves
 * the new request's data alone */
static void test_cancel(void)
{
  uint8_t buf[256];
  uint16_t n;
  static const uint8_t junk[64] = {0xEE};
  static const uint8_t good[4] = {1, 2, 3, 4};

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();

  /* A is pending; the host gives up and reads the device descriptor */
  app_handler = defer;
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0x0100, 0x0400, 64, buf, &n), U2PS_FAKE_HOST_NAK);
  uint32_t a = app_log.last.id;
  u2ps_fake_setup(0x80, U2PS_GET_DESCRIPTOR, U2PS_DESC_DEVICE << 8, 0, 18);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_IN_DATA);
  /* A's reply comes while the descriptor's data stage is armed */
  u2ps_fake_clear_log();
  CHECK_EQ(u2ps_dev_reply(a, junk, 64), false);
  CHECK_EQ(u2ps_fake_ncalls, 2); /* the mask, nothing else */
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 18);
  CHECK(memcmp(buf, test_device, 18) == 0);

  /* B, then C: C cancels B, and B's late reply changes nothing */
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0x0100, 0x0400, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  uint32_t b = app_log.last.id;
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0x0100, 0x0400, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  uint32_t c = app_log.last.id;
  CHECK(b != c);
  CHECK_EQ(u2ps_dev_reply(b, junk, 4), false);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_NAK);
  CHECK_EQ(u2ps_dev_reply(c, good, 4), true);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 4);
  CHECK(memcmp(buf, good, 4) == 0);

  /* A host-to-device request cancelled during its data stage: its data
   * never reaches the app */
  unsigned before = app_log.requests;
  u2ps_fake_setup(0x41, 0x01, 0x0100, 0x0400, 4);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_OUT_DATA);
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 18, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.requests, before);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A SETUP arrives while the thread holds the vectors masked to arm the
 * reply to the request it replaces. The stack still finds the old request
 * pending, but the port arms nothing while it holds the SETUP
 * (u2ps_dport.h), so the stale reply never reaches the host. A class
 * request's sequence, with a vendor request in its place:
 * c1 01 00 01 00 04 04 00 deferred, then 80 06 00 01 00 00 12 00. A reply
 * with data copies it in a masked region before the one that arms, so the
 * SETUP comes at the second mask there; a reply without data has one. */
static void test_held_setup(void)
{
  uint8_t buf[64];
  uint16_t n;
  static const uint8_t stale[4] = {0xDE, 0xAD, 0xBE, 0xEF};

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  app_handler = defer;

  /* Device-to-host: the stale data stage */
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0x0100, 0x0400, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  uint32_t a = app_log.last.id;
  u2ps_fake_clear_log();
  u2ps_fake_setup_on_mask_nth(2, 0x80, U2PS_GET_DESCRIPTOR, U2PS_DESC_DEVICE << 8, 0, 18);
  u2ps_dev_reply(a, stale, 4);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_IN, 0).dropped, true);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_IN, 0).len, 4);
  /* the held SETUP was delivered at the unmask and armed its own stage */
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_IN_DATA);
  memset(buf, 0, sizeof buf);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 18);
  CHECK(memcmp(buf, test_device, 18) == 0);
  CHECK(memcmp(buf, stale, 4) != 0);
  /* the late reply cannot come again */
  CHECK_EQ(u2ps_dev_reply(a, stale, 4), false);

  /* Host-to-device: the stale status stage would end the new request's
   * control write before its data */
  static uint8_t cur4[4] = {0x00, 0x77, 0x01, 0x00};
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0x0100, 0x0400, 4, cur4, &n), U2PS_FAKE_HOST_NAK);
  uint32_t b = app_log.last.id;
  app_handler = accept;
  u2ps_fake_clear_log();
  u2ps_fake_setup_on_mask(0x41, 0x01, 0x0100, 0x0400, 4);
  u2ps_dev_reply(b, NULL, 0);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_STATUS, 0).dropped, true);
  CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_OUT_DATA);
  CHECK_EQ(u2ps_fake_host_resume(cur4, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.last.len, 4);
  CHECK(app_log.last.id != b);

  /* A stale request error: the new request must not be stalled */
  app_handler = defer;
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x33, 0, 1, 0, NULL, NULL), U2PS_FAKE_HOST_NAK);
  uint32_t c = app_log.last.id;
  u2ps_fake_setup_on_mask(0x80, U2PS_GET_DESCRIPTOR, U2PS_DESC_DEVICE << 8, 0, 18);
  u2ps_dev_reply(c, NULL, U2PS_REQ_STALL);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 18);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A long deferred reply goes into the control buffer in pieces, each copied
 * with the vectors masked and only while the request is still pending;
 * the stage is armed in a masked region of its own. So no masked region
 * copies more than U2PS_EP0_MPS bytes, whatever the reply's size. A
 * SETUP between two pieces cancels the reply: it returns false, arms
 * nothing, and the new request's answer stays intact. */
static void test_reply_pieces(void)
{
  static uint8_t buf[U2PS_EP0_BUF];
  static uint8_t big[U2PS_EP0_BUF];
  uint16_t n;

  for (unsigned i = 0; i < sizeof big; i++)
    big[i] = (uint8_t)(i * 7u + 1u);
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  app_handler = defer;

  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0x0100, 0x0400, U2PS_EP0_BUF, buf, &n), U2PS_FAKE_HOST_NAK);
  u2ps_fake_clear_log();
  u2ps_fake_masked_bytes_max = 0;
  CHECK_EQ(u2ps_dev_reply(app_log.last.id, big, (int)sizeof big), true);
  CHECK(u2ps_fake_masked_bytes_max > 0u);
  CHECK(u2ps_fake_masked_bytes_max <= U2PS_EP0_MPS);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_IN), 1);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_EP0_IN, 0).len, U2PS_EP0_BUF);
  /* The arm comes in the last masked region, which copies nothing */
  int arm = u2ps_fake_find(U2PS_FAKE_EP0_IN, 0);
  CHECK(arm >= 2);
  if (arm >= 2) {
    CHECK_EQ(u2ps_fake_calls[arm - 1].kind, U2PS_FAKE_IRQ_MASK);
    CHECK_EQ(u2ps_fake_calls[arm - 1].arg, 1);
    CHECK_EQ(u2ps_fake_calls[arm - 2].kind, U2PS_FAKE_IRQ_MASK);
    CHECK_EQ(u2ps_fake_calls[arm - 2].arg, 0);
  }
  memset(buf, 0, sizeof buf);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, U2PS_EP0_BUF);
  CHECK(memcmp(buf, big, sizeof big) == 0);

  /* The SETUP of a descriptor request arrives at the second, third or
   * fourth mask, between two pieces */
  for (unsigned nth = 2; nth <= 4; nth++) {
    CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0x0100, 0x0400, U2PS_EP0_BUF, buf, &n), U2PS_FAKE_HOST_NAK);
    uint32_t a = app_log.last.id;
    u2ps_fake_clear_log();
    u2ps_fake_setup_on_mask_nth(nth, 0x80, U2PS_GET_DESCRIPTOR, U2PS_DESC_DEVICE << 8, 0, 18);
    CHECK_EQ(u2ps_dev_reply(a, big, (int)sizeof big), false);
    /* Nothing armed from thread; the new request armed its own stage */
    for (unsigned i = 0; i < u2ps_fake_ncalls; i++)
      CHECK(u2ps_fake_calls[i].kind != U2PS_FAKE_EP0_IN || u2ps_fake_calls[i].from == U2PS_FAKE_FROM_SETUP);
    CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_IN_DATA);
    memset(buf, 0, sizeof buf);
    CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
    CHECK_EQ(n, 18);
    CHECK(memcmp(buf, test_device, 18) == 0);
    CHECK_EQ(u2ps_dev_reply(a, big, (int)sizeof big), false);
  }
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A host-to-device data stage shorter than wLength: wLength is the exact
 * length of the host's data (9.3.5), so the request is an error (9.2.7):
 * a STALL, and the app never sees it. A whole data stage after it reaches
 * the app with len = wLength. */
static void test_short_data_stage(void)
{
  static uint8_t data8[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint16_t n;

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  app_handler = accept;
  static const uint16_t lens[] = {0, 1, 4, 7};
  for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++) {
    unsigned before = app_log.requests;
    u2ps_fake_clear_log();
    u2ps_fake_setup(0x41, 0x01, 0x0100, 0x0400, 8);
    CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_OUT_DATA);
    u2ps_fake_ep0_out_done(data8, lens[i]);
    if (app_log.requests != before || u2ps_fake_ep0 != U2PS_FAKE_EP0_STALLED)
      printf("  wLength 8, %u bytes: app requests %u -> %u, EP0 state %d\n", lens[i], before,
             app_log.requests, (int)u2ps_fake_ep0);
    CHECK_EQ(app_log.requests, before);
    CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_STALL), 1);
    CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_STATUS), 0);
    CHECK_EQ(u2ps_fake_ep0, U2PS_FAKE_EP0_STALLED);
  }
  unsigned before = app_log.requests;
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0x0100, 0x0400, 8, data8, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.requests, before + 1u);
  CHECK_EQ(app_log.last.len, 8);
  CHECK(memcmp(app_log.last_data, data8, 8) == 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* u2ps_app_on_request() returns a negative result other than STALL and
 * DEFER: a request error like STALL (and a contract violation in a build
 * with the checks), in each of the three directions; EP0 neither NAKs nor
 * holds the request, and the tick reports no fault after it */
static int app_result;

static int unlisted(const u2ps_request_t *req)
{
  (void)req;
  return app_result;
}

static void test_unlisted_result(void)
{
  uint8_t buf[64];
  static uint8_t data4[4] = {1, 2, 3, 4};
  uint16_t n;
  static const int results[] = {-3, -4, -100, -32768};

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  app_handler = unlisted;
  contract_expect(true);
  for (unsigned i = 0; i < sizeof results / sizeof results[0]; i++) {
    app_result = results[i];
    CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0x0100, 0x0400, 4, buf, &n), U2PS_FAKE_HOST_STALL);
    CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0x0100, 0x0400, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
    CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0x0100, 0x0400, 4, data4, &n), U2PS_FAKE_HOST_STALL);
  }
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_DEV_CALLBACK_RESULT], CONTRACT_N(12));
  CHECK_EQ(contract_log.total, CONTRACT_N(12));
  contract_expect(false);
  /* Nothing waits: no fault from the tick, well past U2PS_START_MAX */
  for (uint32_t t = 0; t <= 2u * U2PS_START_MAX; t += 5u) {
    u2ps_fake_clear_log();
    CHECK_EQ(u2ps_dev_tick(100000u + t), U2PS_FAULT_NONE);
  }
  app_handler = accept;
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x01, 0x0100, 0x0400, 0, NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_violations, 0);
}

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
  test_short_and_zlp();
  test_routing();
  test_unsupported();
  test_deferred();
  test_cancel();
  test_held_setup();
  test_reply_pieces();
  test_short_data_stage();
  test_unlisted_result();
  return check_done();
}
