/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dwc2_old_rule.c: the DWC2 logic built with U2PS_DWC2_OLD_RULE,
 * the re-arm rule before B-next, which an injection build may select for
 * a comparison: every arm is for parity(now + 1), so a completion handled
 * in P + 1 loses P + 1 (LATE 1) where B-next loses nothing.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "u2ps_dwc2_iso.h"

static u2ps_uframe_t uf(long v)
{
  return (u2ps_uframe_t)((unsigned long)v & U2PS_UFRAME_MASK);
}

int main(void)
{
  unsigned bad = 0;
  for (long p = 0; p <= (long)U2PS_UFRAME_MASK; p++) {
    for (long d = 0; d <= 3; d++) {
      u2ps_dwc2_bnext_t a = u2ps_dwc2_bnext(uf(p), uf(p + d));
      if (a.odd != ((uf(p + d + 1) & 1u) != 0u) || a.late != (uint16_t)d)
        bad++;
    }
  }
  CHECK_EQ(bad, 0);

  /* The stream: a packet in 101, its arm decided in 102 */
  u2ps_dwc2_out_t o;
  u2ps_dwc2_out_packet_t r;
  u2ps_dwc2_bnext_t a;
  bool odd;
  memset(&o, 0, sizeof o);
  CHECK(u2ps_dwc2_out_open(&o, 4, 100, &odd));
  CHECK_EQ(odd, true);
  CHECK(u2ps_dwc2_out_complete(&o, 101, false, &r));
  CHECK(u2ps_dwc2_out_arm(&o, 102, &a));
  CHECK_EQ(a.odd, true); /* 103: 102 is lost */
  CHECK_EQ(a.late, 1);
  CHECK_EQ(o.late_completions, 1);
  CHECK_EQ(o.late_total, 1);
  return check_done();
}
