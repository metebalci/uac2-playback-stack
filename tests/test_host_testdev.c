/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_host_testdev.c: the test device (VID 0x1A0A) of the
 * Embedded Host tests, against the fake host port and the device model,
 * with the library of a 100 mA budget (U2PS_HOST_IA_VBUS_RATED_MA):
 * - the requests in order: DEVICE8, ADDRESS, DEVICE, CONFIG9, CONFIG,
 *   SET_CONFIGURATION of the value read, then on_test_device(pid, bcd);
 * - PIDs 0x0101-0x0104: on_end(TEST_MODE), disable, test mode 3, 1, 2, 4;
 *   nothing for 60 s; connect refused; a re-init ends it;
 * - 0x0106: suspend 15 s after SET_CONFIGURATION, resume(true) 15 s later,
 *   resume(false) 20 ms after that, in the DWC2 and the ChipIdea model;
 *   no GET_STATUS, no call and no on_end for 60 s;
 * - 0x0107: one whole GET_DESCRIPTOR(device, 18) 15 s after it;
 * - 0x0108: the SETUP stage at once, the data stage 15 s after its
 *   completion, 18 bytes;
 * - 0x0200: stays configured for 60 s;
 * - bConfigurationValue 2; a test device over the budget (END_POWER, no
 *   SET_CONFIGURATION, no test); the other PIDs of 0x1A0A and VID 0x1A0B
 *   take the normal path; a disconnect in each wait ends the session once
 *   and nothing follows; suspend and stream_start refused in the session.
 * Each 15 s wait must lie in [15,000, 15,001] ms of the fake's 1 ms tick.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "host_support.h"
#include "u2ps_ch9.h"

#define WAIT_MS 15000u
#define MINUTE  60000u

enum { R_DEVICE8, R_ADDRESS, R_DEVICE, R_CONFIG9, R_CONFIG, R_SET_CONFIG, R_COUNT };

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

/* The device model as test device 'vid'/'pid' with bcdDevice 'bcd' and the
 * configuration of 'fixture'; connected, and stepped until on_test_device,
 * on_device or on_end */
static bool test_device(const char *fixture, uint16_t vid, uint16_t pid, uint16_t bcd)
{
  if (!hstart(fixture))
    return false;
  u2ps_hmodel.device[8] = (uint8_t)vid;
  u2ps_hmodel.device[9] = (uint8_t)(vid >> 8);
  u2ps_hmodel.device[10] = (uint8_t)pid;
  u2ps_hmodel.device[11] = (uint8_t)(pid >> 8);
  u2ps_hmodel.device[12] = (uint8_t)bcd;
  u2ps_hmodel.device[13] = (uint8_t)(bcd >> 8);
  u2ps_hfake_connect();
  return u2ps_hfake_wait(decided, 2000);
}

/* The requests up to on_test_device, in order, and the callback in an
 * upcall at SET_CONFIGURATION's completion with pid and bcd */
static void check_identified(uint16_t pid, uint16_t bcd, uint8_t value)
{
  CHECK_EQ(happ_log.test_devices, 1);
  CHECK_EQ(happ_log.test_pid, pid);
  CHECK_EQ(happ_log.test_bcd, bcd);
  CHECK_EQ(happ_log.test_from, U2PS_HFAKE_FROM_UPCALL);
  CHECK_EQ(happ_log.devices, 0);
  CHECK_EQ(happ_log.calls_at_test, R_COUNT + 2u); /* the two reset calls first */
  static const struct {
    uint8_t addr, bm, req;
    uint16_t value, length;
  } want[R_COUNT] = {
    {0, 0x80, U2PS_GET_DESCRIPTOR, 0x0100, 64}, {0, 0x00, U2PS_SET_ADDRESS, 1, 0},
    {1, 0x80, U2PS_GET_DESCRIPTOR, 0x0100, 18}, {1, 0x80, U2PS_GET_DESCRIPTOR, 0x0200, 9},
    {1, 0x80, U2PS_GET_DESCRIPTOR, 0x0200, 18}, {1, 0x00, U2PS_SET_CONFIGURATION, 0, 0}};
  for (unsigned k = 0; k < R_COUNT; k++) {
    const u2ps_hfake_call_t *c = req(k);
    CHECK_EQ(c->kind, U2PS_HFAKE_CONTROL);
    CHECK_EQ(c->arg, want[k].addr);
    CHECK_EQ(c->mps0, 64);
    CHECK_EQ(c->setup.bmRequestType, want[k].bm);
    CHECK_EQ(c->setup.bRequest, want[k].req);
    CHECK_EQ(c->setup.wValue, k == R_SET_CONFIG ? value : want[k].value);
    CHECK_EQ(c->setup.wLength, want[k].length);
    CHECK_EQ(c->result, U2PS_HPORT_CONTROL_OK);
  }
  CHECK_EQ(happ_log.test_ms, req(R_SET_CONFIG)->done_ms);
  CHECK_EQ(u2ps_hmodel.configuration, value);
}

