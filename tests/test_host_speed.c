/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_host_speed.c: full- and low-speed devices (USB 2.0 7.1.2.3: a
 * host's port supports all three speeds), built twice: against the
 * default library, and against the one built with U2PS_HOST_REFUSE_FS=1.
 * The default:
 * - an FS or LS device is identified: GET_DESCRIPTOR(device, 8) with an
 *   mps0 of 8 at address 0 (USB 2.0 5.5.3), then every request with its
 *   bMaxPacketSize0, the device descriptor and the configurations; a
 *   UAC2 device then ends with SPEED, one without audio with
 *   NOT_PLAYABLE; never SET_CONFIGURATION, a string or a stream;
 * - with no playable configuration, the device_qualifier (9.6.2): a STALL
 *   (a full-speed-only device) or a malformed one gives NOT_PLAYABLE with
 *   the first configuration's refusal; a valid one, each other-speed
 *   configuration (9.6.4, head then whole) up to its bNumConfigurations:
 *   one playable gives SPEED, none NOT_PLAYABLE with the first full-speed
 *   configuration's refusal; a too-long one TOO_LARGE, a malformed head
 *   DESCRIPTOR; a timeout TIMEOUT, errors ERROR, a STALL of an
 *   other-speed request STALL; a whole descriptor of the other type is
 *   no configuration; a replug starts the reading afresh;
 * - bMaxPacketSize0 8, 16, 32, 64 at FS, 8 at LS; others EP0_MPS;
 * - a silent FS device: TIMEOUT; one that answers with errors: ERROR;
 * - the test device at FS runs its sequence;
 * - high speed is unchanged.
 * With U2PS_HOST_REFUSE_FS=1: every FS and LS device, the test device
 * too, ends with SPEED at once, before any request.
 * The fake port's own checks at low speed: a control transfer with an
 * mps0 other than 8, and a stream, are port violations.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "host_support.h"
#include "u2ps_ch9.h"

enum { R_DEVICE8, R_ADDRESS, R_DEVICE, R_CONFIG9, R_CONFIG };

/* No device_qualifier request (check_identified) */
#define NO_QUALIFIER (-1)

/* The walk's refusal of the u2ps device's full-speed configuration (an
 * AudioControl interface alone) */
#define FS_MINIMAL_REFUSAL U2PS_HOST_REFUSAL_NO_TERMINAL

static const u2ps_hfake_call_t *req(unsigned nth)
{
  static const u2ps_hfake_call_t none = {.kind = U2PS_HFAKE_MISSING, .ms = 0xFFFFFFFFu,
                                         .done_ms = 0xFFFFFFFFu};
  const u2ps_hfake_call_t *c = u2ps_hfake_control_nth(nth);
  return c != NULL ? c : &none;
}

static bool decided(void)
{
  return happ_log.test_devices > 0u || happ_log.ends > 0u || happ_log.devices > 0u;
}

/* The device model at 'speed' with bMaxPacketSize0 'mps0' and the
 * configuration of 'fixture', connected; stepped until it is decided */
static bool at_speed(const char *fixture, u2ps_speed_t speed, uint8_t mps0)
{
  if (!hstart(fixture))
    return false;
  u2ps_hfake_speed = speed;
  u2ps_hmodel.device[7] = mps0;
  u2ps_hfake_connect();
  return u2ps_hfake_wait(decided, U2PS_HOST_CONTROL_MAX + 2000u);
}

static void as_test_device(uint16_t pid)
{
  u2ps_hmodel.device[8] = 0x0A;
  u2ps_hmodel.device[9] = 0x1A;
  u2ps_hmodel.device[10] = (uint8_t)pid;
  u2ps_hmodel.device[11] = (uint8_t)(pid >> 8);
}

/* One on_end 'why', in an upcall, the port disabled, nothing after it */
static void check_end(u2ps_host_end_t why, u2ps_host_refusal_t refusal)
{
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, why);
  CHECK_EQ(happ_log.refusal, refusal);
  CHECK_EQ(happ_log.devices, 0);
  CHECK_EQ(happ_log.test_devices, 0);
  CHECK_EQ(happ_log.end_from, U2PS_HFAKE_FROM_UPCALL);
  unsigned at = happ_log.calls_at_end;
  CHECK_EQ(u2ps_hfake_get(U2PS_HFAKE_DISABLE, at).kind, U2PS_HFAKE_DISABLE);
  u2ps_hfake_run_until(u2ps_hfake_now + 6000u);
  CHECK_EQ(u2ps_hfake_ncalls, at + 1u);
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(u2ps_hmodel.configuration, 0);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_STREAM_OPEN), 0);
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_hmodel.unexpected, 0);
}

