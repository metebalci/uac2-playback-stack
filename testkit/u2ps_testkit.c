/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_testkit.c: the PC test kit's common part: checks and the result,
 * the trace, the record of violations, the time. See u2ps_testkit.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u2ps_testkit.h"
#include "u2ps_testkit_priv.h"

/* ---- Names ------------------------------------------------------------------ */

static const char *const callback_names[U2PS_TESTKIT_CALLBACKS] = {
  [U2PS_TESTKIT_ON_REQUEST] = "on_request",
  [U2PS_TESTKIT_ON_SAMPLING_FREQUENCY] = "on_sampling_frequency",
  [U2PS_TESTKIT_ON_CONFIGURED] = "on_configured",
  [U2PS_TESTKIT_ON_STREAM_START] = "on_stream_start",
  [U2PS_TESTKIT_ON_PACKET] = "on_packet",
  [U2PS_TESTKIT_ON_HOLE] = "on_hole",
  [U2PS_TESTKIT_ON_PAUSE] = "on_pause",
  [U2PS_TESTKIT_ON_STREAM_STOP] = "on_stream_stop",
  [U2PS_TESTKIT_ON_FAULT] = "on_fault",
  [U2PS_TESTKIT_ON_RESET] = "on_reset",
  [U2PS_TESTKIT_ON_SUSPEND] = "on_suspend",
  [U2PS_TESTKIT_ON_RESUME] = "on_resume",
  [U2PS_TESTKIT_ON_VBUS] = "on_vbus",
  [U2PS_TESTKIT_ON_CONTROL] = "on_control",
  [U2PS_TESTKIT_HOST_ON_DEVICE] = "host_on_device",
  [U2PS_TESTKIT_HOST_ON_END] = "host_on_end",
  [U2PS_TESTKIT_HOST_ON_STREAM_START] = "host_on_stream_start",
  [U2PS_TESTKIT_HOST_ON_SENT] = "host_on_sent",
  [U2PS_TESTKIT_HOST_ON_HOLE] = "host_on_hole",
  [U2PS_TESTKIT_HOST_ON_STREAM_STOP] = "host_on_stream_stop",
  [U2PS_TESTKIT_HOST_ON_TEST_DEVICE] = "host_on_test_device",
  [U2PS_TESTKIT_HOST_ON_RESUMED] = "host_on_resumed",
  [U2PS_TESTKIT_ON_CONTRACT] = "on_contract",
};

/* The names of each callback's arguments, as the trace prints them; a
 * callback with a result prints it last */
static const char *const arg_names[U2PS_TESTKIT_CALLBACKS][4] = {
  [U2PS_TESTKIT_ON_REQUEST] = {"bmRequestType", "bRequest", "wValue", "wIndex"},
  [U2PS_TESTKIT_ON_SAMPLING_FREQUENCY] = {"requested", "chosen", "id", NULL},
  [U2PS_TESTKIT_ON_CONFIGURED] = {"configured", "why", NULL, NULL},
  [U2PS_TESTKIT_ON_STREAM_START] = {"stream", "alternate_setting", "sampling_frequency",
                                    "max_packet_size"},
  [U2PS_TESTKIT_ON_PACKET] = {"len", "uframe", NULL, NULL},
  [U2PS_TESTKIT_ON_HOLE] = {"uframes", "why", NULL, NULL},
  [U2PS_TESTKIT_ON_PAUSE] = {"uframes", NULL, NULL, NULL},
  [U2PS_TESTKIT_ON_FAULT] = {"fault", NULL, NULL, NULL},
  [U2PS_TESTKIT_ON_VBUS] = {"present", NULL, NULL, NULL},
  [U2PS_TESTKIT_ON_CONTROL] = {"bmRequestType", "bRequest", "wValue", "wIndex"},
  [U2PS_TESTKIT_HOST_ON_DEVICE] = {"id_vendor", "id_product", "alternate_setting_count",
                                   "sampling_frequency"},
  [U2PS_TESTKIT_HOST_ON_END] = {"why", "refusal", NULL, NULL},
  [U2PS_TESTKIT_HOST_ON_STREAM_START] = {"stream", "alternate_setting", "sampling_frequency",
                                         "max_packet_size"},
  [U2PS_TESTKIT_HOST_ON_SENT] = {"uframe", NULL, NULL, NULL},
  [U2PS_TESTKIT_HOST_ON_HOLE] = {"uframes", "why", NULL, NULL},
  [U2PS_TESTKIT_HOST_ON_STREAM_STOP] = {"why", "request", NULL, NULL},
  [U2PS_TESTKIT_HOST_ON_TEST_DEVICE] = {"id_product", "bcd_device", NULL, NULL},
  [U2PS_TESTKIT_HOST_ON_RESUMED] = {"remote_wakeup", NULL, NULL, NULL},
  [U2PS_TESTKIT_ON_CONTRACT] = {"violation", "stack", NULL, NULL},
};

