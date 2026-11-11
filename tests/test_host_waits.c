/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_host_waits.c: every wait of the enumeration and the resume is a
 * lower bound (u2ps_host.h, u2ps_host_tick()), with the app's millisecond
 * counter read at any phase. The test keeps a real time in microseconds:
 * the fake's step m ticks at m * 1000 + j(m), with j(m) in 0..999 from a
 * pseudo-random sequence per run, and the counter the stack reads there is
 * m. An upcall in step m comes before that step's tick, so its real time
 * is at most the tick's; a thread call between steps m and m + 1 at most
 * the tick of m + 1. Each wait, from the latest real time its start can
 * have to the real time of the port call that ends it, must last at least
 * its value and at most 2 ms more (one step of 1 ms between ticks and the
 * counter's resolution):
 * - TATTDB 100 ms: the connect to reset(true) (USB 2.0 7.1.7.3);
 * - TDRSTR 50 ms: reset(true) to reset(false) (7.1.7.5);
 * - TRSTRCY 10 ms: the reset's end (on_enabled) to the first request
 *   (9.2.6.2);
 * - 2 ms after SET_ADDRESS's completion to the next request (9.2.6.3);
 * - TDRSMDN 20 ms: resume(true), and a remote wakeup, to the first
 *   resume(false) (7.1.7.7);
 * - TRSMRCY 10 ms: the resume's end to GET_STATUS (7.1.7.7).
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "host_support.h"
#include "u2ps_ch9.h"

#define STEPS 4096u
#define RUNS  64u

static uint32_t real_us[STEPS]; /* the real time of each step's tick */
static uint32_t seed;

static uint32_t rnd(void)
{
  seed = seed * 1664525u + 1013904223u;
  return seed >> 8;
}

static void tick(uint32_t now_ms)
{
  if (now_ms < STEPS)
    real_us[now_ms] = now_ms * 1000u + rnd() % 1000u;
  u2ps_host_tick(now_ms);
}

static uint32_t at(uint32_t ms)
{
  return ms < STEPS ? real_us[ms] : 0xFFFFFFFFu;
}

/* The shortest and the longest of each wait over all runs, in us */
enum { W_ATTDB, W_DRSTR, W_RSTRCY, W_SETADR, W_DRSMDN, W_WAKEUP, W_RSMRCY, W_COUNT };
static const char *const names[W_COUNT] = {"TATTDB", "TDRSTR", "TRSTRCY", "SET_ADDRESS",
                                           "TDRSMDN", "TDRSMDN (wakeup)", "TRSMRCY"};
static const uint32_t value_ms[W_COUNT] = {100, 50, 10, 2, 20, 20, 10};
static uint32_t lo[W_COUNT], hi[W_COUNT], seen[W_COUNT];

static void wait_is(unsigned w, uint32_t start_ms, uint32_t end_ms)
{
  uint32_t d = at(end_ms) - at(start_ms);
  if (end_ms <= start_ms || end_ms >= STEPS)
    d = 0;
  if (seen[w] == 0u || d < lo[w])
    lo[w] = d;
  if (seen[w] == 0u || d > hi[w])
    hi[w] = d;
  seen[w]++;
}

/* The first call of 'kind' (and arg, if arg >= 0) at or after index from */
static int find(u2ps_hfake_kind_t kind, int arg, unsigned from)
{
  for (unsigned i = from; i < u2ps_hfake_ncalls; i++)
    if (u2ps_hfake_calls[i].kind == kind && (arg < 0 || u2ps_hfake_calls[i].arg == (uint32_t)arg))
      return (int)i;
  return -1;
}

static bool resumed(void)
{
  return happ_log.resumes > 0u || happ_log.ends > 0u;
}

/* The resume that began at real-time step 'from_ms' (the latest its start
 * can be), whose port calls start at log index 'from': TDRSMDN to the
 * first resume(false), TRSMRCY from the true one to GET_STATUS */
static void resume_waits(unsigned w_k, uint32_t from_ms, unsigned from)
{
  CHECK(u2ps_hfake_wait(resumed, 200));
  CHECK_EQ(happ_log.resumes, 1);
  int f = find(U2PS_HFAKE_RESUME, 0, from);
  int t = f;
  while (t >= 0 && !u2ps_hfake_calls[t].ret)
    t = find(U2PS_HFAKE_RESUME, 0, (unsigned)t + 1u);
  int g = t >= 0 ? find(U2PS_HFAKE_CONTROL, -1, (unsigned)t) : -1;
  CHECK(f >= 0 && t >= 0 && g >= 0);
  if (f < 0 || t < 0 || g < 0)
    return;
  CHECK_EQ(u2ps_hfake_calls[g].setup.bRequest, U2PS_GET_STATUS);
  wait_is(w_k, from_ms, u2ps_hfake_calls[f].ms);
  wait_is(W_RSMRCY, u2ps_hfake_calls[t].ms, u2ps_hfake_calls[g].ms);
}

static void run(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hfake_tick = tick;
  memset(real_us, 0, sizeof real_us);

  /* The connect, as the port's interrupt between steps 0 and 1 */
  uint32_t connect_ms = u2ps_hfake_now + 1u;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  int r1 = find(U2PS_HFAKE_RESET, 1, 0);
  int r0 = r1 >= 0 ? find(U2PS_HFAKE_RESET, 0, (unsigned)r1) : -1;
  int c0 = r0 >= 0 ? find(U2PS_HFAKE_CONTROL, -1, (unsigned)r0) : -1;
  CHECK(r1 >= 0 && r0 >= 0 && c0 >= 0);
  if (r1 < 0 || r0 < 0 || c0 < 0)
    return;
  wait_is(W_ATTDB, connect_ms, u2ps_hfake_calls[r1].ms);
  wait_is(W_DRSTR, u2ps_hfake_calls[r1].ms, u2ps_hfake_calls[r0].ms);
  /* on_enabled comes at the step after reset(false) (the fake's DWC2
   * model), before that step's tick */
  wait_is(W_RSTRCY, u2ps_hfake_calls[r0].ms + 1u, u2ps_hfake_calls[c0].ms);
  /* SET_ADDRESS completes in an upcall at done_ms; the next request */
  int a = c0;
  while (a >= 0 && u2ps_hfake_calls[a].setup.bRequest != U2PS_SET_ADDRESS)
    a = find(U2PS_HFAKE_CONTROL, -1, (unsigned)a + 1u);
  int n = a >= 0 ? find(U2PS_HFAKE_CONTROL, -1, (unsigned)a + 1u) : -1;
  CHECK(a >= 0 && n >= 0);
  if (a >= 0 && n >= 0)
    wait_is(W_SETADR, u2ps_hfake_calls[a].done_ms, u2ps_hfake_calls[n].ms);

  /* The app's suspend and resume, from thread between two steps */
  CHECK(u2ps_host_suspend());
  u2ps_hfake_run_until(u2ps_hfake_now + 5u + rnd() % 7u);
  unsigned from = u2ps_hfake_ncalls;
  uint32_t resume_ms = u2ps_hfake_now + 1u;
  CHECK(u2ps_host_resume());
  resume_waits(W_DRSMDN, resume_ms, from);

  /* A remote wakeup, as the port's interrupt between two steps */
  happ_log.resumes = 0;
  CHECK(u2ps_host_suspend());
  u2ps_hfake_run_until(u2ps_hfake_now + 5u + rnd() % 7u);
  from = u2ps_hfake_ncalls;
  uint32_t wakeup_ms = u2ps_hfake_now + 1u;
  u2ps_hfake_wakeup();
  resume_waits(W_WAKEUP, wakeup_ms, from);
  CHECK_EQ(happ_log.ends, 0);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

int main(void)
{
  for (unsigned i = 0; i < RUNS; i++) {
    seed = 0x9E3779B9u * (i + 1u);
    run();
  }
  for (unsigned w = 0; w < W_COUNT; w++) {
    printf("%s (%u ms): %u waits, shortest %u us, longest %u us\n", names[w],
           (unsigned)value_ms[w], (unsigned)seen[w], (unsigned)lo[w], (unsigned)hi[w]);
    CHECK_EQ(seen[w], w == W_RSMRCY ? 2u * RUNS : RUNS);
    CHECK(lo[w] >= value_ms[w] * 1000u);
    CHECK(hi[w] <= (value_ms[w] + 2u) * 1000u);
  }
  return check_done();
}