/* No port call and no app callback for a minute */
static void check_quiet_minute(void)
{
  unsigned calls = u2ps_hfake_ncalls, events = happ_log.nevents;
  u2ps_hfake_run_until(u2ps_hfake_now + MINUTE);
  CHECK_EQ(u2ps_hfake_ncalls, calls);
  CHECK_EQ(happ_log.nevents, events);
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_hmodel.unexpected, 0);
}

/* ---- 0x0101 to 0x0104: test modes ------------------------------------------------ */

static void test_test_modes(void)
{
  static const uint16_t pids[4] = {0x0101, 0x0102, 0x0103, 0x0104};
  static const int selectors[4] = {3, 1, 2, 4};
  for (unsigned k = 0; k < 4u; k++) {
    if (!test_device("testdev.txt", 0x1A0A, pids[k], 0x0100))
      return;
    check_identified(pids[k], 0x0100, 1);
    CHECK_EQ(happ_log.ends, 1);
    CHECK_EQ(happ_log.end, U2PS_HOST_END_TEST_MODE);
    CHECK_EQ(happ_log.refusal, U2PS_HOST_REFUSAL_NONE);
    CHECK_EQ(happ_log.end_from, U2PS_HFAKE_FROM_UPCALL);
    CHECK(strcmp(happ_log.events, "TE") == 0);
    /* on_end, then disable, then test mode */
    int dis = u2ps_hfake_find(U2PS_HFAKE_DISABLE, happ_log.calls_at_test);
    int tm = u2ps_hfake_find(U2PS_HFAKE_TEST_MODE, happ_log.calls_at_test);
    CHECK(dis >= (int)happ_log.calls_at_end && tm == dis + 1);
    CHECK_EQ(u2ps_hfake_test_selector, selectors[k]);
    CHECK_EQ(u2ps_hfake_get(U2PS_HFAKE_TEST_MODE, 0).from, U2PS_HFAKE_FROM_UPCALL);
    CHECK_EQ(u2ps_hfake_ncalls, (unsigned)tm + 1u);
    check_quiet_minute();
    /* connect(true) and a new connect are refused */
    unsigned n = u2ps_hfake_ncalls;
    u2ps_host_connect(false);
    u2ps_host_connect(true);
    u2ps_hfake_disconnect();
    u2ps_hfake_connect();
    u2ps_hfake_run_until(u2ps_hfake_now + 1000);
    CHECK_EQ(u2ps_hfake_ncalls, n);
    CHECK_EQ(happ_log.ends, 1);
    CHECK(!u2ps_host_suspend());
    /* A re-init ends it: the next device enumerates */
    happ_clear();
    hmodel_load("uac2-hs-default.txt");
    CHECK_EQ(u2ps_host_init(), U2PS_HOST_OK);
    CHECK_EQ(happ_log.ends, 0);
    CHECK_EQ(u2ps_hfake_test_selector, -1);
    u2ps_host_connect(true);
    u2ps_hfake_connect();
    CHECK(u2ps_hfake_wait(happ_done, 2000));
    CHECK_EQ(happ_log.devices, 1);
    CHECK_EQ(u2ps_hfake_violations, 0);
  }
}

/* ---- 0x0106: suspend and resume ----------------------------------------------------- */

/* The sequence with the port in the DWC2 model (resume_ms 0) or the
 * ChipIdea model (the port ends the K resume_ms after it began) */
