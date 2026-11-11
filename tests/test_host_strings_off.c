/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_host_strings_off.c: the host library built with U2PS_HOST_STRINGS=0
 * asks for no string descriptor, and its device's strings are NULL.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "host_support.h"
#include "u2ps_ch9.h"

int main(void)
{
  CHECK_EQ(U2PS_HOST_STRINGS, 0);
  if (!hstart("uac2-hs-default.txt"))
    return check_done();
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK(happ_log.dev.manufacturer == NULL);
  CHECK(happ_log.dev.product == NULL);
  CHECK(happ_log.dev.serial_number == NULL);
  CHECK_EQ(happ_log.dev.id_vendor, 0x1234);
  /* Device (twice), SET_ADDRESS, configuration (twice), SET_CONFIGURATION,
   * RANGE, CUR, Clock Validity: no string */
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), 9);
  for (unsigned k = 0; k < u2ps_hfake_count(U2PS_HFAKE_CONTROL); k++)
    CHECK(u2ps_hfake_control_nth(k)->setup.wValue >> 8 != U2PS_DESC_STRING ||
          u2ps_hfake_control_nth(k)->setup.bRequest != U2PS_GET_DESCRIPTOR);
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_hmodel.unexpected, 0);
  return check_done();
}