/* Callbacks whose result the trace prints */
static bool has_result(u2ps_testkit_callback_t c)
{
  return c == U2PS_TESTKIT_ON_REQUEST || c == U2PS_TESTKIT_ON_SAMPLING_FREQUENCY ||
         c == U2PS_TESTKIT_ON_STREAM_START || c == U2PS_TESTKIT_ON_CONTROL;
}

#define CONTRACT_CODES (U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED + 1)

static const char *const contract_names[CONTRACT_CODES] = {
  [U2PS_CONTRACT_NONE] = "U2PS_CONTRACT_NONE",
  [U2PS_CONTRACT_DEV_CALL_IN_CALLBACK] = "U2PS_CONTRACT_DEV_CALL_IN_CALLBACK",
  [U2PS_CONTRACT_DEV_CALL_NESTED] = "U2PS_CONTRACT_DEV_CALL_NESTED",
  [U2PS_CONTRACT_DEV_CONNECT_BEFORE_INIT] = "U2PS_CONTRACT_DEV_CONNECT_BEFORE_INIT",
  [U2PS_CONTRACT_DEV_TICK_LATE] = "U2PS_CONTRACT_DEV_TICK_LATE",
  [U2PS_CONTRACT_DEV_DESCRIPTION_CHANGED] = "U2PS_CONTRACT_DEV_DESCRIPTION_CHANGED",
  [U2PS_CONTRACT_DEV_RELEASE_COUNT] = "U2PS_CONTRACT_DEV_RELEASE_COUNT",
  [U2PS_CONTRACT_DEV_RELEASE_STREAM] = "U2PS_CONTRACT_DEV_RELEASE_STREAM",
  [U2PS_CONTRACT_DEV_REPLY_RESULT] = "U2PS_CONTRACT_DEV_REPLY_RESULT",
  [U2PS_CONTRACT_DEV_REPLY_DATA] = "U2PS_CONTRACT_DEV_REPLY_DATA",
  [U2PS_CONTRACT_DEV_CALLBACK_RESULT] = "U2PS_CONTRACT_DEV_CALLBACK_RESULT",
  [U2PS_CONTRACT_HOST_CALL_IN_CALLBACK] = "U2PS_CONTRACT_HOST_CALL_IN_CALLBACK",
  [U2PS_CONTRACT_HOST_CALL_NESTED] = "U2PS_CONTRACT_HOST_CALL_NESTED",
  [U2PS_CONTRACT_HOST_CONNECT_BEFORE_INIT] = "U2PS_CONTRACT_HOST_CONNECT_BEFORE_INIT",
  [U2PS_CONTRACT_HOST_TICK_LATE] = "U2PS_CONTRACT_HOST_TICK_LATE",
  [U2PS_CONTRACT_HOST_QUEUE_STREAM] = "U2PS_CONTRACT_HOST_QUEUE_STREAM",
  [U2PS_CONTRACT_HOST_QUEUE_DATA] = "U2PS_CONTRACT_HOST_QUEUE_DATA",
  [U2PS_CONTRACT_HOST_QUEUE_FULL] = "U2PS_CONTRACT_HOST_QUEUE_FULL",
  [U2PS_CONTRACT_HOST_QUEUE_UNSIZED] = "U2PS_CONTRACT_HOST_QUEUE_UNSIZED",
  [U2PS_CONTRACT_HOST_SUSPEND_STREAM] = "U2PS_CONTRACT_HOST_SUSPEND_STREAM",
  [U2PS_CONTRACT_HOST_SUSPEND_SUSPENDED] = "U2PS_CONTRACT_HOST_SUSPEND_SUSPENDED",
  [U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED] = "U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED",
};

const char *u2ps_testkit_callback_name(u2ps_testkit_callback_t callback)
{
  if ((unsigned)callback >= U2PS_TESTKIT_CALLBACKS)
    return "?";
  return callback_names[callback];
}

const char *u2ps_testkit_contract_name(u2ps_contract_t violation)
{
  if ((unsigned)violation >= CONTRACT_CODES || contract_names[violation] == NULL)
    return "?";
  return contract_names[violation];
}

