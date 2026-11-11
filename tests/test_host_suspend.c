/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_host_suspend.c: the app's suspend and resume and a
 * device's remote wakeup, against the fake host port in its DWC2 model
 * (resume(false) ends the resume at once) and its ChipIdea model (the port
 * ends the K itself, 20 or 25 ms after it began):
 * - each refusal cause of u2ps_host_suspend() and u2ps_host_resume():
 *   false, no port call, and the contract code where the header names one;
 * - accepted: SUSPEND; stream_start refused while suspended; RESUME(true);
 *   RESUME(false) first [21, 22] ms after it; GET_STATUS [10, 11] ms after
 *   its first true (each wait ends at the first tick that reads more than
 *   its value since its start, test_host_waits.c); on_resumed(false) at GET_STATUS's completion; a stream
 *   starts again;
 * - a remote wakeup while suspended: no RESUME(true), then the same, and
 *   on_resumed(true); during the app's resume: counted, one
 *   on_resumed(false); when not suspended: counted only;
 * - while suspended or resuming: a disconnect (no GET_STATUS), VBUS loss,
 *   a re-init, test mode end the session with their reasons;
 * - GET_STATUS stalled or unanswered: STALL, TIMEOUT; a port that stays in
 *   resume: END_ERROR 50 ms after the first resume(false);
 * - the fake port's own check: a control transfer while suspended is a
 *   port violation.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "host_support.h"
#include "u2ps_ch9.h"

static uint32_t mode; /* u2ps_hfake_resume_ms of the run */

/* When the resume K ends after it began: 20 ms (TDRSMDN), or later in the
 * ChipIdea model */
static uint32_t k_end(void)
{
  return mode > 20u ? mode : 20u;
}

static bool stream_decided(void)
{
  return happ_log.stream_starts > 0u || happ_log.stream_stops > 0u;
}

static bool resumed_or_ended(void)
{
  return happ_log.resumes > 0u || happ_log.ends > 0u;
}

/* The device configured, the port in the run's model */
static bool configured(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return false;
  if (!henumerate() || happ_log.devices != 1u) {
    printf("  not configured\n");
    return false;
  }
  u2ps_hfake_resume_ms = mode;
  return true;
}

/* The suspend refused: false, no port call, and 'code' reported once
 * (U2PS_CONTRACT_NONE: none) */
static void check_suspend_refused(u2ps_contract_t code)
{
  unsigned n = u2ps_hfake_ncalls;
  contract_expect(true);
  CHECK(!u2ps_host_suspend());
  CHECK_EQ(u2ps_hfake_ncalls, n);
  CHECK_EQ(contract_log.total, code == U2PS_CONTRACT_NONE ? 0u : CONTRACT_N(1u));
  if (code != U2PS_CONTRACT_NONE)
    CHECK_EQ(contract_log.count[code], CONTRACT_N(1u));
  contract_expect(false);
}

static void check_resume_refused(u2ps_contract_t code)
{
  unsigned n = u2ps_hfake_ncalls;
  contract_expect(true);
  CHECK(!u2ps_host_resume());
  CHECK_EQ(u2ps_hfake_ncalls, n);
  CHECK_EQ(contract_log.total, code == U2PS_CONTRACT_NONE ? 0u : CONTRACT_N(1u));
  if (code != U2PS_CONTRACT_NONE)
    CHECK_EQ(contract_log.count[code], CONTRACT_N(1u));
  contract_expect(false);
}

/* After a resume began at 'k' (resume(true) or the wakeup): RESUME(false)
 * first [21, 22] ms after it, then on every tick until true; GET_STATUS
 * [10, 11] ms after the first true; on_resumed(remote) at its completion,
 * in an upcall. 'from' is the log index the resume's calls start at. */
