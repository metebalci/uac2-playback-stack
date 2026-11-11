/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_example_stride.c: the example app's class memory layout
 * (examples/nucleo_u5a5zj_q/app_class_mem.h) against a port's
 * configuration header. For every packet size from 1 to 1024 bytes the
 * stride is a multiple of U2PS_DPORT_ALIGN (u2ps_stream_cfg_t), holds the
 * port's least stride and then the canary word, and is no larger than the
 * stride for 1024 bytes, which sizes the example's class memory; on DWC2
 * the port's own class memory check accepts it. Built once per
 * configuration header: STM32U5 (ALIGN 4), STM32F723 (ALIGN 32) and
 * i.MX RT1062 (ALIGN 32).
 */
#include <stdio.h>

#include "app_class_mem.h"
#include "check.h"
#include "u2ps_dport_cfg.h"
#if defined(TEST_DWC2)
#include "u2ps_dwc2_iso.h"
#endif

int main(void)
{
  unsigned bad_align = 0, bad_room = 0, bad_max = 0, bad_port = 0;
  for (unsigned mps = 1; mps <= 1024u; mps++) {
    unsigned s = APP_STRIDE(mps);
    if (s % U2PS_DPORT_ALIGN != 0u)
      bad_align++;
    if (s < U2PS_DPORT_STRIDE(mps) + APP_CANARY_BYTES)
      bad_room++;
    if (s > APP_STRIDE(1024u))
      bad_max++;
#if defined(TEST_DWC2)
    if (u2ps_dwc2_mem_check(0x20000000u, (uint16_t)s, U2PS_DPORT_ISO_OUT_MIN, (uint16_t)mps,
                            U2PS_DPORT_ISO_OUT_MIN, 16, U2PS_DPORT_ALIGN) != U2PS_DWC2_OPEN_OK)
      bad_port++;
#endif
  }
  printf("  U2PS_DPORT_ALIGN %u: stride %u for 1 byte, %u for 1024\n", U2PS_DPORT_ALIGN,
         (unsigned)APP_STRIDE(1u), (unsigned)APP_STRIDE(1024u));
  CHECK_EQ(U2PS_DPORT_ALIGN, TEST_ALIGN); /* the port's header, not the fake's */
  CHECK_EQ(bad_align, 0);
  CHECK_EQ(bad_room, 0);
  CHECK_EQ(bad_max, 0);
  CHECK_EQ(bad_port, 0);
  return check_done();
}