/* ---- Checks and the result ------------------------------------------------------ */

static unsigned checks, checks_failed;

bool u2ps_testkit_check(bool ok, const char *cond, const char *file, int line)
{
  checks++;
  if (!ok) {
    checks_failed++;
    printf("u2ps testkit: CHECK FAILED at %s:%d: %s\n", file, line, cond);
  }
  return ok;
}

/* ---- The record of violations ------------------------------------------------- */

static unsigned contracts[CONTRACT_CODES];
static unsigned contracts_other; /* codes outside u2ps_contract_t */
static const u2ps_testkit_port_t *ports[2];
static unsigned port_carried; /* of fake port states since reset */

void u2ps_testkit_record_contract(u2ps_contract_t violation, unsigned stack)
{
  if ((unsigned)violation < CONTRACT_CODES)
    contracts[violation]++;
  else
    contracts_other++;
  printf("u2ps testkit: contract violation %s (%s stack)\n", u2ps_testkit_contract_name(violation),
         stack == 0u ? "device" : "host");
  (void)u2ps_testkit_record(U2PS_TESTKIT_ON_CONTRACT, (uint32_t)violation, stack, 0, 0);
}

unsigned u2ps_testkit_contract_count(void)
{
  unsigned n = contracts_other;
  for (unsigned i = 0; i < CONTRACT_CODES; i++)
    n += contracts[i];
  return n;
}

unsigned u2ps_testkit_contract_take(u2ps_contract_t violation)
{
  if ((unsigned)violation >= CONTRACT_CODES)
    return 0;
  unsigned n = contracts[violation];
  contracts[violation] = 0;
  return n;
}

unsigned u2ps_testkit_port_violations(void)
{
  unsigned n = port_carried;
  for (unsigned i = 0; i < 2u; i++)
    if (ports[i] != NULL)
      n += ports[i]->count();
  return n;
}

unsigned u2ps_testkit_port_violations_take(void)
{
  unsigned n = u2ps_testkit_port_violations();
  port_carried = 0;
  for (unsigned i = 0; i < 2u; i++)
    if (ports[i] != NULL)
      ports[i]->clear();
  return n;
}

int u2ps_testkit_result(void)
{
  unsigned c = u2ps_testkit_contract_count();
  unsigned p = u2ps_testkit_port_violations();
  bool pass = checks_failed == 0u && c == 0u && p == 0u;
  printf("u2ps testkit: %s: %u checks, %u failed, %u contract violations, %u port violations\n",
         pass ? "PASS" : "FAIL", checks, checks_failed, c, p);
  return pass ? 0 : 1;
}

/* ---- The trace ------------------------------------------------------------------- */

static u2ps_testkit_call_t trace[U2PS_TESTKIT_TRACE_MAX];
static unsigned trace_n;
static unsigned counts[U2PS_TESTKIT_CALLBACKS];

void u2ps_testkit_trace_clear(void)
{
  trace_n = 0;
  memset(counts, 0, sizeof counts);
}

unsigned u2ps_testkit_record(u2ps_testkit_callback_t callback, uint32_t a0, uint32_t a1,
                             uint32_t a2, uint32_t a3)
{
  unsigned i = trace_n++;
  if ((unsigned)callback < U2PS_TESTKIT_CALLBACKS)
    counts[callback]++;
  if (i < U2PS_TESTKIT_TRACE_MAX)
    trace[i] = (u2ps_testkit_call_t){.callback = callback, .arg = {a0, a1, a2, a3}};
  return i;
}

void u2ps_testkit_record_result(unsigned index, int32_t result)
{
  if (index < U2PS_TESTKIT_TRACE_MAX && index < trace_n)
    trace[index].result = result;
}

unsigned u2ps_testkit_trace_count(void)
{
  return trace_n;
}

const u2ps_testkit_call_t *u2ps_testkit_trace_get(unsigned i)
{
  if (i >= trace_n || i >= U2PS_TESTKIT_TRACE_MAX)
    return NULL;
  return &trace[i];
}

unsigned u2ps_testkit_calls(u2ps_testkit_callback_t callback)
{
  if ((unsigned)callback >= U2PS_TESTKIT_CALLBACKS)
    return 0;
  return counts[callback];
}

