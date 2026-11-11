/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_host_messages.c: the session's end reasons that the
 * OTG and EH Compliance Plan's messages need, with the library of a
 * 100 mA budget (U2PS_HOST_IA_VBUS_RATED_MA):
 * - a hub: END_HUB, with no configuration read;
 * - bMaxPower 50 (100 mA) selected, 51 (102 mA) END_POWER;
 * - configuration 0 over the budget and configuration 1 within it: the
 *   second selected; configuration 0 without audio and configuration 1
 *   over the budget: END_POWER, not NOT_AUDIO; and the other way round;
 * - an unsupported device (an OTG descriptor and nine interfaces, Plan
 *   7.3.6), a hub and an over-budget device end less than 100 ms after the
 *   device descriptor.
 * The budget's range (a static assert at 7 and 5001 mA) is
 * test_host_budget.cmake.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "host_support.h"
#include "u2ps_ch9.h"

enum { R_DEVICE8, R_ADDRESS, R_DEVICE, R_CONFIG9 };

static const u2ps_hfake_call_t *req(unsigned nth)
{
  static const u2ps_hfake_call_t none = {.kind = U2PS_HFAKE_MISSING, .ms = 0xFFFFFFFFu,
                                         .done_ms = 0xFFFFFFFFu};
  const u2ps_hfake_call_t *c = u2ps_hfake_control_nth(nth);
  return c != NULL ? c : &none;
}

/* GET_DESCRIPTOR(configuration 'index') was asked */
static bool asked_config(uint8_t index)
{
  for (unsigned k = 0; k < u2ps_hfake_count(U2PS_HFAKE_CONTROL); k++)
    if (req(k)->setup.bRequest == U2PS_GET_DESCRIPTOR && req(k)->setup.wValue == (0x0200u | index))
      return true;
  return false;
}

static bool set_configuration_sent(void)
{
  for (unsigned k = 0; k < u2ps_hfake_count(U2PS_HFAKE_CONTROL); k++)
    if (req(k)->setup.bRequest == U2PS_SET_CONFIGURATION)
      return true;
  return false;
}

/* One on_end, 'why' and 'refusal', in an upcall, less than 100 ms after the
 * device descriptor's completion; no on_device, the port disabled */
static void check_end(u2ps_host_end_t why, u2ps_host_refusal_t refusal)
{
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, why);
  CHECK_EQ(happ_log.refusal, refusal);
  CHECK_EQ(happ_log.devices, 0);
  CHECK_EQ(happ_log.test_devices, 0);
  CHECK_EQ(happ_log.end_from, U2PS_HFAKE_FROM_UPCALL);
  uint32_t d = req(R_DEVICE)->done_ms;
  unsigned last = happ_log.calls_at_end;
  uint32_t end = last > 0u ? u2ps_hfake_calls[last - 1u].done_ms : 0xFFFFFFFFu;
  CHECK(d != 0xFFFFFFFFu && end >= d && end < d + 100u);
  CHECK_EQ(u2ps_hfake_get(U2PS_HFAKE_DISABLE, happ_log.calls_at_end).kind, U2PS_HFAKE_DISABLE);
  CHECK(!set_configuration_sent());
  CHECK_EQ(u2ps_hmodel.configuration, 0);
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_hmodel.unexpected, 0);
}

static void test_hub(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.device[4] = 0x09; /* bDeviceClass: hub (USB 2.0 11.23.1) */
  u2ps_hmodel.device[5] = 0x00;
  u2ps_hmodel.device[6] = 0x01;
  CHECK(henumerate());
  check_end(U2PS_HOST_END_HUB, U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), R_DEVICE + 1u);
  CHECK(!asked_config(0));
}

static void test_bmaxpower(void)
{
  /* 50: 100 mA, within */
  if (!hstart("uac2-hs-default.txt"))
    return;
  CHECK_EQ(u2ps_hmodel.config[0][8], 50);
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(happ_log.ends, 0);
  CHECK_EQ(u2ps_hmodel.configuration, 1);
  /* 51: 102 mA, beyond */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.config[0][8] = 51;
  CHECK(henumerate());
  check_end(U2PS_HOST_END_POWER, U2PS_HOST_REFUSAL_NONE);
  /* bmAttributes D6 (self-powered) changes nothing: bMaxPower is bus
   * current */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.config[0][7] = 0xC0;
  u2ps_hmodel.config[0][8] = 51;
  CHECK(henumerate());
  check_end(U2PS_HOST_END_POWER, U2PS_HOST_REFUSAL_NONE);
}

/* Two configurations of the device model, from the fixtures, with their
 * bConfigurationValue and bMaxPower */
static bool two(const char *c0, uint8_t power0, const char *c1, uint8_t value1, uint8_t power1)
{
  static uint8_t buf[U2PS_HMODEL_CONFIG_MAX];
  if (!hstart(c1))
    return false;
  memcpy(buf, u2ps_hmodel.config[0], u2ps_hmodel.config_len[0]);
  uint16_t n1 = u2ps_hmodel.config_len[0];
  if (!hmodel_load(c0))
    return false;
  u2ps_hmodel_set_config(1, buf, n1);
  u2ps_hmodel.config[0][8] = power0;
  u2ps_hmodel.config[1][5] = value1;
  u2ps_hmodel.config[1][8] = power1;
  u2ps_hmodel.device[17] = 2;
  return true;
}

static void test_two_configurations(void)
{
  /* Configuration 0 over the budget, configuration 1 within: the second */
  if (!two("uac2-hs-default.txt", 51, "uac2-hs-24-3.txt", 2, 50))
    return;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(happ_log.ends, 0);
  CHECK_EQ(u2ps_hmodel.configuration, 2);
  CHECK(asked_config(1));
  CHECK_EQ(happ_log.dev.alternate_setting_count, 1);
  CHECK_EQ(u2ps_hfake_violations, 0);
  /* Configuration 0 without audio, configuration 1 audio over the budget:
   * END_POWER, not the first configuration's NOT_AUDIO */
  if (!two("testdev.txt", 50, "uac2-hs-default.txt", 2, 51))
    return;
  CHECK(henumerate());
  check_end(U2PS_HOST_END_POWER, U2PS_HOST_REFUSAL_NONE);
  CHECK(asked_config(1));
  /* The other way round */
  if (!two("uac2-hs-default.txt", 51, "testdev.txt", 2, 50))
    return;
  CHECK(henumerate());
  check_end(U2PS_HOST_END_POWER, U2PS_HOST_REFUSAL_NONE);
  /* Neither playable: the first configuration's refusal stands */
  if (!two("testdev.txt", 50, "testdev.txt", 2, 51))
    return;
  CHECK(henumerate());
  check_end(U2PS_HOST_END_NOT_PLAYABLE, U2PS_HOST_REFUSAL_NOT_AUDIO);
}

/* The Plan's unsupported device: an OTG descriptor and nine interfaces */
static void test_unsupported(void)
{
  if (!hstart("otg-9-interfaces.txt"))
    return;
  CHECK(henumerate());
  check_end(U2PS_HOST_END_NOT_PLAYABLE, U2PS_HOST_REFUSAL_NOT_AUDIO);
}

int main(void)
{
  test_hub();
  test_bmaxpower();
  test_two_configurations();
  test_unsupported();
  return check_done();
}