/* The identification's requests: the first 8 bytes of the device
 * descriptor at address 0 with mps0 8, then SET_ADDRESS, the device
 * descriptor and 'configs' configurations (head and whole), each with the
 * device's mps0; then, unless 'others' is NO_QUALIFIER, the
 * device_qualifier and 'others' other-speed configurations (head and
 * whole, the whole one's wLength its wTotalLength); nothing else */
static void check_identified(uint8_t mps0, unsigned configs, int others)
{
  unsigned q = R_CONFIG9 + 2u * configs;
  unsigned n = others == NO_QUALIFIER ? q : q + 1u + 2u * (unsigned)others;
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), n);
  const u2ps_hfake_call_t *c = req(R_DEVICE8);
  CHECK_EQ(c->arg, 0);
  CHECK_EQ(c->mps0, 8);
  CHECK_EQ(c->setup.bRequest, U2PS_GET_DESCRIPTOR);
  CHECK_EQ(c->setup.wValue, 0x0100);
  CHECK_EQ(c->setup.wLength, 8);
  CHECK_EQ(req(R_ADDRESS)->setup.bRequest, U2PS_SET_ADDRESS);
  CHECK_EQ(req(R_DEVICE)->setup.wLength, 18);
  for (unsigned k = R_ADDRESS; k < u2ps_hfake_count(U2PS_HFAKE_CONTROL); k++) {
    CHECK_EQ(req(k)->mps0, mps0);
    CHECK(req(k)->setup.bRequest != U2PS_SET_CONFIGURATION);
    CHECK((req(k)->setup.wValue >> 8) != U2PS_DESC_STRING);
  }
  for (unsigned k = 0; k < configs; k++) {
    CHECK_EQ(req(R_CONFIG9 + 2u * k)->setup.wValue, 0x0200u | k);
    CHECK_EQ(req(R_CONFIG9 + 2u * k)->setup.wLength, 9);
    CHECK_EQ(req(R_CONFIG + 2u * k)->setup.wValue, 0x0200u | k);
  }
  if (others == NO_QUALIFIER)
    return;
  const u2ps_hfake_call_t *c6 = req(q);
  CHECK_EQ(c6->setup.bmRequestType, 0x80);
  CHECK_EQ(c6->setup.bRequest, U2PS_GET_DESCRIPTOR);
  CHECK_EQ(c6->setup.wValue, U2PS_DESC_DEVICE_QUALIFIER << 8);
  CHECK_EQ(c6->setup.wIndex, 0);
  CHECK_EQ(c6->setup.wLength, U2PS_QUALIFIER_LEN);
  for (unsigned k = 0; k < (unsigned)others; k++) {
    const u2ps_hfake_call_t *h = req(q + 1u + 2u * k), *w = req(q + 2u + 2u * k);
    CHECK_EQ(h->setup.bRequest, U2PS_GET_DESCRIPTOR);
    CHECK_EQ(h->setup.wValue, U2PS_DESC_OTHER_SPEED << 8 | k);
    CHECK_EQ(h->setup.wLength, U2PS_CONFIG_LEN);
    CHECK_EQ(w->setup.bRequest, U2PS_GET_DESCRIPTOR);
    CHECK_EQ(w->setup.wValue, U2PS_DESC_OTHER_SPEED << 8 | k);
    CHECK_EQ(w->setup.wLength, u2ps_hmodel.other[k][2] | u2ps_hmodel.other[k][3] << 8);
  }
}

/* With U2PS_HOST_REFUSE_FS=1: SPEED at once, no request */
static void check_refused(void)
{
  check_end(U2PS_HOST_END_SPEED, U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), 0);
}

/* ---- Identification ------------------------------------------------------------- */