static void suspend_resume(uint32_t resume_ms)
{
  if (!test_device("testdev.txt", 0x1A0A, 0x0106, 0x0100))
    return;
  u2ps_hfake_resume_ms = resume_ms;
  check_identified(0x0106, 0x0100, 1);
  uint32_t d = req(R_SET_CONFIG)->done_ms;
  /* Nothing but SOFs during the wait: the stack refuses a stream and a
   * suspend of its own */
  u2ps_hfake_run_until(d + 100u);
  CHECK_EQ(u2ps_host_stream_start(1, 48000), 0);
  CHECK(!u2ps_host_suspend());
  u2ps_hfake_run_until(d + WAIT_MS + 2u);
  u2ps_hfake_call_t s = u2ps_hfake_get(U2PS_HFAKE_SUSPEND, 0);
  CHECK_EQ(s.kind, U2PS_HFAKE_SUSPEND);
  CHECK(s.ms >= d + WAIT_MS && s.ms <= d + WAIT_MS + 1u);
  CHECK(u2ps_hfake_suspended());
  CHECK(!u2ps_host_resume());
  u2ps_hfake_run_until(s.ms + WAIT_MS + 2u);
  int i = u2ps_hfake_find(U2PS_HFAKE_RESUME, 0);
  CHECK(i >= 0);
  if (i < 0)
    return;
  u2ps_hfake_call_t r = u2ps_hfake_calls[i];
  CHECK_EQ(r.arg, 1);
  CHECK(r.ms >= s.ms + WAIT_MS && r.ms <= s.ms + WAIT_MS + 1u);
  u2ps_hfake_run_until(r.ms + 20u + resume_ms + 2u);
  u2ps_hfake_call_t f = u2ps_hfake_get(U2PS_HFAKE_RESUME, (unsigned)i + 1u);
  CHECK_EQ(f.kind, U2PS_HFAKE_RESUME);
  CHECK_EQ(f.arg, 0);
  CHECK(f.ms >= r.ms + 20u && f.ms <= r.ms + 21u);
  /* resume(false) until it returns true, once per tick, then nothing */
  unsigned falses = 0;
  int last = -1;
  for (unsigned k = (unsigned)i + 1u; k < u2ps_hfake_ncalls; k++) {
    CHECK_EQ(u2ps_hfake_calls[k].kind, U2PS_HFAKE_RESUME);
    falses += !u2ps_hfake_calls[k].ret;
    last = (int)k;
  }
  CHECK(last >= 0 && u2ps_hfake_calls[last].ret);
  /* The first resume(false) comes 21 ms after resume(true): TDRSMDN is a
   * lower bound, more than 20 counts of the ms counter */
  CHECK_EQ(falses, resume_ms > 21u ? resume_ms - 21u : 0u);
  CHECK(!u2ps_hfake_suspended() && !u2ps_hfake_resuming());
  CHECK_EQ(u2ps_hmodel.get_status, 0);
  CHECK_EQ(happ_log.ends, 0);
  CHECK_EQ(happ_log.resumes, 0);
  check_quiet_minute();
  CHECK_EQ(happ_log.ends, 0);
}

static void test_suspend_resume(void)
{
  suspend_resume(0);
  suspend_resume(20);
  suspend_resume(25);
}

/* ---- 0x0107 and 0x0108: single steps --------------------------------------------- */

static void test_single_step_get(void)
{
  if (!test_device("testdev.txt", 0x1A0A, 0x0107, 0x0100))
    return;
  check_identified(0x0107, 0x0100, 1);
  uint32_t d = req(R_SET_CONFIG)->done_ms;
  u2ps_hfake_run_until(d + WAIT_MS + 3u);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), R_COUNT + 1u);
  const u2ps_hfake_call_t *g = req(R_COUNT);
  CHECK_EQ(g->kind, U2PS_HFAKE_CONTROL);
  CHECK(g->ms >= d + WAIT_MS && g->ms <= d + WAIT_MS + 1u);
  CHECK_EQ(g->arg, 1);
  CHECK_EQ(g->setup.bmRequestType, 0x80);
  CHECK_EQ(g->setup.bRequest, U2PS_GET_DESCRIPTOR);
  CHECK_EQ(g->setup.wValue, 0x0100);
  CHECK_EQ(g->setup.wLength, 18);
  CHECK_EQ(g->result, U2PS_HPORT_CONTROL_OK);
  CHECK_EQ(g->len, 18);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL_SETUP), 0);
  CHECK_EQ(happ_log.ends, 0);
  check_quiet_minute();
}