/* One call as text: name and arguments, key=value */
static void call_text(const u2ps_testkit_call_t *c, char *out, size_t size)
{
  int n = snprintf(out, size, "%s", u2ps_testkit_callback_name(c->callback));
  for (unsigned a = 0; a < 4u && n > 0 && (size_t)n < size; a++) {
    const char *name = (unsigned)c->callback < U2PS_TESTKIT_CALLBACKS
                         ? arg_names[c->callback][a]
                         : NULL;
    if (name == NULL)
      break;
    if (c->callback == U2PS_TESTKIT_ON_CONTRACT && a == 0u)
      n += snprintf(out + n, size - (size_t)n, " %s=%s", name,
                    u2ps_testkit_contract_name((u2ps_contract_t)c->arg[0]));
    else
      n += snprintf(out + n, size - (size_t)n, " %s=%u", name, (unsigned)c->arg[a]);
  }
  if (has_result(c->callback) && n > 0 && (size_t)n < size)
    snprintf(out + n, size - (size_t)n, " result=%d", (int)c->result);
}

void u2ps_testkit_trace_print(void)
{
  char line[192];
  unsigned kept = trace_n < U2PS_TESTKIT_TRACE_MAX ? trace_n : U2PS_TESTKIT_TRACE_MAX;
  for (unsigned i = 0; i < kept; i++) {
    call_text(&trace[i], line, sizeof line);
    printf("u2ps testkit: trace %u: %s\n", i, line);
  }
  if (trace_n > kept)
    printf("u2ps testkit: trace: %u more callbacks not kept\n", trace_n - kept);
}

/* The callback named by the n characters at s, or U2PS_TESTKIT_CALLBACKS */
static unsigned callback_of(const char *s, size_t n)
{
  for (unsigned c = 0; c < U2PS_TESTKIT_CALLBACKS; c++)
    if (strlen(callback_names[c]) == n && strncmp(callback_names[c], s, n) == 0)
      return c;
  return U2PS_TESTKIT_CALLBACKS;
}

/* Prints the trace's names in a row, for a mismatch */
static void print_names(void)
{
  unsigned kept = trace_n < U2PS_TESTKIT_TRACE_MAX ? trace_n : U2PS_TESTKIT_TRACE_MAX;
  printf("u2ps testkit: trace_match: the trace is:");
  for (unsigned i = 0; i < kept; i++)
    printf(" %s", u2ps_testkit_callback_name(trace[i].callback));
  printf("%s\n", trace_n > kept ? " ... (more not kept)" : "");
}

bool u2ps_testkit_trace_match(const char *expected)
{
  unsigned at = 0;
  const char *p = expected;
  const char *why = NULL;
  if (trace_n > U2PS_TESTKIT_TRACE_MAX)
    why = "the trace holds more callbacks than it keeps";
  while (why == NULL) {
    while (*p == ' ')
      p++;
    if (*p == '\0')
      break;
    size_t n = strcspn(p, " *");
    unsigned c = callback_of(p, n);
    p += n;
    unsigned long times = 1;
    if (*p == '*') {
      char *end;
      times = strtoul(p + 1, &end, 10);
      p = end;
    }
    if (c == U2PS_TESTKIT_CALLBACKS) {
      why = "an unknown callback name";
      break;
    }
    for (unsigned long k = 0; k < times && why == NULL; k++, at++)
      if (at >= trace_n || trace[at].callback != (u2ps_testkit_callback_t)c)
        why = "the callbacks differ";
  }
  if (why == NULL && at != trace_n)
    why = "the trace has more callbacks";
  if (why == NULL)
    return true;
  printf("u2ps testkit: trace_match: %s (at callback %u)\n", why, at);
  printf("u2ps testkit: trace_match: expected: %s\n", expected);
  print_names();
  return false;
}

/* ---- Sides, time ----------------------------------------------------------------- */

static uint32_t now;
static void (*app_poll)(uint32_t now_ms);

void u2ps_testkit_side_reset(unsigned side, const u2ps_testkit_port_t *port)
{
  if (side < 2u) {
    if (ports[side] != NULL)
      port_carried += ports[side]->count();
    ports[side] = port;
  }
  u2ps_testkit_trace_clear();
  now = 0;
}

void u2ps_testkit_set_poll(void (*poll)(uint32_t now_ms))
{
  app_poll = poll;
}

uint32_t u2ps_testkit_now_ms(void)
{
  return now;
}

void u2ps_testkit_set_now(uint32_t now_ms)
{
  now = now_ms;
}

bool u2ps_testkit_poll_run(uint32_t now_ms)
{
  if (app_poll == NULL)
    return false;
  app_poll(now_ms);
  return true;
}