static void test_full_speed_audio(void)
{
  static const uint8_t mps[4] = {8, 16, 32, 64};
  for (unsigned k = 0; k < 4u; k++) {
    if (!at_speed("uac2-hs-default.txt", U2PS_SPEED_FULL, mps[k]))
      return;
    if (U2PS_HOST_REFUSE_FS) {
      check_refused();
      continue;
    }
    check_identified(mps[k], 1, NO_QUALIFIER);
    check_end(U2PS_HOST_END_SPEED, U2PS_HOST_REFUSAL_NONE);
  }
  /* The first playable configuration ends it: the second is not read, nor
   * the device_qualifier the device has */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel_set_config(1, u2ps_hmodel.config[0], u2ps_hmodel.config_len[0]);
  u2ps_hmodel.device[17] = 2;
  u2ps_hmodel_set_qualifier(1);
  CHECK(hmodel_load_other(0, "uac2-hs-default.txt"));
  u2ps_hfake_speed = U2PS_SPEED_FULL;
  u2ps_hfake_connect();
  CHECK(u2ps_hfake_wait(decided, 2000));
  if (U2PS_HOST_REFUSE_FS) {
    check_refused();
  } else {
    check_identified(64, 1, NO_QUALIFIER);
    check_end(U2PS_HOST_END_SPEED, U2PS_HOST_REFUSAL_NONE);
  }
}

static void test_full_speed_other(void)
{
  /* No audio function, no device_qualifier (a STALL, as from a
   * full-speed-only device): NOT_PLAYABLE with the refusal */
  if (!at_speed("testdev.txt", U2PS_SPEED_FULL, 64))
    return;
  if (U2PS_HOST_REFUSE_FS) {
    check_refused();
  } else {
    check_identified(64, 1, 0);
    check_end(U2PS_HOST_END_NOT_PLAYABLE, U2PS_HOST_REFUSAL_NOT_AUDIO);
  }
  /* A u2ps device's full-speed configuration: no streaming interface */
  if (!at_speed("uac2-fs-minimal.txt", U2PS_SPEED_FULL, 64))
    return;
  if (U2PS_HOST_REFUSE_FS) {
    check_refused();
  } else {
    check_identified(64, 1, 0);
    check_end(U2PS_HOST_END_NOT_PLAYABLE, FS_MINIMAL_REFUSAL);
  }
  /* Two configurations without audio: both read */
  if (!hstart("testdev.txt"))
    return;
  u2ps_hmodel_set_config(1, u2ps_hmodel.config[0], u2ps_hmodel.config_len[0]);
  u2ps_hmodel.device[17] = 2;
  u2ps_hfake_speed = U2PS_SPEED_FULL;
  u2ps_hfake_connect();
  CHECK(u2ps_hfake_wait(decided, 2000));
  if (U2PS_HOST_REFUSE_FS) {
    check_refused();
  } else {
    check_identified(64, 2, 0);
    check_end(U2PS_HOST_END_NOT_PLAYABLE, U2PS_HOST_REFUSAL_NOT_AUDIO);
  }
}

static void test_low_speed(void)
{
  if (!at_speed("uac2-hs-default.txt", U2PS_SPEED_LOW, 8))
    return;
  if (U2PS_HOST_REFUSE_FS) {
    check_refused();
  } else {
    check_identified(8, 1, NO_QUALIFIER);
    check_end(U2PS_HOST_END_SPEED, U2PS_HOST_REFUSAL_NONE);
  }
  /* No audio: the device_qualifier stalls, as a low-speed device's must
   * (9.6.2) */
  if (!at_speed("testdev.txt", U2PS_SPEED_LOW, 8))
    return;
  if (U2PS_HOST_REFUSE_FS) {
    check_refused();
  } else {
    check_identified(8, 1, 0);
    check_end(U2PS_HOST_END_NOT_PLAYABLE, U2PS_HOST_REFUSAL_NOT_AUDIO);
  }
}

/* ---- The device_qualifier and the other-speed configurations ------------------------ */

/* A device at full speed whose configuration is 'fs'; with a
 * device_qualifier of n other-speed configurations 'other' (n 0: none, the
 * request stalls); stepped until it is decided */
static bool hs_capable(const char *fs, unsigned n, const char *const *other)
{
  if (!hstart(fs))
    return false;
  if (n > 0u)
    u2ps_hmodel_set_qualifier((uint8_t)n);
  for (unsigned k = 0; k < n; k++)
    if (!hmodel_load_other((uint8_t)k, other[k]))
      return false;
  u2ps_hfake_speed = U2PS_SPEED_FULL;
  u2ps_hfake_connect();
  return u2ps_hfake_wait(decided, U2PS_HOST_CONTROL_MAX + 2000u);
}

