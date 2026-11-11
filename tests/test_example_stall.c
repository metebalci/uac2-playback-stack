/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_example_stall.c: the example app's stall mode
 * (examples/nucleo_u5a5zj_q/app_stall.h): when a stall is due, how many
 * packets the app holds (count - U2PS_DPORT_ISO_OUT_RESERVE), what it
 * releases at the stall's end, and the tail, the holes since the last
 * packet.
 */
#include "app_stall.h"
#include "check.h"

static void test_due(void)
{
  CHECK(!app_stall_due(0));
  CHECK(!app_stall_due(APP_STALL_PERIOD_MS - APP_STALL_HOLD_MS - 1u));
  CHECK(app_stall_due(APP_STALL_PERIOD_MS - APP_STALL_HOLD_MS));
  CHECK(app_stall_due(APP_STALL_PERIOD_MS - 1u));
  CHECK(!app_stall_due(APP_STALL_PERIOD_MS));
  CHECK(app_stall_due(2u * APP_STALL_PERIOD_MS - 1u));
  unsigned due = 0;
  for (uint32_t ms = 0; ms < 10u * APP_STALL_PERIOD_MS; ms++)
    due += app_stall_due(ms) ? 1u : 0u;
  CHECK_EQ(due, 10u * APP_STALL_HOLD_MS);
}

static void test_hold(void)
{
  app_stall_t s;
  app_stall_start(&s, 8);
  CHECK_EQ(s.limit, 8u - U2PS_DPORT_ISO_OUT_RESERVE);
  /* No stall: every packet goes back at once */
  for (unsigned i = 0; i < 20u; i++)
    CHECK(!app_stall_packet(&s));
  CHECK_EQ(s.held, 0u);
  CHECK_EQ(app_stall_end(&s), 0u);

  app_stall_begin(&s);
  app_stall_begin(&s); /* in progress: not a new stall */
  CHECK_EQ(s.stalls, 1u);
  CHECK(s.holding);
  for (unsigned i = 0; i < s.limit; i++)
    CHECK(app_stall_packet(&s));
  CHECK_EQ(s.held, s.limit);
  CHECK_EQ(s.over, 0u);
  /* The port must not deliver more; one that comes goes back at once */
  CHECK(!app_stall_packet(&s));
  CHECK_EQ(s.over, 1u);
  CHECK_EQ(s.held, s.limit);
  CHECK_EQ(app_stall_end(&s), 8u - U2PS_DPORT_ISO_OUT_RESERVE);
  CHECK(!s.holding);
  CHECK_EQ(s.held, 0u);
  CHECK(!app_stall_packet(&s));

  app_stall_begin(&s);
  CHECK_EQ(s.stalls, 2u);
  CHECK(app_stall_packet(&s));
  CHECK(app_stall_packet(&s));
  CHECK_EQ(app_stall_end(&s), 2u);
  CHECK_EQ(s.held_max, 8u - U2PS_DPORT_ISO_OUT_RESERVE);

  /* A new stream starts clean */
  app_stall_start(&s, U2PS_DPORT_ISO_OUT_MIN);
  CHECK_EQ(s.limit, U2PS_DPORT_ISO_OUT_MIN - U2PS_DPORT_ISO_OUT_RESERVE);
  CHECK_EQ(s.stalls, 0u);
  CHECK_EQ(s.held_max, 0u);
  CHECK_EQ(s.over, 0u);
  CHECK(!s.holding);
}

static void test_tail(void)
{
  app_stall_t s;
  app_stall_start(&s, 8);
  app_stall_hole(&s, 3, U2PS_MISS_NO_PACKET);
  app_stall_hole(&s, 5, U2PS_MISS_NO_BUFFER);
  CHECK_EQ(s.tail[U2PS_MISS_NO_PACKET], 1u);
  CHECK_EQ(s.tail_uframes[U2PS_MISS_NO_BUFFER], 5u);
  /* A packet ends them: they were not the tail */
  (void)app_stall_packet(&s);
  for (unsigned i = 0; i < 4u; i++) {
    CHECK_EQ(s.tail[i], 0u);
    CHECK_EQ(s.tail_uframes[i], 0u);
  }
  app_stall_hole(&s, 65535, U2PS_MISS_NO_BUFFER);
  app_stall_hole(&s, 7, U2PS_MISS_NO_BUFFER);
  app_stall_hole(&s, 2, U2PS_MISS_LATE);
  app_stall_hole(&s, 1, U2PS_MISS_CORRUPTED);
  app_stall_hole(&s, 9, (u2ps_miss_t)4); /* no such reason: ignored */
  CHECK_EQ(s.tail[U2PS_MISS_NO_BUFFER], 2u);
  CHECK_EQ(s.tail_uframes[U2PS_MISS_NO_BUFFER], 65542u);
  CHECK_EQ(s.tail[U2PS_MISS_LATE], 1u);
  CHECK_EQ(s.tail_uframes[U2PS_MISS_LATE], 2u);
  CHECK_EQ(s.tail[U2PS_MISS_CORRUPTED], 1u);
  CHECK_EQ(s.tail[U2PS_MISS_NO_PACKET], 0u);
  /* A held packet ends them too */
  app_stall_begin(&s);
  CHECK(app_stall_packet(&s));
  CHECK_EQ(s.tail[U2PS_MISS_NO_BUFFER], 0u);
}

int main(void)
{
  test_due();
  test_hold();
  test_tail();
  return check_done();
}
