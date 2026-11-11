/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_testkit.h: the PC test kit of u2ps, for an integrator who tests
 * its own app against u2ps on a PC, without a board. PC build only: the
 * kit is never part of an ARM build.
 *
 * Link one kit library instead of the stack's library (never both):
 *   u2ps_testkit_device    a DAC app (its u2ps_app_* callbacks) against the
 *                          device stack; the kit plays the device port and
 *                          the host (u2ps_testkit_dev.h)
 *   u2ps_testkit_host      a player app (its u2ps_app_host_* callbacks)
 *                          against the host stack; the kit plays the host
 *                          port and a scripted UAC2 device
 *                          (u2ps_testkit_host.h)
 *   u2ps_testkit_loopback  a player app against the host stack, which plays
 *                          through a bus that steps microframes to the u2ps
 *                          device stack and the kit's own DAC app
 *                          (u2ps_testkit_loopback.h)
 * Each library holds the stack built with the configure's options and
 * build constants (U2PS_VBUS, U2PS_CONTROL_HOOK, U2PS_EP0_BUF, ...), with
 * the contract checks on whatever U2PS_CONTRACT_CHECKS says: the app's
 * header declares u2ps_app_on_contract(), and the app defines it. Each
 * library also brings AddressSanitizer and UndefinedBehaviorSanitizer to
 * the test executable, compile and link options both.
 *
 * The app's callbacks run as on the board: in the kit's injections, which
 * stand for the port's interrupt, or inside the app's own thread calls.
 * The app calls the stack as on the board, from its thread: the test's
 * main(), or the poll function it gives (u2ps_testkit_set_poll).
 *
 * What the kit records, from the side's reset on:
 * - the trace: every app callback, in order, with its arguments and what
 *   it returned (u2ps_testkit_trace_*);
 * - every contract violation the stacks report (u2ps_contract_t), before
 *   the kit passes it on to the app's u2ps_app_on_contract();
 * - every port violation: a rule of the port interface broken (by the
 *   stack, or by the app's misuse of it), or a mistake of the test's
 *   script, such as a packet injected without a stream. The fake port
 *   prints each with its reason.
 * u2ps_testkit_result() fails if any contract violation or port violation
 * is left in the record, or if a U2PS_TESTKIT_CHECK failed.
 */
#ifndef U2PS_TESTKIT_H
#define U2PS_TESTKIT_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_types.h"

/* ---- Checks and the result --------------------------------------------------- */

/* Counts a check; a failed one prints
 * "u2ps testkit: CHECK FAILED at <file>:<line>: <cond>". Evaluates to
 * cond. */
#define U2PS_TESTKIT_CHECK(cond) u2ps_testkit_check((cond), #cond, __FILE__, __LINE__)
bool u2ps_testkit_check(bool ok, const char *cond, const char *file, int line);

/* The end of a test: prints one line,
 *   u2ps testkit: PASS: <n> checks, 0 failed, 0 contract violations, 0 port violations
 * or the same with FAIL and the counts, and returns 0 (PASS) or 1 (FAIL),
 * the test's exit code: return u2ps_testkit_result(); from main(). */
int u2ps_testkit_result(void);

/* ---- The trace of the app's callbacks ---------------------------------------- */

/* An app callback; its name in the trace is the function's name without
 * u2ps_app_ (on_packet, host_on_sent). arg[] holds its arguments in this
 * order, as uint32_t; result what it returned (0 for a void callback). */
typedef enum {
  /* Device stack (u2ps_dev.h) */
  U2PS_TESTKIT_ON_REQUEST,            /* bmRequestType, bRequest, wValue, wIndex; result */
  U2PS_TESTKIT_ON_SAMPLING_FREQUENCY, /* requested, chosen, id; result */
  U2PS_TESTKIT_ON_CONFIGURED,         /* configured (0, 1), why (u2ps_config_end_t) */
  /* info->stream, info->alternate_setting, info->sampling_frequency,
   * info->max_packet_size; result (u2ps_stream_start_t) */
  U2PS_TESTKIT_ON_STREAM_START,
  U2PS_TESTKIT_ON_PACKET,             /* len, uframe */
  U2PS_TESTKIT_ON_HOLE,               /* uframes, why (u2ps_miss_t) */
  U2PS_TESTKIT_ON_PAUSE,              /* uframes */
  U2PS_TESTKIT_ON_STREAM_STOP,        /* none */
  U2PS_TESTKIT_ON_FAULT,              /* fault (u2ps_fault_t) */
  U2PS_TESTKIT_ON_RESET,              /* none */
  U2PS_TESTKIT_ON_SUSPEND,            /* none */
  U2PS_TESTKIT_ON_RESUME,             /* none */
  U2PS_TESTKIT_ON_VBUS,               /* present (0, 1); not in VBUS mode bus */
  /* setup->bmRequestType, bRequest, wValue, wIndex; result: the result
   * argument (only with U2PS_CONTROL_HOOK) */
  U2PS_TESTKIT_ON_CONTROL,
  /* Host stack (u2ps_host.h) */
  /* dev->id_vendor, dev->id_product, dev->alternate_setting_count,
   * dev->sampling_frequency */
  U2PS_TESTKIT_HOST_ON_DEVICE,
  U2PS_TESTKIT_HOST_ON_END,           /* why (u2ps_host_end_t), refusal (u2ps_host_refusal_t) */
  /* info->stream, info->alternate_setting, info->sampling_frequency,
   * info->max_packet_size */
  U2PS_TESTKIT_HOST_ON_STREAM_START,
  U2PS_TESTKIT_HOST_ON_SENT,          /* uframe */
  U2PS_TESTKIT_HOST_ON_HOLE,          /* uframes, why (u2ps_miss_t) */
  /* why (u2ps_host_stop_t), request (u2ps_host_stop_request_t) */
  U2PS_TESTKIT_HOST_ON_STREAM_STOP,
  U2PS_TESTKIT_HOST_ON_TEST_DEVICE,   /* id_product, bcd_device */
  U2PS_TESTKIT_HOST_ON_RESUMED,       /* remote_wakeup (0, 1) */
  /* Either stack: violation (u2ps_contract_t), stack (0 device, 1 host) */
  U2PS_TESTKIT_ON_CONTRACT
} u2ps_testkit_callback_t;

/* Number of u2ps_testkit_callback_t values */
#define U2PS_TESTKIT_CALLBACKS (U2PS_TESTKIT_ON_CONTRACT + 1)

/* One callback in the trace */
typedef struct {
  u2ps_testkit_callback_t callback;
  uint32_t arg[4];
  int32_t result;
} u2ps_testkit_call_t;

/* Calls the trace keeps since its clear; later ones are counted only */
#define U2PS_TESTKIT_TRACE_MAX 4096u

/* Clears the trace and the counts of u2ps_testkit_calls(). The side's
 * reset clears it too. */
void u2ps_testkit_trace_clear(void);
/* Callbacks since the clear, kept or not */
unsigned u2ps_testkit_trace_count(void);
/* The ith callback since the clear (0 first), or NULL beyond the kept */
const u2ps_testkit_call_t *u2ps_testkit_trace_get(unsigned i);
/* Calls of one callback since the clear */
unsigned u2ps_testkit_calls(u2ps_testkit_callback_t callback);
/* The trace since the clear against 'expected': callback names separated
 * by spaces, in order, each optionally followed by *n for n calls in a
 * row ("on_configured on_stream_start on_packet*3 on_hole on_packet").
 * True if they are equal; otherwise it prints both and returns false. */
bool u2ps_testkit_trace_match(const char *expected);
/* Prints the kept trace, one callback per line with its arguments as
 * key=value: "u2ps testkit: trace 3: on_packet len=192 uframe=17" */
void u2ps_testkit_trace_print(void);
/* The callback's name in the trace ("on_packet"), or "?" */
const char *u2ps_testkit_callback_name(u2ps_testkit_callback_t callback);

/* ---- Contract violations and port violations ---------------------------------- */

/* Contract violations in the record; the kit prints each as it comes:
 * "u2ps testkit: contract violation U2PS_CONTRACT_DEV_RELEASE_COUNT
 * (device stack)" */
unsigned u2ps_testkit_contract_count(void);
/* Takes the violations of one code out of the record and returns how many
 * there were: for a test that breaks a contract on purpose. The rest stay
 * and fail u2ps_testkit_result(). */
unsigned u2ps_testkit_contract_take(u2ps_contract_t violation);
/* The code's name ("U2PS_CONTRACT_DEV_RELEASE_COUNT"), or "?" */
const char *u2ps_testkit_contract_name(u2ps_contract_t violation);
/* Port violations in the record (each printed by the fake port) */
unsigned u2ps_testkit_port_violations(void);
/* Takes them all out of the record and returns how many there were */
unsigned u2ps_testkit_port_violations_take(void);

/* ---- Time ---------------------------------------------------------------------- */

/* The app's main-loop pass, which the kit calls once per millisecond of
 * its time with the time, from the app's thread: it calls u2ps_dev_tick()
 * or u2ps_host_tick() with now_ms, and whatever else the app's loop does.
 * NULL (the default): the kit calls the stack's tick itself. */
void u2ps_testkit_set_poll(void (*poll)(uint32_t now_ms));
/* The kit's time in ms since the side's reset */
uint32_t u2ps_testkit_now_ms(void);

#endif /* U2PS_TESTKIT_H */