static void test_single_step_set_feature(void)
{
  if (!test_device("testdev.txt", 0x1A0A, 0x0108, 0x0100))
    return;
  check_identified(0x0108, 0x0100, 1);
  uint32_t d = req(R_SET_CONFIG)->done_ms;
  /* The SETUP stage at once, in the upcall of SET_CONFIGURATION */
  u2ps_hfake_call_t s = u2ps_hfake_get(U2PS_HFAKE_CONTROL_SETUP, 0);
  CHECK_EQ(s.kind, U2PS_HFAKE_CONTROL_SETUP);
  CHECK_EQ(s.ms, d);
  CHECK_EQ(s.from, U2PS_HFAKE_FROM_UPCALL);
  CHECK_EQ(s.arg, 1);
  CHECK_EQ(s.mps0, 64);
  CHECK_EQ(s.setup.bmRequestType, 0x80);
  CHECK_EQ(s.setup.bRequest, U2PS_GET_DESCRIPTOR);
  CHECK_EQ(s.setup.wValue, 0x0100);
  CHECK_EQ(s.setup.wLength, 18);
  u2ps_hfake_run_until(d + 5u);
  s = u2ps_hfake_get(U2PS_HFAKE_CONTROL_SETUP, 0);
  CHECK_EQ(s.result, U2PS_HPORT_CONTROL_OK);
  CHECK_EQ(s.len, 0);
  uint32_t c = s.done_ms;
  CHECK(c != 0xFFFFFFFFu);
  u2ps_hfake_run_until(c + WAIT_MS + 3u);
  u2ps_hfake_call_t data = u2ps_hfake_get(U2PS_HFAKE_CONTROL_DATA, 0);
  CHECK_EQ(data.kind, U2PS_HFAKE_CONTROL_DATA);
  CHECK(data.ms >= c + WAIT_MS && data.ms <= c + WAIT_MS + 1u);
  CHECK_EQ(data.result, U2PS_HPORT_CONTROL_OK);
  CHECK_EQ(data.len, 18);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), R_COUNT);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL_DATA), 1);
  CHECK_EQ(happ_log.ends, 0);
  check_quiet_minute();
}

/* ---- 0x0200, bConfigurationValue, power, the other PIDs ------------------------- */

static void test_ttst_config(void)
{
  if (!test_device("testdev.txt", 0x1A0A, 0x0200, 0x0001))
    return;
  check_identified(0x0200, 0x0001, 1);
  CHECK_EQ(u2ps_hfake_ncalls, happ_log.calls_at_test);
  check_quiet_minute();
  CHECK_EQ(happ_log.ends, 0);
  CHECK_EQ(u2ps_hmodel.configuration, 1);
  CHECK_EQ(u2ps_host_stream_start(1, 48000), 0);
  CHECK(!u2ps_host_suspend());
  CHECK_EQ(u2ps_hfake_ncalls, happ_log.calls_at_test);
}

/* testdev2: bConfigurationValue 2, which SET_CONFIGURATION names */
static void test_configuration_value(void)
{
  static const uint16_t pids[2] = {0x0200, 0x0104};
  for (unsigned k = 0; k < 2u; k++) {
    if (!hstart("testdev.txt"))
      return;
    u2ps_hmodel.config[0][5] = 2;
    u2ps_hmodel.device[8] = 0x0A;
    u2ps_hmodel.device[9] = 0x1A;
    u2ps_hmodel.device[10] = (uint8_t)pids[k];
    u2ps_hmodel.device[11] = (uint8_t)(pids[k] >> 8);
    u2ps_hfake_connect();
    CHECK(u2ps_hfake_wait(decided, 2000));
    check_identified(pids[k], 0x0100, 2);
  }
}

/* A test device beyond the 100 mA budget: END_POWER, no
 * SET_CONFIGURATION, no on_test_device, no test mode */
