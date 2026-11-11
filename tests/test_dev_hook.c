/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_hook.c: u2ps_app_on_control(), against the device library
 * built with U2PS_CONTROL_HOOK=1. Every SETUP the stack answers, standard,
 * class and vendor, reaches the hook once the stack has acted on it: with
 * the data stage's length for a device-to-host request (after the limit of
 * wLength), 0 for any other success, U2PS_REQ_STALL for a request error.
 * A host-to-device request with data is answered after its data stage, a
 * deferred one at u2ps_dev_reply(); a cancelled one gets no call. Called
 * in thread from u2ps_dev_reply(), the hook is a callback like the others:
 * it may release without masking again, and a thread call inside it is a
 * thread call inside a callback (the library also has the contract
 * checks).
 */
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "dev_support.h"

#if !U2PS_CONTROL_HOOK
#error "test_dev_hook: link the library built with U2PS_CONTROL_HOOK=1"
#endif

static struct {
  unsigned calls;
  u2ps_setup_t setup;
  int result;
} hook;

/* What the hook does besides its log, once (NULL: nothing) */
static void (*hook_action)(void);

void u2ps_app_on_control(const u2ps_setup_t *setup, int result)
{
  hook.calls++;
  hook.setup = *setup;
  hook.result = result;
  if (hook_action != NULL) {
    void (*f)(void) = hook_action;
    hook_action = NULL;
    f();
  }
}

static uint8_t buf[512];
static uint16_t n;

static int defer_all(const u2ps_request_t *req)
{
  (void)req;
  return U2PS_REQ_DEFER;
}

static void test_results(void)
{
  dev_start(U2PS_SPEED_HIGH);
  memset(&hook, 0, sizeof hook);

  /* Device-to-host: the length sent, limited by wLength */
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 64, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(hook.calls, 1);
  CHECK_EQ(hook.setup.bRequest, U2PS_GET_DESCRIPTOR);
  CHECK_EQ(hook.setup.wValue, U2PS_DESC_DEVICE << 8);
  CHECK_EQ(hook.setup.wLength, 64);
  CHECK_EQ(hook.result, 18);
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 8, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(hook.calls, 2);
  CHECK_EQ(hook.result, 8);
  /* No data: 0 */
  CHECK_EQ(set_address(5), U2PS_FAKE_HOST_OK);
  CHECK_EQ(hook.calls, 3);
  CHECK_EQ(hook.setup.bRequest, U2PS_SET_ADDRESS);
  CHECK_EQ(hook.result, 0);
  /* A request error, standard */
  CHECK_EQ(get_descriptor(0x7F, 0, 0, 8, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(hook.calls, 4);
  CHECK_EQ(hook.result, U2PS_REQ_STALL);
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(hook.calls, 5);
  CHECK_EQ(hook.result, 0);

  /* Class requests: one the stack answers, one it refuses (entity 3 has no
   * Controls) */
  uint32_t rate;
  CHECK_EQ(get_rate(&rate), U2PS_FAKE_HOST_OK);
  CHECK_EQ(hook.calls, 6);
  CHECK_EQ(hook.setup.bmRequestType, 0xA1);
  CHECK_EQ(hook.result, 4);
  CHECK_EQ(u2ps_fake_host_control(0xA1, 0x01, 0x0100, 0x0300, 4, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(hook.calls, 7);
  CHECK_EQ(hook.setup.wIndex, 0x0300);
  CHECK_EQ(hook.result, U2PS_REQ_STALL);

  /* Host-to-device with data: answered after the data stage */
  uint8_t b[4] = {0x80, 0xBB, 0x00, 0x00};
  u2ps_fake_setup(0x21, 0x01, 0x0100, 0x0400, 4);
  CHECK_EQ(hook.calls, 7);
  u2ps_fake_ep0_out_done(b, 4);
  CHECK_EQ(hook.calls, 8);
  CHECK_EQ(hook.setup.bRequest, 0x01);
  CHECK_EQ(hook.result, 0);
  u2ps_fake_ep0_status_done();
  /* Too long for the control buffer: refused at the SETUP */
  CHECK_EQ(u2ps_fake_host_control(0x21, 0x01, 0x0100, 0x0400, U2PS_EP0_BUF + 1u, buf, &n),
           U2PS_FAKE_HOST_STALL);
  CHECK_EQ(hook.calls, 9);
  CHECK_EQ(hook.result, U2PS_REQ_STALL);
  /* Vendor, refused by the app */
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x05, 0, 0, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(hook.calls, 10);
  CHECK_EQ(hook.setup.bmRequestType, 0x41);
  CHECK_EQ(hook.result, U2PS_REQ_STALL);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A deferred request is reported at its reply, a cancelled one never */
static void test_deferred(void)
{
  static const uint8_t answer[6] = {1, 2, 3, 4, 5, 6};

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  app_handler = defer_all;
  memset(&hook, 0, sizeof hook);
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(hook.calls, 0);
  uint32_t id = app_log.last.id;
  CHECK(u2ps_dev_reply(id, answer, (int)sizeof answer));
  CHECK_EQ(hook.calls, 1);
  CHECK_EQ(hook.result, 4);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);

  /* Cancelled by the next SETUP: only that one is reported */
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  id = app_log.last.id;
  app_handler = NULL;
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_GET_CONFIGURATION, 0, 0, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(hook.calls, 2);
  CHECK_EQ(hook.setup.bRequest, U2PS_GET_CONFIGURATION);
  CHECK_EQ(hook.result, 1);
  CHECK(!u2ps_dev_reply(id, answer, (int)sizeof answer));
  CHECK_EQ(hook.calls, 2);
  /* A deferred stall */
  app_handler = defer_all;
  CHECK_EQ(u2ps_fake_host_control(0x41, 0x07, 0, 0, 0, NULL, NULL), U2PS_FAKE_HOST_NAK);
  CHECK(u2ps_dev_reply(app_log.last.id, NULL, U2PS_REQ_STALL));
  CHECK_EQ(hook.calls, 3);
  CHECK_EQ(hook.result, U2PS_REQ_STALL);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static void release_none(void)
{
  u2ps_dev_release(app_log.info.stream, 0);
}

static void stats_now(void)
{
  u2ps_dev_stats(&(u2ps_dev_stats_t){0});
}

/* The hook in thread, from u2ps_dev_reply(), with the vectors masked */
static void test_in_thread(void)
{
  static const uint8_t answer[4] = {1, 2, 3, 4};

  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  app_handler = defer_all;
  /* A release inside it masks nothing more */
  contract_expect(true);
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  hook_action = release_none;
  CHECK(u2ps_dev_reply(app_log.last.id, answer, 4));
  CHECK(hook_action == NULL);
  CHECK_EQ(u2ps_fake_host_resume(buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_violations, 0);
  CHECK_EQ(contract_log.total, 0);
  /* A thread call inside it is one inside a callback */
  CHECK_EQ(u2ps_fake_host_control(0xC1, 0x01, 0, 0, 4, buf, &n), U2PS_FAKE_HOST_NAK);
  hook_action = stats_now;
  CHECK(u2ps_dev_reply(app_log.last.id, answer, 4));
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_DEV_CALL_IN_CALLBACK], 1);
  CHECK_EQ(contract_log.total, 1);
  contract_expect(false);
}

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
  test_results();
  test_deferred();
  test_in_thread();
  return check_done();
}