static void test_qualifier(void)
{
  static const char *const hs[1] = {"uac2-hs-default.txt"};
  static const char *const none[1] = {"testdev.txt"};
  static const char *const second[2] = {"testdev.txt", "uac2-hs-default.txt"};
  static const char *const ac_only[1] = {"uac2-fs-minimal.txt"};
  /* A high-speed DAC at full speed: its full-speed configuration holds
   * the AudioControl interface alone, its other-speed one plays */
  if (!hs_capable("uac2-fs-minimal.txt", 1, hs))
    return;
  if (U2PS_HOST_REFUSE_FS) {
    check_refused();
    return;
  }
  check_identified(64, 1, 1);
  check_end(U2PS_HOST_END_SPEED, U2PS_HOST_REFUSAL_NONE);
  /* Again after a replug, without a re-init: the session starts afresh */
  u2ps_hfake_disconnect();
  u2ps_hmodel.address = 0;
  u2ps_hmodel.qualified = false;
  happ_clear();
  u2ps_hfake_clear_log();
  u2ps_hfake_connect();
  CHECK(u2ps_hfake_wait(decided, 2000));
  check_identified(64, 1, 1);
  check_end(U2PS_HOST_END_SPEED, U2PS_HOST_REFUSAL_NONE);
  /* The same device without a device_qualifier */
  if (!hs_capable("uac2-fs-minimal.txt", 0, NULL))
    return;
  check_identified(64, 1, 0);
  check_end(U2PS_HOST_END_NOT_PLAYABLE, FS_MINIMAL_REFUSAL);
  /* A high-speed device without audio: the refusal of its first
   * full-speed configuration, not of its other-speed one */
  if (!hs_capable("testdev.txt", 1, none))
    return;
  check_identified(64, 1, 1);
  check_end(U2PS_HOST_END_NOT_PLAYABLE, U2PS_HOST_REFUSAL_NOT_AUDIO);
  if (!hs_capable("testdev.txt", 1, ac_only))
    return;
  check_identified(64, 1, 1);
  check_end(U2PS_HOST_END_NOT_PLAYABLE, U2PS_HOST_REFUSAL_NOT_AUDIO);
  if (!hs_capable("uac2-fs-minimal.txt", 1, none))
    return;
  check_identified(64, 1, 1);
  check_end(U2PS_HOST_END_NOT_PLAYABLE, FS_MINIMAL_REFUSAL);
  /* Two other-speed configurations: the second plays */
  if (!hs_capable("uac2-fs-minimal.txt", 2, second))
    return;
  check_identified(64, 1, 2);
  check_end(U2PS_HOST_END_SPEED, U2PS_HOST_REFUSAL_NONE);
}

/* The model, but the whole descriptor of type 'swap' read comes with the
 * other type of the two (configuration, other-speed configuration) */
static uint8_t swap;

static u2ps_hfake_answer_t swapped(uint8_t addr, const u2ps_hfake_setup_t *s, uint8_t *data,
                                   uint16_t *len)
{
  u2ps_hfake_answer_t a = u2ps_hmodel_answer(addr, s, data, len);
  if (a == U2PS_HFAKE_ANSWER_OK && s->wValue >> 8 == swap && s->wLength > U2PS_CONFIG_LEN &&
      *len >= 2u)
    data[1] = swap == U2PS_DESC_OTHER_SPEED ? U2PS_DESC_CONFIGURATION : U2PS_DESC_OTHER_SPEED;
  return a;
}

/* A whole descriptor of the other type, its head right: not a
 * configuration (NOT_CONFIG), at either speed */
static void test_type_of_whole(void)
{
  static const char *const hs[1] = {"uac2-hs-default.txt"};
  if (!hstart("uac2-fs-minimal.txt"))
    return;
  u2ps_hmodel_set_qualifier(1);
  CHECK(hmodel_load_other(0, hs[0]));
  swap = U2PS_DESC_OTHER_SPEED;
  u2ps_hfake_device = swapped;
  u2ps_hfake_speed = U2PS_SPEED_FULL;
  u2ps_hfake_connect();
  CHECK(u2ps_hfake_wait(decided, 2000));
  if (U2PS_HOST_REFUSE_FS) {
    check_refused();
  } else {
    check_identified(64, 1, 1);
    check_end(U2PS_HOST_END_NOT_PLAYABLE, FS_MINIMAL_REFUSAL);
  }
  if (!hstart("uac2-hs-default.txt"))
    return;
  swap = U2PS_DESC_CONFIGURATION;
  u2ps_hfake_device = swapped;
  u2ps_hfake_connect();
  CHECK(u2ps_hfake_wait(decided, 2000));
  check_end(U2PS_HOST_END_NOT_PLAYABLE, U2PS_HOST_REFUSAL_NOT_CONFIG);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), R_CONFIG + 1u);
}