static void check_resume_sequence(uint32_t k, unsigned from, bool remote)
{
  CHECK(u2ps_hfake_wait(resumed_or_ended, 200));
  CHECK_EQ(happ_log.ends, 0);
  CHECK_EQ(happ_log.resumes, 1);
  CHECK_EQ(happ_log.resumed_remote, remote);
  CHECK_EQ(happ_log.resumed_from, U2PS_HFAKE_FROM_UPCALL);
  int f = -1, t = -1;
  unsigned trues = 0;
  for (unsigned i = from; i < u2ps_hfake_ncalls; i++) {
    const u2ps_hfake_call_t *c = &u2ps_hfake_calls[i];
    if (c->kind == U2PS_HFAKE_RESUME && c->arg == 0u) {
      if (f < 0)
        f = (int)i;
      if (c->ret && t < 0)
        t = (int)i;
      trues += c->ret;
    }
  }
  CHECK(f >= 0 && t >= 0);
  if (f < 0 || t < 0)
    return;
  CHECK_EQ(trues, 1);
  /* TDRSMDN is a lower bound: the first tick that reads more than 20 ms
   * since the first tick after k, so 21 or 22 ms after k in the fake's
   * 1 ms steps */
  CHECK(u2ps_hfake_calls[f].ms >= k + 21u && u2ps_hfake_calls[f].ms <= k + 22u);
  uint32_t first_true = u2ps_hfake_calls[t].ms;
  /* The K lasts until the port ends it: in the ChipIdea model 'mode' ms */
  CHECK(first_true >= k + 21u && first_true >= k + mode && first_true <= k + 22u + mode);
  int g = u2ps_hfake_find(U2PS_HFAKE_CONTROL, (unsigned)t);
  CHECK(g >= 0);
  if (g < 0)
    return;
  const u2ps_hfake_call_t *gs = &u2ps_hfake_calls[g];
  CHECK(gs->ms >= first_true + 10u && gs->ms <= first_true + 11u);
  CHECK_EQ(gs->arg, 1);
  CHECK_EQ(gs->setup.bmRequestType, 0x80);
  CHECK_EQ(gs->setup.bRequest, U2PS_GET_STATUS);
  CHECK_EQ(gs->setup.wValue, 0);
  CHECK_EQ(gs->setup.wIndex, 0);
  CHECK_EQ(gs->setup.wLength, 2);
  CHECK_EQ(happ_log.resumed_ms, gs->done_ms);
  CHECK_EQ(happ_log.calls_at_resumed, (unsigned)g + 1u);
  CHECK(!u2ps_hfake_suspended() && !u2ps_hfake_resuming());
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* A stream starts and runs */
static void check_stream_starts(void)
{
  happ_log.stream_starts = happ_log.stream_stops = 0;
  CHECK(u2ps_host_stream_start(1, 48000) != 0u);
  CHECK(u2ps_hfake_wait(stream_decided, 100));
  CHECK_EQ(happ_log.stream_starts, 1);
  CHECK_EQ(happ_log.stream_stops, 0);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* ---- Refusals ------------------------------------------------------------------- */

static void test_refusals(void)
{
  /* (1) before a successful init */
  u2ps_hfake_reset_all();
  u2ps_hfake_init_err = U2PS_HOST_ERR_PHY;
  CHECK_EQ(u2ps_host_init(), U2PS_HOST_ERR_PHY);
  u2ps_hfake_clear_log();
  check_suspend_refused(U2PS_CONTRACT_NONE);
  check_resume_refused(U2PS_CONTRACT_NONE);
  /* (2) before on_device: during the debounce and during the requests */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hfake_connect();
  check_suspend_refused(U2PS_CONTRACT_NONE);
  CHECK(u2ps_hfake_wait(u2ps_hfake_control_pending, 1000));
  check_suspend_refused(U2PS_CONTRACT_NONE);
  CHECK(u2ps_hfake_wait(happ_done, 2000));
  /* (6) configured, not suspended */
  check_resume_refused(U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED);
  /* (3) a stream that starts, runs, stops */
  CHECK(u2ps_host_stream_start(1, 48000) != 0u);
  check_suspend_refused(U2PS_CONTRACT_HOST_SUSPEND_STREAM);
  CHECK(u2ps_hfake_wait(stream_decided, 100));
  CHECK_EQ(happ_log.stream_starts, 1);
  check_suspend_refused(U2PS_CONTRACT_HOST_SUSPEND_STREAM);
  u2ps_host_stream_stop(happ_log.info.stream);
  check_suspend_refused(U2PS_CONTRACT_HOST_SUSPEND_STREAM);
  for (unsigned i = 0; i < 400u && happ_log.stream_stops == 0u; i++)
    u2ps_hfake_uframes(1);
  CHECK_EQ(happ_log.stream_stops, 1);
  /* (4) already suspended, and resuming */
  CHECK(u2ps_host_suspend());
  check_suspend_refused(U2PS_CONTRACT_HOST_SUSPEND_SUSPENDED);
  CHECK(u2ps_host_resume());
  check_suspend_refused(U2PS_CONTRACT_HOST_SUSPEND_SUSPENDED);
  /* (6) a second resume while the first runs */
  check_resume_refused(U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED);
  u2ps_hfake_run_until(u2ps_hfake_now + 25u);
  check_suspend_refused(U2PS_CONTRACT_HOST_SUSPEND_SUSPENDED);
  check_resume_refused(U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED);
  CHECK(u2ps_hfake_wait(resumed_or_ended, 200));
  CHECK_EQ(happ_log.resumes, 1);
  /* (6) after on_resumed */
  check_resume_refused(U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED);
  /* (5) test mode */
  CHECK(u2ps_host_test_mode(4));
  check_suspend_refused(U2PS_CONTRACT_NONE);
  check_resume_refused(U2PS_CONTRACT_NONE);
  /* No session after a disconnect: no contract for the resume */
  if (!configured())
    return;
  CHECK(u2ps_host_suspend());
  u2ps_hfake_disconnect();
  check_resume_refused(U2PS_CONTRACT_NONE);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* ---- Accepted ------------------------------------------------------------------- */

static void test_suspend_and_resume(void)
{
  if (!configured())
    return;
  unsigned n = u2ps_hfake_ncalls;
  CHECK(u2ps_host_suspend());
  CHECK_EQ(u2ps_hfake_ncalls, n + 1u);
  u2ps_hfake_call_t s = u2ps_hfake_get(U2PS_HFAKE_SUSPEND, n);
  CHECK_EQ(s.kind, U2PS_HFAKE_SUSPEND);
  CHECK_EQ(s.from, U2PS_HFAKE_FROM_MASKED);
  CHECK(u2ps_hfake_suspended());
  /* Suspended: no stream, nothing on the bus */
  CHECK_EQ(u2ps_host_stream_start(1, 48000), 0);
  u2ps_hfake_run_until(u2ps_hfake_now + 1000u);
  CHECK_EQ(u2ps_hfake_ncalls, n + 1u);
  CHECK_EQ(happ_log.ends, 0);
  CHECK_EQ(happ_log.resumes, 0);
  /* The app's resume */
  n = u2ps_hfake_ncalls;
  uint32_t k = u2ps_hfake_now;
  CHECK(u2ps_host_resume());
  u2ps_hfake_call_t r = u2ps_hfake_get(U2PS_HFAKE_RESUME, n);
  CHECK_EQ(r.kind, U2PS_HFAKE_RESUME);
  CHECK_EQ(r.arg, 1);
  CHECK_EQ(r.ms, k);
  CHECK_EQ(r.from, U2PS_HFAKE_FROM_MASKED);
  /* No GET_STATUS before the resume has ended, nor on_resumed */
  u2ps_hfake_run_until(k + 19u);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), 13);
  CHECK_EQ(happ_log.resumes, 0);
  check_resume_sequence(k, n + 1u, false);
  u2ps_host_stats_t st;
  u2ps_host_stats(&st);
  CHECK_EQ(st.wakeups, 0);
  check_stream_starts();
}

/* ---- Remote wakeup -------------------------------------------------------------- */

static void test_wakeup(void)
{
  if (!configured())
    return;
  CHECK(u2ps_host_suspend());
  u2ps_hfake_run_until(u2ps_hfake_now + 100u);
  unsigned n = u2ps_hfake_ncalls;
  uint32_t w = u2ps_hfake_now;
  u2ps_hfake_wakeup();
  CHECK_EQ(u2ps_hfake_ncalls, n);
  check_resume_sequence(w, n, true);
  /* No resume(true): the port drove the K itself */
  for (unsigned i = n; i < u2ps_hfake_ncalls; i++)
    CHECK(!(u2ps_hfake_calls[i].kind == U2PS_HFAKE_RESUME && u2ps_hfake_calls[i].arg != 0u));
  u2ps_host_stats_t st;
  u2ps_host_stats(&st);
  CHECK_EQ(st.wakeups, 1);
  check_stream_starts();
}

/* A wakeup during the app's resume: counted, one on_resumed(false) */
static void test_wakeup_during_resume(void)
{
  if (!configured())
    return;
  CHECK(u2ps_host_suspend());
  unsigned n = u2ps_hfake_ncalls;
  uint32_t k = u2ps_hfake_now;
  CHECK(u2ps_host_resume());
  u2ps_hfake_run_until(k + 5u);
  u2ps_hfake_wakeup();
  check_resume_sequence(k, n + 1u, false);
  u2ps_hfake_run_until(u2ps_hfake_now + 1000u);
  CHECK_EQ(happ_log.resumes, 1);
  unsigned trues = 0;
  for (unsigned i = n; i < u2ps_hfake_ncalls; i++)
    trues += u2ps_hfake_calls[i].kind == U2PS_HFAKE_RESUME && u2ps_hfake_calls[i].arg != 0u;
  CHECK_EQ(trues, 1);
  u2ps_host_stats_t st;
  u2ps_host_stats(&st);
  CHECK_EQ(st.wakeups, 1);
  /* In the recovery wait and in GET_STATUS too */
  if (!configured())
    return;
  CHECK(u2ps_host_suspend());
  k = u2ps_hfake_now;
  CHECK(u2ps_host_resume());
  u2ps_hfake_run_until(k + k_end() + 3u);
  u2ps_hfake_wakeup();
  CHECK(u2ps_hfake_wait(u2ps_hfake_control_pending, 20));
  u2ps_hfake_wakeup();
  CHECK(u2ps_hfake_wait(resumed_or_ended, 200));
  CHECK_EQ(happ_log.resumes, 1);
  CHECK(!happ_log.resumed_remote);
  u2ps_host_stats(&st);
  CHECK_EQ(st.wakeups, 2);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* A wakeup when not suspended: counted, nothing else */
static void test_wakeup_not_suspended(void)
{
  if (!configured())
    return;
  unsigned n = u2ps_hfake_ncalls, events = happ_log.nevents;
  u2ps_hfake_wakeup();
  u2ps_hfake_run_until(u2ps_hfake_now + 1000u);
  CHECK_EQ(u2ps_hfake_ncalls, n);
  CHECK_EQ(happ_log.nevents, events);
  u2ps_host_stats_t st;
  u2ps_host_stats(&st);
  CHECK_EQ(st.wakeups, 1);
  check_stream_starts();
}

/* ---- Ends while suspended or resuming ------------------------------------------- */

typedef enum { AT_SUSPENDED, AT_K, AT_RECOVERY, AT_COUNT } stage_t;

/* Suspended, then resumed by the app up to 'stage' */
static bool reach(stage_t stage)
{
  if (!configured())
    return false;
  CHECK(u2ps_host_suspend());
  u2ps_hfake_run_until(u2ps_hfake_now + 10u);
  if (stage == AT_SUSPENDED)
    return true;
  uint32_t k = u2ps_hfake_now;
  CHECK(u2ps_host_resume());
  u2ps_hfake_run_until(k + (stage == AT_K ? 10u : k_end() + 3u));
  return true;
}

static void test_ends(void)
{
  for (stage_t at = AT_SUSPENDED; at < AT_COUNT; at++) {
    /* A disconnect: no GET_STATUS, ever */
    if (!reach(at))
      return;
    u2ps_hfake_disconnect();
    CHECK_EQ(happ_log.ends, 1);
    CHECK_EQ(happ_log.end, U2PS_HOST_END_DISCONNECT);
    unsigned n = u2ps_hfake_ncalls;
    u2ps_hfake_run_until(u2ps_hfake_now + 1000u);
    CHECK_EQ(u2ps_hfake_ncalls, n);
    CHECK_EQ(u2ps_hmodel.get_status, 0);
    CHECK_EQ(happ_log.resumes, 0);
    /* VBUS lost */
    if (!reach(at))
      return;
    n = u2ps_hfake_ncalls;
    u2ps_host_vbus(false);
    CHECK_EQ(happ_log.ends, 1);
    CHECK_EQ(happ_log.end, U2PS_HOST_END_VBUS);
    CHECK_EQ(u2ps_hfake_get(U2PS_HFAKE_DISABLE, n).kind, U2PS_HFAKE_DISABLE);
    u2ps_hfake_run_until(u2ps_hfake_now + 1000u);
    CHECK_EQ(happ_log.resumes, 0);
    CHECK_EQ(u2ps_hmodel.get_status, 0);
    u2ps_host_vbus(true);
    /* A re-init */
    if (!reach(at))
      return;
    CHECK_EQ(u2ps_host_init(), U2PS_HOST_OK);
    CHECK_EQ(happ_log.ends, 1);
    CHECK_EQ(happ_log.end, U2PS_HOST_END_REINIT);
    u2ps_hfake_run_until(u2ps_hfake_now + 1000u);
    CHECK_EQ(happ_log.resumes, 0);
    /* Test mode */
    if (!reach(at))
      return;
    n = u2ps_hfake_ncalls;
    CHECK(u2ps_host_test_mode(4));
    CHECK_EQ(happ_log.ends, 1);
    CHECK_EQ(happ_log.end, U2PS_HOST_END_TEST_MODE);
    int dis = u2ps_hfake_find(U2PS_HFAKE_DISABLE, n), tm = u2ps_hfake_find(U2PS_HFAKE_TEST_MODE, n);
    CHECK(dis >= 0 && tm > dis);
    CHECK_EQ(u2ps_hfake_test_selector, 4);
    u2ps_hfake_run_until(u2ps_hfake_now + 1000u);
    CHECK_EQ(happ_log.resumes, 0);
    CHECK_EQ(u2ps_hfake_violations, 0);
  }
}

/* GET_STATUS stalled, unanswered; a resume that does not end */
static void test_failures(void)
{
  if (!reach(AT_SUSPENDED))
    return;
  u2ps_hmodel.stall_at = (int)u2ps_hmodel.requests;
  CHECK(u2ps_host_resume());
  CHECK(u2ps_hfake_wait(resumed_or_ended, 200));
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, U2PS_HOST_END_STALL);
  CHECK_EQ(happ_log.resumes, 0);

  if (!reach(AT_SUSPENDED))
    return;
  u2ps_hmodel.hang_at = (int)u2ps_hmodel.requests;
  CHECK(u2ps_host_resume());
  CHECK(u2ps_hfake_wait(resumed_or_ended, U2PS_HOST_CONTROL_MAX + 200u));
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, U2PS_HOST_END_TIMEOUT);
  CHECK_EQ(happ_log.resumes, 0);

  /* The port stays in resume: END_ERROR 50 ms after the first
   * resume(false), in the tick, with the port disabled */
  if (!reach(AT_SUSPENDED))
    return;
  u2ps_hfake_resume_ms = 100000u;
  unsigned n = u2ps_hfake_ncalls;
  CHECK(u2ps_host_resume());
  CHECK(u2ps_hfake_wait(resumed_or_ended, 200));
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, U2PS_HOST_END_ERROR);
  CHECK_EQ(happ_log.end_from, U2PS_HFAKE_FROM_MASKED);
  CHECK_EQ(happ_log.resumes, 0);
  int f = -1;
  for (unsigned i = n; i < u2ps_hfake_ncalls && f < 0; i++)
    if (u2ps_hfake_calls[i].kind == U2PS_HFAKE_RESUME && u2ps_hfake_calls[i].arg == 0u)
      f = (int)i;
  CHECK(f >= 0);
  if (f >= 0) {
    uint32_t end_ms = u2ps_hfake_now;
    CHECK(end_ms >= u2ps_hfake_calls[f].ms + 50u && end_ms <= u2ps_hfake_calls[f].ms + 51u);
  }
  CHECK_EQ(u2ps_hfake_get(U2PS_HFAKE_DISABLE, n).kind, U2PS_HFAKE_DISABLE);
  CHECK_EQ(u2ps_hmodel.get_status, 0);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* The test makes the port call the stack must never make while suspended */
static void test_fake_checks(void)
{
  if (!configured())
    return;
  CHECK(u2ps_host_suspend());
  static const uint8_t setup[8] = {0x80, U2PS_GET_STATUS, 0, 0, 0, 0, 2, 0};
  static uint8_t data[64];
  u2ps_hport_irq_mask(true);
  u2ps_hport_control(1, 64, setup, data);
  u2ps_hport_irq_mask(false);
  /* That one, and the test's SETUP packet and data stage, which lie
   * outside the DMA memory the stack checked at init */
  CHECK_EQ(u2ps_hfake_violations, 3);
  u2ps_hfake_violations = 0;
}

int main(void)
{
  static const uint32_t modes[3] = {0, 20, 25};
  for (unsigned m = 0; m < 3u; m++) {
    mode = modes[m];
    printf("port model: resume ends %u ms after the K began%s\n", (unsigned)mode,
           mode == 0u ? " (DWC2: at once)" : " (ChipIdea)");
    test_refusals();
    test_suspend_and_resume();
    test_wakeup();
    test_wakeup_during_resume();
    test_wakeup_not_suspended();
    test_ends();
    test_failures();
  }
  test_fake_checks();
  return check_done();
}
