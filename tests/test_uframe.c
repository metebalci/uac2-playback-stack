/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_uframe.c: microframe arithmetic modulo 2^14, its wrap at 0x3FFF and
 * the frame/microframe split (USB 2.0 8.3.3, 8.4.3.1, 10.2.3).
 */
#include "check.h"
#include "u2ps_uframe.h"

static void test_constants(void)
{
  CHECK_EQ(U2PS_UFRAME_COUNT, 16384);
  CHECK_EQ(U2PS_UFRAME_MASK, 0x3FFF);
  CHECK_EQ(U2PS_FRAME_MASK, 0x7FF);
}

static void test_split(void)
{
  CHECK_EQ(u2ps_uframe_frame(0x0000), 0);
  CHECK_EQ(u2ps_uframe_micro(0x0000), 0);
  CHECK_EQ(u2ps_uframe_frame(0x3FFF), 0x7FF);
  CHECK_EQ(u2ps_uframe_micro(0x3FFF), 7);
  CHECK_EQ(u2ps_uframe_frame(0x091D), 0x123);
  CHECK_EQ(u2ps_uframe_micro(0x091D), 5);

  CHECK_EQ(u2ps_uframe_make(0x123, 5), 0x091D);
  CHECK_EQ(u2ps_uframe_make(0x7FF, 7), 0x3FFF);
  CHECK_EQ(u2ps_uframe_make(0x000, 0), 0x0000);
  /* Arguments are taken modulo their range */
  CHECK_EQ(u2ps_uframe_make(0x800, 0), 0x0000);
  CHECK_EQ(u2ps_uframe_make(0x801, 9), 0x0009);

  /* Every number splits and joins back */
  for (unsigned u = 0; u <= U2PS_UFRAME_MASK; u++) {
    u2ps_uframe_t v = (u2ps_uframe_t)u;
    CHECK_EQ(u2ps_uframe_make(u2ps_uframe_frame(v), u2ps_uframe_micro(v)), v);
  }
}

static void test_add(void)
{
  CHECK_EQ(u2ps_uframe_add(0x0000, 1), 0x0001);
  CHECK_EQ(u2ps_uframe_add(0x1234, 0), 0x1234);
  CHECK_EQ(u2ps_uframe_add(0x3FFF, 1), 0x0000);
  CHECK_EQ(u2ps_uframe_add(0x3FFE, 3), 0x0001);
  CHECK_EQ(u2ps_uframe_add(0x3FFF, 0x3FFF), 0x3FFE);
  CHECK_EQ(u2ps_uframe_add(0x2000, 0x2000), 0x0000);
  CHECK_EQ(u2ps_uframe_add(0x0005, 0x4000), 0x0005);
  CHECK_EQ(u2ps_uframe_add(0x0005, 0xFFFF), 0x0004);

  /* Microframe 7 of frame 7FFh is followed by microframe 0 of frame 0 */
  u2ps_uframe_t next = u2ps_uframe_add(u2ps_uframe_make(0x7FF, 7), 1);
  CHECK_EQ(u2ps_uframe_frame(next), 0);
  CHECK_EQ(u2ps_uframe_micro(next), 0);
  /* Eight microframes make one frame */
  CHECK_EQ(u2ps_uframe_add(u2ps_uframe_make(0x7FF, 3), 8), u2ps_uframe_make(0x000, 3));
}

static void test_sub(void)
{
  CHECK_EQ(u2ps_uframe_sub(0x0001, 1), 0x0000);
  CHECK_EQ(u2ps_uframe_sub(0x0000, 1), 0x3FFF);
  CHECK_EQ(u2ps_uframe_sub(0x0001, 3), 0x3FFE);
  CHECK_EQ(u2ps_uframe_sub(0x1234, 0x1234), 0x0000);
  CHECK_EQ(u2ps_uframe_sub(0x0005, 0x4000), 0x0005);
  CHECK_EQ(u2ps_uframe_sub(0x0000, 0xFFFF), 0x0001);

  /* Forward distance from n to u, across the wrap */
  CHECK_EQ(u2ps_uframe_sub(0x0002, 0x3FFE), 4);
  CHECK_EQ(u2ps_uframe_sub(0x3FFE, 0x0002), 0x3FFC);
  CHECK_EQ(u2ps_uframe_sub(0x0000, 0x3FFF), 1);
}