/* A malformed device_qualifier is as none: fewer than 10 bytes, another
 * bDescriptorType, bLength below 10, no other-speed configuration */
static void test_qualifier_malformed(void)
{
  static const char *const hs[1] = {"uac2-hs-default.txt"};
  for (unsigned k = 0; k < 4u; k++) {
    if (!hstart("uac2-fs-minimal.txt"))
      return;
    u2ps_hmodel_set_qualifier(1);
    CHECK(hmodel_load_other(0, hs[0]));
    if (k == 0u)
      u2ps_hmodel.qualifier_len = 9;
    else if (k == 1u)
      u2ps_hmodel.qualifier[1] = U2PS_DESC_DEVICE;
    else if (k == 2u)
      u2ps_hmodel.qualifier[0] = 9;
    else
      u2ps_hmodel.qualifier[8] = 0;
    u2ps_hfake_speed = U2PS_SPEED_FULL;
    u2ps_hfake_connect();
    CHECK(u2ps_hfake_wait(decided, 2000));
    if (U2PS_HOST_REFUSE_FS) {
      check_refused();
      continue;
    }
    check_identified(64, 1, 0);
    check_end(U2PS_HOST_END_NOT_PLAYABLE, FS_MINIMAL_REFUSAL);
  }
}

/* The requests' failures: the device_qualifier times out (TIMEOUT) or
 * fails (ERROR); an other-speed configuration's head stalls (STALL), is
 * longer than U2PS_HOST_EP0_BUF (TOO_LARGE) or malformed (DESCRIPTOR) */
static void test_qualifier_failures(void)
{
  static const char *const hs[1] = {"uac2-hs-default.txt"};
  unsigned q = R_CONFIG9 + 2u; /* the device_qualifier's request */
  for (unsigned k = 0; k < 5u; k++) {
    if (!hstart("uac2-fs-minimal.txt"))
      return;
    u2ps_hmodel_set_qualifier(1);
    CHECK(hmodel_load_other(0, hs[0]));
    if (k == 0u)
      u2ps_hmodel.hang_at = (int)q;
    else if (k == 1u)
      u2ps_hmodel.error_at = (int)q;
    else if (k == 2u)
      u2ps_hmodel.stall_at = (int)q + 1;
    else if (k == 3u) {
      u2ps_hmodel.other[0][2] = (uint8_t)(U2PS_HOST_EP0_BUF + 1u);
      u2ps_hmodel.other[0][3] = (uint8_t)((U2PS_HOST_EP0_BUF + 1u) >> 8);
    } else
      u2ps_hmodel.other[0][1] = U2PS_DESC_CONFIGURATION;
    u2ps_hfake_speed = U2PS_SPEED_FULL;
    u2ps_hfake_connect();
    CHECK(u2ps_hfake_wait(decided, U2PS_HOST_CONTROL_MAX + 2000u));
    if (U2PS_HOST_REFUSE_FS) {
      check_refused();
      continue;
    }
    static const u2ps_host_end_t why[5] = {U2PS_HOST_END_TIMEOUT, U2PS_HOST_END_ERROR,
                                           U2PS_HOST_END_STALL, U2PS_HOST_END_TOO_LARGE,
                                           U2PS_HOST_END_DESCRIPTOR};
    CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), k < 2u ? q + 1u : q + 2u);
    CHECK_EQ(req(q)->setup.wValue, U2PS_DESC_DEVICE_QUALIFIER << 8);
    if (k >= 2u)
      CHECK_EQ(req(q + 1u)->setup.wValue, U2PS_DESC_OTHER_SPEED << 8);
    CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL_ABORT), k == 0u ? 1u : 0u);
    check_end(why[k], U2PS_HOST_REFUSAL_NONE);
  }
}

/* bMaxPacketSize0 the speed does not allow: EP0_MPS after the first
 * request */
