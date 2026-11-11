/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * contract_log.c: the tests' u2ps_app_on_contract(). See contract_log.h.
 */
#include "contract_log.h"

#include <stdio.h>
#include <string.h>

contract_log_t contract_log;
static bool expected;

void contract_expect(bool e)
{
  memset(&contract_log, 0, sizeof contract_log);
  expected = e;
}

#if U2PS_CONTRACT_CHECKS
void u2ps_app_on_contract(u2ps_contract_t violation)
{
  contract_log.total++;
  if ((unsigned)violation < CONTRACT_CODES)
    contract_log.count[violation]++;
  contract_log.last = violation;
  if (!expected)
    printf("unexpected u2ps contract violation %d\n", (int)violation);
}
#endif
