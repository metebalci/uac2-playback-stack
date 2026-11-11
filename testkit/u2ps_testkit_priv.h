/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_testkit_priv.h: what the kit's sides share with its common part,
 * u2ps_testkit.c. Not for the app.
 */
#ifndef U2PS_TESTKIT_PRIV_H
#define U2PS_TESTKIT_PRIV_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_testkit.h"

/* A callback begins: records it with its arguments; returns its index in
 * the trace (or one beyond the kept), for u2ps_testkit_record_result() */
unsigned u2ps_testkit_record(u2ps_testkit_callback_t callback, uint32_t a0, uint32_t a1,
                             uint32_t a2, uint32_t a3);
/* What the callback of that index returned */
void u2ps_testkit_record_result(unsigned index, int32_t result);
/* A stack reported a contract violation (stack: 0 device, 1 host) */
void u2ps_testkit_record_contract(u2ps_contract_t violation, unsigned stack);

/* A side's fake port: the count of its port violations, and their clear */
typedef struct {
  unsigned (*count)(void);
  void (*clear)(void);
} u2ps_testkit_port_t;
/* A side's reset: its port joins the record (side: 0 device, 1 host); the
 * violations it holds now are added to the record before its state is
 * reset. Also clears the trace and sets the time to 0. */
void u2ps_testkit_side_reset(unsigned side, const u2ps_testkit_port_t *port);

/* The kit's time; the poll the app gave. u2ps_testkit_poll_run() calls
 * it with now_ms and returns true, or returns false without a poll. */
void u2ps_testkit_set_now(uint32_t now_ms);
bool u2ps_testkit_poll_run(uint32_t now_ms);

/* The host side's record of what the device received (u2ps_testkit_host.h):
 * a packet that arrived intact, which the app's hook then sees, and one
 * that arrived damaged. The loopback's DAC app reports here too. */
void u2ps_testkit_host_deliver(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe);
void u2ps_testkit_host_deliver_corrupted(void);

#endif /* U2PS_TESTKIT_PRIV_H */