static void test_ep0_sizes(void)
{
  static const struct {
    u2ps_speed_t speed;
    uint8_t mps0;
  } bad[] = {{U2PS_SPEED_FULL, 0},  {U2PS_SPEED_FULL, 4},  {U2PS_SPEED_FULL, 9},
             {U2PS_SPEED_FULL, 128}, {U2PS_SPEED_FULL, 255}, {U2PS_SPEED_LOW, 16},
             {U2PS_SPEED_LOW, 64},  {U2PS_SPEED_LOW, 0},  {U2PS_SPEED_HIGH, 8},
             {U2PS_SPEED_HIGH, 32}};
  for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
    if (!at_speed("uac2-hs-default.txt", bad[k].speed, bad[k].mps0))
      return;
    if (U2PS_HOST_REFUSE_FS && bad[k].speed != U2PS_SPEED_HIGH) {
      check_refused();
      continue;
    }
    check_end(U2PS_HOST_END_EP0_MPS, U2PS_HOST_REFUSAL_NONE);
    CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), 1);
  }
}

/* A silent FS device: TIMEOUT ("no response"); one whose transactions
 * fail: ERROR */
static void test_no_response(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hfake_speed = U2PS_SPEED_FULL;
  u2ps_hmodel.hang_at = 0;
  u2ps_hfake_connect();
  CHECK(u2ps_hfake_wait(decided, U2PS_HOST_CONTROL_MAX + 2000u));
  if (U2PS_HOST_REFUSE_FS) {
    check_refused();
  } else {
    CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL_ABORT), 1);
    check_end(U2PS_HOST_END_TIMEOUT, U2PS_HOST_REFUSAL_NONE);
  }
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hfake_speed = U2PS_SPEED_FULL;
  u2ps_hmodel.error_at = 0;
  u2ps_hfake_connect();
  CHECK(u2ps_hfake_wait(decided, 2000));
  if (U2PS_HOST_REFUSE_FS)
    check_refused();
  else
    check_end(U2PS_HOST_END_ERROR, U2PS_HOST_REFUSAL_NONE);
}

/* ---- The test device at full speed ------------------------------------------------ */

static void test_test_device(void)
{
  /* TTST_CONFIG: configured, and stays */
  if (!hstart("testdev.txt"))
    return;
  u2ps_hfake_speed = U2PS_SPEED_FULL;
  u2ps_hmodel.device[7] = 8;
  as_test_device(0x0200);
  u2ps_hfake_connect();
  CHECK(u2ps_hfake_wait(decided, 2000));
  if (U2PS_HOST_REFUSE_FS) {
    check_refused();
  } else {
    CHECK_EQ(happ_log.test_devices, 1);
    CHECK_EQ(happ_log.test_pid, 0x0200);
    CHECK_EQ(happ_log.ends, 0);
    CHECK_EQ(u2ps_hmodel.configuration, 1);
    const u2ps_hfake_call_t *sc = req(R_CONFIG + 1u);
    CHECK_EQ(sc->setup.bRequest, U2PS_SET_CONFIGURATION);
    CHECK_EQ(sc->mps0, 8);
    CHECK_EQ(req(R_DEVICE8)->setup.wLength, 8);
    unsigned n = u2ps_hfake_ncalls;
    u2ps_hfake_run_until(u2ps_hfake_now + 60000u);
    CHECK_EQ(u2ps_hfake_ncalls, n);
    CHECK_EQ(happ_log.ends, 0);
  }
  /* TEST_PACKET: test mode 4 */
  if (!hstart("testdev.txt"))
    return;
  u2ps_hfake_speed = U2PS_SPEED_FULL;
  as_test_device(0x0104);
  u2ps_hfake_connect();
  CHECK(u2ps_hfake_wait(decided, 2000));
  if (U2PS_HOST_REFUSE_FS) {
    check_refused();
  } else {
    CHECK_EQ(happ_log.test_devices, 1);
    CHECK_EQ(happ_log.ends, 1);
    CHECK_EQ(happ_log.end, U2PS_HOST_END_TEST_MODE);
    CHECK_EQ(u2ps_hfake_test_selector, 4);
  }
  /* SINGLE_STEP_GET_DEV_DESC: the request 15 s later, with the device's
   * mps0 */
  if (!hstart("testdev.txt"))
    return;
  u2ps_hfake_speed = U2PS_SPEED_FULL;
  u2ps_hmodel.device[7] = 16;
  as_test_device(0x0107);
  u2ps_hfake_connect();
  CHECK(u2ps_hfake_wait(decided, 2000));
  if (U2PS_HOST_REFUSE_FS) {
    check_refused();
  } else {
    CHECK_EQ(happ_log.test_devices, 1);
    uint32_t d = req(R_CONFIG + 1u)->done_ms;
    u2ps_hfake_run_until(d + 15003u);
    const u2ps_hfake_call_t *g = req(R_CONFIG + 2u);
    CHECK_EQ(g->kind, U2PS_HFAKE_CONTROL);
    CHECK(g->ms >= d + 15000u && g->ms <= d + 15001u);
    CHECK_EQ(g->mps0, 16);
    CHECK_EQ(g->len, 18);
    CHECK_EQ(happ_log.ends, 0);
  }
  /* HS_HOST_PORT_SUSPEND_RESUME: suspend and resume at full speed */
  if (!hstart("testdev.txt"))
    return;
  u2ps_hfake_speed = U2PS_SPEED_FULL;
  as_test_device(0x0106);
  u2ps_hfake_connect();
  CHECK(u2ps_hfake_wait(decided, 2000));
  if (U2PS_HOST_REFUSE_FS) {
    check_refused();
  } else {
    uint32_t d = req(R_CONFIG + 1u)->done_ms;
    u2ps_hfake_run_until(d + 30100u);
    CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_SUSPEND), 1);
    CHECK(u2ps_hfake_get(U2PS_HFAKE_SUSPEND, 0).ms <= d + 15001u);
    CHECK(u2ps_hfake_count(U2PS_HFAKE_RESUME) >= 2u);
    CHECK(!u2ps_hfake_suspended() && !u2ps_hfake_resuming());
    CHECK_EQ(happ_log.ends, 0);
    CHECK_EQ(u2ps_hfake_violations, 0);
  }
}