static void test_over_budget(void)
{
  static const uint16_t pids[3] = {0x0104, 0x0106, 0x0200};
  for (unsigned k = 0; k < 3u; k++) {
    if (!hstart("testdev.txt"))
      return;
    u2ps_hmodel.config[0][8] = 255;
    u2ps_hmodel.device[8] = 0x0A;
    u2ps_hmodel.device[9] = 0x1A;
    u2ps_hmodel.device[10] = (uint8_t)pids[k];
    u2ps_hmodel.device[11] = (uint8_t)(pids[k] >> 8);
    u2ps_hfake_connect();
    CHECK(u2ps_hfake_wait(decided, 2000));
    CHECK_EQ(happ_log.ends, 1);
    CHECK_EQ(happ_log.end, U2PS_HOST_END_POWER);
    CHECK_EQ(happ_log.refusal, U2PS_HOST_REFUSAL_NONE);
    CHECK_EQ(happ_log.test_devices, 0);
    CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), R_SET_CONFIG);
    CHECK_EQ(u2ps_hmodel.configuration, 0);
    u2ps_hfake_run_until(u2ps_hfake_now + MINUTE);
    CHECK_EQ(u2ps_hfake_test_selector, -1);
    CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_SUSPEND), 0);
    CHECK_EQ(happ_log.ends, 1);
    CHECK_EQ(u2ps_hfake_violations, 0);
  }
  /* bMaxPower 50 is 100 mA: within */
  if (!test_device("testdev.txt", 0x1A0A, 0x0200, 0x0100))
    return;
  check_identified(0x0200, 0x0100, 1);
}

/* 0x0105 (reserved), 0x0201 and 0x0202 of VID 0x1A0A, and VID 0x1A0B with
 * PID 0x0104: the normal path. With no audio function: NOT_PLAYABLE; with
 * one: on_device. */
static void test_other_ids(void)
{
  static const struct {
    uint16_t id_vendor, id_product;
  } ids[4] = {{0x1A0A, 0x0105}, {0x1A0A, 0x0201}, {0x1A0A, 0x0202}, {0x1A0B, 0x0104}};
  for (unsigned k = 0; k < 4u; k++) {
    if (!test_device("testdev.txt", ids[k].id_vendor, ids[k].id_product, 0x0100))
      return;
    CHECK_EQ(happ_log.test_devices, 0);
    CHECK_EQ(happ_log.ends, 1);
    CHECK_EQ(happ_log.end, U2PS_HOST_END_NOT_PLAYABLE);
    CHECK_EQ(happ_log.refusal, U2PS_HOST_REFUSAL_NOT_AUDIO);
    CHECK_EQ(u2ps_hmodel.configuration, 0);
    if (!test_device("uac2-hs-default.txt", ids[k].id_vendor, ids[k].id_product, 0x0100))
      return;
    CHECK_EQ(happ_log.test_devices, 0);
    CHECK_EQ(happ_log.devices, 1);
    CHECK_EQ(happ_log.ends, 0);
    CHECK_EQ(happ_log.dev.id_vendor, ids[k].id_vendor);
    CHECK_EQ(happ_log.dev.id_product, ids[k].id_product);
    u2ps_hfake_run_until(u2ps_hfake_now + MINUTE);
    CHECK_EQ(u2ps_hfake_test_selector, -1);
    CHECK_EQ(u2ps_hfake_violations, 0);
  }
}

/* ---- A disconnect in each wait -------------------------------------------------- */

/* PID 'pid', disconnected 'after' ms after the completion of SET_CONFIGURATION
 * (for 0x0108, of its SETUP stage): one on_end(DISCONNECT), then nothing */
static void disconnect_in_wait(uint16_t pid, uint32_t after)
{
  if (!test_device("testdev.txt", 0x1A0A, pid, 0x0100))
    return;
  check_identified(pid, 0x0100, 1);
  uint32_t d = req(R_SET_CONFIG)->done_ms;
  if (pid == 0x0108) {
    u2ps_hfake_run_until(d + 3u);
    d = u2ps_hfake_get(U2PS_HFAKE_CONTROL_SETUP, 0).done_ms;
  }
  u2ps_hfake_run_until(d + after);
  u2ps_hfake_disconnect();
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, U2PS_HOST_END_DISCONNECT);
  check_quiet_minute();
  CHECK_EQ(happ_log.ends, 1);
}

static void test_disconnects(void)
{
  disconnect_in_wait(0x0106, 5000);                    /* before the suspend */
  disconnect_in_wait(0x0106, WAIT_MS + 5000u);         /* suspended */
  disconnect_in_wait(0x0106, 2u * WAIT_MS + 10u);      /* in the resume K */
  disconnect_in_wait(0x0107, 5000);
  disconnect_in_wait(0x0108, 5000);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL_DATA), 0);
  disconnect_in_wait(0x0200, 5000);
}

int main(void)
{
  test_test_modes();
  test_suspend_resume();
  test_single_step_get();
  test_single_step_set_feature();
  test_ttst_config();
  test_configuration_value();
  test_over_budget();
  test_other_ids();
  test_disconnects();
  return check_done();
}