static void test_dist(void)
{
  CHECK_EQ(u2ps_uframe_dist(0x1000, 0x1000), 0);
  CHECK_EQ(u2ps_uframe_dist(0x1000, 0x1001), 1);
  CHECK_EQ(u2ps_uframe_dist(0x1001, 0x1000), -1);

  /* Across the wrap, both directions */
  CHECK_EQ(u2ps_uframe_dist(0x3FFF, 0x0000), 1);
  CHECK_EQ(u2ps_uframe_dist(0x0000, 0x3FFF), -1);
  CHECK_EQ(u2ps_uframe_dist(0x3FFC, 0x0003), 7);
  CHECK_EQ(u2ps_uframe_dist(0x0003, 0x3FFC), -7);
  CHECK_EQ(u2ps_uframe_dist(0x3FFF, 0x1FFE), 8191);
  CHECK_EQ(u2ps_uframe_dist(0x1FFE, 0x3FFF), -8191);

  /* The ends of the range: half the wrap counts as backwards */
  CHECK_EQ(u2ps_uframe_dist(0x0000, 0x1FFF), 8191);
  CHECK_EQ(u2ps_uframe_dist(0x1FFF, 0x0000), -8191);
  CHECK_EQ(u2ps_uframe_dist(0x0000, 0x2000), -8192);
  CHECK_EQ(u2ps_uframe_dist(0x2000, 0x0000), -8192);
  CHECK_EQ(u2ps_uframe_dist(0x3000, 0x0FFF), 8191);
  CHECK_EQ(u2ps_uframe_dist(0x3000, 0x1000), -8192);
}

/* For every microframe number: add and sub undo each other, and dist
 * recovers every step from -8192 to 8191 in either direction. */
static void test_all(void)
{
  static const int steps[] = {-8192, -8191, -4096, -9, -8, -7, -1, 0, 1, 7, 8, 9, 4096, 8191};
  unsigned long bad = 0;

  for (unsigned u = 0; u <= U2PS_UFRAME_MASK; u++) {
    u2ps_uframe_t a = (u2ps_uframe_t)u;

    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++) {
      int d = steps[i];
      u2ps_uframe_t b = d >= 0 ? u2ps_uframe_add(a, (uint16_t)d)
                                 : u2ps_uframe_sub(a, (uint16_t)-d);
      if (b > U2PS_UFRAME_MASK || u2ps_uframe_dist(a, b) != d ||
          u2ps_uframe_dist(b, a) != (d == -8192 ? -8192 : -d) ||
          u2ps_uframe_sub(b, a) != (u2ps_uframe_t)((unsigned)d & U2PS_UFRAME_MASK) ||
          u2ps_uframe_add(u2ps_uframe_sub(a, (uint16_t)d), (uint16_t)d) != a) {
        if (bad++ < 5)
          printf("u=0x%04X d=%d: b=0x%04X dist(a,b)=%d dist(b,a)=%d\n", u, d, b,
                 u2ps_uframe_dist(a, b), u2ps_uframe_dist(b, a));
      }
    }
  }
  CHECK_EQ(bad, 0);

  /* Every distance from one point on each side of the wrap */
  bad = 0;
  for (int d = -8192; d <= 8191; d++) {
    u2ps_uframe_t from = 0x3FF0;
    u2ps_uframe_t to = (u2ps_uframe_t)(((unsigned)from + (unsigned)d) & U2PS_UFRAME_MASK);
    if (u2ps_uframe_dist(from, to) != d)
      bad++;
  }
  CHECK_EQ(bad, 0);
}

int main(void)
{
  test_constants();
  test_split();
  test_add();
  test_sub();
  test_dist();
  test_all();
  return check_done();
}