/* ---- High speed unchanged ---------------------------------------------------------- */

static void test_high_speed(void)
{
  if (!at_speed("uac2-hs-default.txt", U2PS_SPEED_HIGH, 64))
    return;
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(req(R_DEVICE8)->setup.wLength, 64);
  for (unsigned k = 0; k < u2ps_hfake_count(U2PS_HFAKE_CONTROL); k++)
    CHECK_EQ(req(k)->mps0, 64);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* ---- The fake port's checks ------------------------------------------------------ */

/* With the test device 0x0200 configured at low speed and quiet, the test
 * makes the port calls the stack must never make there */
static void test_fake_checks(void)
{
  if (U2PS_HOST_REFUSE_FS)
    return;
  if (!hstart("testdev.txt"))
    return;
  u2ps_hfake_speed = U2PS_SPEED_LOW;
  u2ps_hmodel.device[7] = 8;
  as_test_device(0x0200);
  u2ps_hfake_connect();
  CHECK(u2ps_hfake_wait(decided, 2000));
  CHECK_EQ(happ_log.test_devices, 1);
  CHECK_EQ(u2ps_hfake_violations, 0);
  static const uint8_t setup[8] = {0x80, U2PS_GET_STATUS, 0, 0, 0, 0, 2, 0};
  static uint8_t data[64];
  u2ps_hport_irq_mask(true);
  u2ps_hport_control(1, 64, setup, data);
  u2ps_hport_irq_mask(false);
  /* That one, and the test's SETUP packet and data stage, which lie
   * outside the DMA memory the stack checked at init */
  CHECK_EQ(u2ps_hfake_violations, 3);
  u2ps_hfake_step();
  u2ps_hstream_t st = {.addr = 1, .ep_out = 1, .mps = 8};
  u2ps_hport_irq_mask(true);
  (void)u2ps_hport_stream_open(&st);
  u2ps_hport_irq_mask(false);
  CHECK_EQ(u2ps_hfake_violations, 4);
  u2ps_hfake_violations = 0;
}

int main(void)
{
  printf("library: U2PS_HOST_REFUSE_FS=%d\n", U2PS_HOST_REFUSE_FS);
  test_full_speed_audio();
  test_full_speed_other();
  test_low_speed();
  test_qualifier();
  test_qualifier_malformed();
  test_qualifier_failures();
  test_type_of_whole();
  test_ep0_sizes();
  test_no_response();
  test_test_device();
  test_high_speed();
  test_fake_checks();
  return check_done();
}
