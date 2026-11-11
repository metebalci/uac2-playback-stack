/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * contract_log.h: the tests' u2ps_app_on_contract(), for the libraries
 * built with U2PS_CONTRACT_CHECKS=1. It counts each violation. A test that
 * does not expect one fails: the hook prints a line that CTest's
 * FAIL_REGULAR_EXPRESSION matches (tests/CMakeLists.txt). A test that
 * provokes one calls contract_expect() first.
 */
#ifndef CONTRACT_LOG_H
#define CONTRACT_LOG_H

#include <stdbool.h>

#include "u2ps_types.h"

#define CONTRACT_CODES (U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED + 1)

typedef struct {
  unsigned total;                   /* every report */
  unsigned count[CONTRACT_CODES];   /* per u2ps_contract_t */
  u2ps_contract_t last;             /* the last one reported */
} contract_log_t;
extern contract_log_t contract_log;

/* Clear the log; from now on reports are expected (true) or fail the test
 * (false, the default) */
void contract_expect(bool expected);

/* n with the contract checks, 0 without: the count a test that provokes n
 * violations finds in contract_log, built against either library */
#define CONTRACT_N(n) (U2PS_CONTRACT_CHECKS ? (n) : 0)

#if U2PS_CONTRACT_CHECKS
void u2ps_app_on_contract(u2ps_contract_t violation);
#endif

#endif /* CONTRACT_LOG_H */
