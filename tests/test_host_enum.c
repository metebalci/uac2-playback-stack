/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_host_enum.c: the host stack's enumeration against the fake host port
 * and a device model answering from fixtures: the requests in order with
 * their fields, the waits measured by the ticks, the debounce, the reset
 * segments, the optional requests whose STALL is harmless, the failures
 * with their reasons (exactly one on_end each), VBUS, the session's end by
 * the app, and test mode.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "host_support.h"
#include "u2ps_ch9.h"
#include "u2ps_uac2.h"

/* The control transfers of a whole enumeration of the test device */
enum {
  R_DEVICE8,  /* GET_DESCRIPTOR(device, 64) at address 0 */
  R_ADDRESS,  /* SET_ADDRESS(1) */
  R_DEVICE,   /* GET_DESCRIPTOR(device, 18) */
  R_CONFIG9,  /* GET_DESCRIPTOR(configuration 0, 9) */
  R_CONFIG,   /* GET_DESCRIPTOR(configuration 0, wTotalLength) */
  R_LANGID,   /* GET_DESCRIPTOR(string 0) */
  R_STRING1,
  R_STRING2,
  R_STRING3,
  R_SET_CONFIG,
  R_RANGE,
  R_CUR,
  R_VALID,
  R_COUNT
};

static const u2ps_hfake_call_t *req(unsigned nth)
{
  static const u2ps_hfake_call_t none = {.kind = U2PS_HFAKE_MISSING, .ms = 0xFFFFFFFFu,
                                         .done_ms = 0xFFFFFFFFu};
  const u2ps_hfake_call_t *c = u2ps_hfake_control_nth(nth);
  return c != NULL ? c : &none;
}

static void check_setup(unsigned nth, uint8_t addr, uint8_t bm, uint8_t breq, uint16_t value,
                        uint16_t index, uint16_t length)
{
  const u2ps_hfake_call_t *c = req(nth);
  CHECK_EQ(c->kind, U2PS_HFAKE_CONTROL);
  CHECK_EQ(c->arg, addr);
  CHECK_EQ(c->setup.bmRequestType, bm);
  CHECK_EQ(c->setup.bRequest, breq);
  CHECK_EQ(c->setup.wValue, value);
  CHECK_EQ(c->setup.wIndex, index);
  CHECK_EQ(c->setup.wLength, length);
  if (c->kind != U2PS_HFAKE_CONTROL || c->setup.bRequest != breq || c->setup.wValue != value)
    printf("  request %u\n", nth);
}

static bool ended(void)
{
  return happ_log.ends > 0u;
}

/* Exactly one on_end, with reason 'why', in an upcall, no on_device; the
 * port disabled at it, and no port call after it */
static void check_failed(u2ps_host_end_t why)
{
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, why);
  if (why != U2PS_HOST_END_NOT_PLAYABLE)
    CHECK_EQ(happ_log.refusal, U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(happ_log.devices, 0);
  CHECK_EQ(happ_log.end_from, U2PS_HFAKE_FROM_UPCALL);
  unsigned at = happ_log.calls_at_end;
  u2ps_hfake_run_until(u2ps_hfake_now + 6000);
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(u2ps_hfake_ncalls, at + 1u);
  CHECK_EQ(u2ps_hfake_get(U2PS_HFAKE_DISABLE, at).kind, U2PS_HFAKE_DISABLE);
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_hmodel.unexpected, 0);
}

/* ---- A whole enumeration --------------------------------------------------------- */

static void test_requests(void)
{
  static const uint8_t total[2] = {0x25, 0x01}; /* uac2-hs-default.txt: 293 bytes */
  if (!hstart("uac2-hs-default.txt"))
    return;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(happ_log.ends, 0);
  if (happ_log.devices != 1u)
    return;
  CHECK(strcmp(happ_log.events, "D") == 0);
  CHECK_EQ(happ_log.device_from, U2PS_HFAKE_FROM_UPCALL);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), R_COUNT);
  check_setup(R_DEVICE8, 0, 0x80, U2PS_GET_DESCRIPTOR, 0x0100, 0, 64);
  check_setup(R_ADDRESS, 0, 0x00, U2PS_SET_ADDRESS, 1, 0, 0);
  check_setup(R_DEVICE, 1, 0x80, U2PS_GET_DESCRIPTOR, 0x0100, 0, 18);
  check_setup(R_CONFIG9, 1, 0x80, U2PS_GET_DESCRIPTOR, 0x0200, 0, 9);
  check_setup(R_CONFIG, 1, 0x80, U2PS_GET_DESCRIPTOR, 0x0200, 0, total[0] | total[1] << 8);
  check_setup(R_LANGID, 1, 0x80, U2PS_GET_DESCRIPTOR, 0x0300, 0, 255);
  check_setup(R_STRING1, 1, 0x80, U2PS_GET_DESCRIPTOR, 0x0301, 0x0409, 255);
  check_setup(R_STRING2, 1, 0x80, U2PS_GET_DESCRIPTOR, 0x0302, 0x0409, 255);
  check_setup(R_STRING3, 1, 0x80, U2PS_GET_DESCRIPTOR, 0x0303, 0x0409, 255);
  check_setup(R_SET_CONFIG, 1, 0x00, U2PS_SET_CONFIGURATION, 1, 0, 0);
  /* The clock's requests: Clock Source 4 on interface 0, channel 0 (UAC2
   * 5.2.2); RANGE for at most U2PS_HOST_SUB_RANGE_MAX sub-ranges */
  check_setup(R_RANGE, 1, 0xA1, U2PS_REQ_RANGE, 0x0100, 0x0400,
              2u + 12u * U2PS_HOST_SUB_RANGE_MAX);
  check_setup(R_CUR, 1, 0xA1, U2PS_REQ_CUR, 0x0100, 0x0400, 4);
  check_setup(R_VALID, 1, 0xA1, U2PS_REQ_CUR, 0x0200, 0x0400, 1);
  CHECK_EQ(u2ps_hmodel.configuration, 1);

  /* What on_device tells */
  const u2ps_host_device_t *d = &happ_log.dev;
  CHECK_EQ(d->id_vendor, 0x1234);
  CHECK_EQ(d->id_product, 0x5678);
  CHECK_EQ(d->bcd_device, 0x0100);
  CHECK(d->manufacturer != NULL && strcmp(d->manufacturer, "Maker") == 0);
  CHECK(d->product != NULL && strcmp(d->product, "Player Test") == 0);
  CHECK(d->serial_number != NULL && strcmp(d->serial_number, "SN-0001") == 0);
  CHECK_EQ(d->terminal_type, 0x0301);
  CHECK(d->sampling_frequency_host_programmable);
  CHECK_EQ(d->sampling_frequency, 48000);
  CHECK_EQ(d->num_sub_ranges, 10);
  for (unsigned i = 0; i < 10u; i++) {
    CHECK_EQ(d->sub_ranges[i].min, test_rates_host[i]);
    CHECK_EQ(d->sub_ranges[i].max, test_rates_host[i]);
    CHECK_EQ(d->sub_ranges[i].res, 0);
  }
  CHECK_EQ(d->alternate_setting_count, 4);
  static const uint8_t slot[4] = {2, 4, 4, 3}, res[4] = {16, 24, 32, 24};
  for (unsigned i = 0; i < 4u; i++) {
    const u2ps_host_alternate_setting_t *a = &d->alternate_settings[i];
    CHECK_EQ(a->alternate_setting, i + 1u);
    CHECK_EQ(a->format.subslot_size, slot[i]);
    CHECK_EQ(a->format.bit_resolution, res[i]);
    CHECK_EQ(a->nr_channels, 2);
    CHECK_EQ(a->max_packet_size, 97u * 2u * slot[i]);
    CHECK_EQ(a->synchronization_type, U2PS_SYNCHRONIZATION_TYPE_ASYNCHRONOUS);
    CHECK(a->explicit_feedback);
    CHECK_EQ(a->skip, U2PS_HOST_SKIP_NONE);
  }
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_hmodel.unexpected, 0);

  /* After on_device the stack waits: no more requests */
  unsigned n = u2ps_hfake_ncalls;
  u2ps_hfake_run_until(u2ps_hfake_now + 6000);
  CHECK_EQ(u2ps_hfake_ncalls, n);
  CHECK_EQ(happ_log.ends, 0);
}

/* The waits, measured by the fake's clock: >= 100 ms after the connect, a
 * 50 ms reset, >= 10 ms of recovery, >= 2 ms after SET_ADDRESS; each at
 * most 2 ms longer (the first tick after the event, then the tick that
 * sees the time) */
static void test_waits(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hfake_run_until(5);
  uint32_t connect = u2ps_hfake_now;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  u2ps_hfake_call_t on = u2ps_hfake_get(U2PS_HFAKE_RESET, 0);
  int i = u2ps_hfake_find(U2PS_HFAKE_RESET, 0);
  u2ps_hfake_call_t off = u2ps_hfake_get(U2PS_HFAKE_RESET, i < 0 ? 0u : (unsigned)i + 1u);
  CHECK_EQ(on.arg, 1);
  CHECK_EQ(off.arg, 0);
  CHECK(on.ms >= connect + 100u && on.ms <= connect + 102u);
  CHECK(off.ms >= on.ms + 50u && off.ms <= on.ms + 52u);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_RESET), 2);
  /* on_enabled comes at the step after reset(false) */
  uint32_t enabled = off.ms + 1u;
  CHECK(req(R_DEVICE8)->ms >= enabled + 10u && req(R_DEVICE8)->ms <= enabled + 12u);
  CHECK(req(R_DEVICE)->ms >= req(R_ADDRESS)->done_ms + 2u &&
        req(R_DEVICE)->ms <= req(R_ADDRESS)->done_ms + 4u);
  printf("connect %u, reset %u..%u, first request %u, SET_ADDRESS done %u, next %u\n",
         (unsigned)connect, (unsigned)on.ms, (unsigned)off.ms, (unsigned)req(R_DEVICE8)->ms,
         (unsigned)req(R_ADDRESS)->done_ms, (unsigned)req(R_DEVICE)->ms);
  /* Every other request follows its predecessor's completion at once */
  for (unsigned k = R_CONFIG9; k < R_COUNT; k++)
    CHECK_EQ(req(k)->ms, req(k - 1u)->done_ms);
  CHECK_EQ(req(R_ADDRESS)->ms, req(R_DEVICE8)->done_ms);
  CHECK_EQ(req(R_CONFIG9)->ms, req(R_DEVICE)->done_ms);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* A disconnect at 60 ms of the debounce restarts it: the reset starts >=
 * 100 ms after the reconnect; no session ended */
static void test_debounce_restarts(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hfake_connect();
  u2ps_hfake_run_until(60);
  u2ps_hfake_disconnect();
  u2ps_hfake_run_until(80);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_RESET), 0);
  u2ps_hfake_connect();
  u2ps_hfake_run_until(179);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_RESET), 0);
  CHECK(u2ps_hfake_wait(happ_done, 2000));
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(happ_log.ends, 0);
  u2ps_hfake_call_t on = u2ps_hfake_get(U2PS_HFAKE_RESET, 0);
  CHECK(on.ms >= 180u && on.ms <= 182u);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* ChipIdea ends each reset segment itself: the stack starts another
 * inside on_enabled until 50 ms have passed */
static void test_reset_segments(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hfake_segment_ms = 20;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  int first = u2ps_hfake_find(U2PS_HFAKE_RESET, 0);
  CHECK(first >= 0);
  if (first < 0)
    return;
  uint32_t start = u2ps_hfake_calls[first].ms;
  unsigned ons = 0;
  uint32_t last_on = 0;
  for (unsigned k = 0; k < u2ps_hfake_ncalls; k++)
    if (u2ps_hfake_calls[k].kind == U2PS_HFAKE_RESET && u2ps_hfake_calls[k].arg == 1u) {
      ons++;
      last_on = u2ps_hfake_calls[k].ms;
      if (k != (unsigned)first)
        CHECK_EQ(u2ps_hfake_calls[k].from, U2PS_HFAKE_FROM_UPCALL);
    }
  /* Segments start at 0, 20 and 40 ms; the last ends at 60 ms */
  CHECK_EQ(ons, 3);
  CHECK_EQ(last_on, start + 40u);
  uint32_t enabled = last_on + 20u;
  CHECK(req(R_DEVICE8)->ms >= enabled + 10u && req(R_DEVICE8)->ms <= enabled + 12u);
  CHECK(req(R_DEVICE8)->ms >= start + 60u);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* ---- Optional requests ----------------------------------------------------------- */

static void test_string_stall(void)
{
  /* String 2 stalls: it alone is NULL */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel_set_string(2, NULL);
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(happ_log.ends, 0);
  CHECK(happ_log.dev.manufacturer != NULL && strcmp(happ_log.dev.manufacturer, "Maker") == 0);
  CHECK(happ_log.dev.product == NULL);
  CHECK(happ_log.dev.serial_number != NULL);
  CHECK_EQ(req(R_STRING2)->result, U2PS_HPORT_CONTROL_STALL);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), R_COUNT);
  /* String 0 stalls: no string is read */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel_set_string(0, NULL);
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK(happ_log.dev.manufacturer == NULL && happ_log.dev.product == NULL &&
        happ_log.dev.serial_number == NULL);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), R_COUNT - 3u);
  /* A device without strings: none is asked for */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.device[14] = u2ps_hmodel.device[15] = u2ps_hmodel.device[16] = 0;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK(happ_log.dev.manufacturer == NULL && happ_log.dev.product == NULL &&
        happ_log.dev.serial_number == NULL);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), R_COUNT - 4u);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* UTF-16 code units outside printable ASCII become '?'; an empty string is
 * "", not NULL; a reply that is no string descriptor is NULL */
static void test_string_text(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  static const uint8_t cafe[] = {12, U2PS_DESC_STRING, 'C', 0, 'a', 0, 'f', 0, 0xE9, 0, 0x09, 0};
  memcpy(u2ps_hmodel.string[1], cafe, sizeof cafe);
  u2ps_hmodel.string_len[1] = sizeof cafe;
  u2ps_hmodel_set_string(2, "");
  u2ps_hmodel.string[3][1] = U2PS_DESC_DEVICE;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK(happ_log.dev.manufacturer != NULL && strcmp(happ_log.dev.manufacturer, "Caf??") == 0);
  CHECK(happ_log.dev.product != NULL && strcmp(happ_log.dev.product, "") == 0);
  CHECK(happ_log.dev.serial_number == NULL);
  /* The longest string, 126 characters */
  if (!hstart("uac2-hs-default.txt"))
    return;
  char longest[127];
  memset(longest, 'x', 126);
  longest[126] = '\0';
  u2ps_hmodel_set_string(3, longest);
  CHECK(henumerate());
  CHECK(happ_log.dev.serial_number != NULL && strcmp(happ_log.dev.serial_number, longest) == 0);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* The Clock Validity Control (UAC2 5.2.5.1.2): CUR true gives VALID, false
 * INVALID; a STALL, or a Control that is not readable, UNKNOWN */
static void test_clock_validity(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(happ_log.dev.clock_validity, U2PS_CLOCK_VALIDITY_VALID);
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.valid = 0;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(happ_log.dev.clock_validity, U2PS_CLOCK_VALIDITY_INVALID);
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.stall_valid = true;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(happ_log.ends, 0);
  CHECK_EQ(req(R_VALID)->result, U2PS_HPORT_CONTROL_STALL);
  CHECK_EQ(happ_log.dev.clock_validity, U2PS_CLOCK_VALIDITY_UNKNOWN);
  /* Clock Validity Control not readable: not asked */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.config[0][9 + 8 + 9 + 9 + 5] = 0x03; /* the Clock Source's bmControls */
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), R_COUNT - 1u);
  CHECK_EQ(happ_log.dev.clock_validity, U2PS_CLOCK_VALIDITY_UNKNOWN);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* ---- Failures --------------------------------------------------------------------- */

static void test_clock_failures(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.stall_range = true;
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_CLOCK);
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.stall_cur = true;
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_CLOCK);
  /* RANGE with no sub-range, and one cut inside its first sub-range */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel_set_rates(test_rates_host, 0);
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_CLOCK);
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.range_len = 13;
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_CLOCK);
  /* CUR is what the device says */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.cur = 96000;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(happ_log.dev.sampling_frequency, 96000);
}

/* RANGE kept as the reply holds it: cut after three whole sub-ranges, or
 * longer than kept */
static void test_range_kept(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.range_len = 2 + 12 * 3 + 5;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(happ_log.dev.num_sub_ranges, 3);
  if (!hstart("uac2-hs-default.txt"))
    return;
  static uint32_t many[40];
  for (unsigned i = 0; i < 40u; i++)
    many[i] = 8000u * (i + 1u);
  u2ps_hmodel_set_rates(many, 40);
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(happ_log.dev.num_sub_ranges, U2PS_HOST_SUB_RANGE_MAX);
  CHECK_EQ(happ_log.dev.sub_ranges[U2PS_HOST_SUB_RANGE_MAX - 1u].min,
           8000u * U2PS_HOST_SUB_RANGE_MAX);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* A control transfer without an answer: aborted after
 * U2PS_HOST_CONTROL_MAX, then on_end(TIMEOUT) at the abort's completion */
static void test_timeout(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.hang_at = R_SET_CONFIG;
  u2ps_hfake_connect();
  u2ps_hfake_wait(u2ps_hfake_control_pending, 1000);
  while (u2ps_hfake_count(U2PS_HFAKE_CONTROL) <= R_SET_CONFIG && u2ps_hfake_now < 2000u)
    u2ps_hfake_step();
  CHECK(u2ps_hfake_control_pending());
  uint32_t start = req(R_SET_CONFIG)->ms;
  u2ps_hfake_run_until(start + U2PS_HOST_CONTROL_MAX - 1u);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL_ABORT), 0);
  CHECK(u2ps_hfake_wait(ended, 10));
  u2ps_hfake_call_t abort = u2ps_hfake_get(U2PS_HFAKE_CONTROL_ABORT, 0);
  CHECK_EQ(abort.kind, U2PS_HFAKE_CONTROL_ABORT);
  CHECK(abort.ms >= start + U2PS_HOST_CONTROL_MAX && abort.ms <= start + U2PS_HOST_CONTROL_MAX + 2u);
  CHECK_EQ(abort.from, U2PS_HFAKE_FROM_MASKED);
  CHECK_EQ(req(R_SET_CONFIG)->result, U2PS_HPORT_CONTROL_ABORTED);
  check_failed(U2PS_HOST_END_TIMEOUT);
  u2ps_host_stats_t st;
  u2ps_host_stats(&st);
  CHECK_EQ(st.control_timeouts, 1);
}

/* A disconnect while a transfer is in flight: on_end(DISCONNECT), no
 * abort; a reconnect enumerates again from the debounce */
static void test_disconnect_mid_transfer(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.hang_at = R_CONFIG;
  u2ps_hfake_connect();
  while (u2ps_hfake_count(U2PS_HFAKE_CONTROL) <= R_CONFIG && u2ps_hfake_now < 2000u)
    u2ps_hfake_step();
  u2ps_hfake_run_until(u2ps_hfake_now + 3);
  u2ps_hfake_disconnect();
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, U2PS_HOST_END_DISCONNECT);
  CHECK_EQ(happ_log.end_from, U2PS_HFAKE_FROM_UPCALL);
  unsigned at = u2ps_hfake_ncalls;
  u2ps_hfake_run_until(u2ps_hfake_now + 6000);
  CHECK_EQ(u2ps_hfake_ncalls, at);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL_ABORT), 0);
  /* Again */
  hmodel_load("uac2-hs-default.txt");
  happ_clear();
  uint32_t connect = u2ps_hfake_now;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  int r = u2ps_hfake_find(U2PS_HFAKE_RESET, at);
  CHECK(r >= 0 && u2ps_hfake_calls[r].ms >= connect + 100u);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* A full-speed device is identified, then ends with SPEED: the device
 * descriptor and the configuration, no more (test_host_speed.c has the
 * full- and low-speed cases and U2PS_HOST_REFUSE_FS) */
static void test_full_speed(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hfake_speed = U2PS_SPEED_FULL;
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_SPEED);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), R_CONFIG + 1u);
}

static void test_ep0_mps(void)
{
  static const uint8_t mps[] = {8, 16, 32, 0, 255};
  for (unsigned k = 0; k < sizeof mps; k++) {
    if (!hstart("uac2-hs-default.txt"))
      return;
    u2ps_hmodel.device[7] = mps[k];
    CHECK(henumerate());
    check_failed(U2PS_HOST_END_EP0_MPS);
    CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), 1);
  }
}

/* A configuration of exactly U2PS_HOST_EP0_BUF bytes plays; one byte more
 * ends the session before the stack asks for it */
static void test_too_large(void)
{
  dl_t l;
  static uint8_t big[U2PS_HOST_EP0_BUF + 1u];
  for (unsigned extra = 0; extra <= 1u; extra++) {
    if (!hstart(NULL) || !dl_load(&l, "uac2-hs-default.txt"))
      return;
    /* Pad with descriptors of an unknown type after the function, written
     * into the bytes: at the largest U2PS_HOST_EP0_BUF they are more than
     * a descriptor list holds. Each pass adds one, or the loop ends. */
    unsigned len = dl_join(&l, big, sizeof big, true);
    unsigned want = U2PS_HOST_EP0_BUF + extra;
    while (len < want) {
      unsigned n = want - len > 255u ? 255u : want - len;
      if (n < 2u)
        n = 2;
      if (want - len - n == 1u)
        n--;
      if (len + n > sizeof big)
        break;
      memset(big + len, 0, n);
      big[len] = (uint8_t)n;
      big[len + 1u] = 0x41;
      len += n;
    }
    big[2] = (uint8_t)len;
    big[3] = (uint8_t)(len >> 8);
    CHECK_EQ(len, want);
    u2ps_hmodel_set_config(0, big, (uint16_t)len);
    CHECK(henumerate());
    if (extra == 0u) {
      CHECK_EQ(happ_log.devices, 1);
      CHECK_EQ(req(R_CONFIG)->setup.wLength, U2PS_HOST_EP0_BUF);
    } else {
      check_failed(U2PS_HOST_END_TOO_LARGE);
      CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), R_CONFIG9 + 1u);
    }
  }
}

static void test_other_failures(void)
{
  /* The device descriptor stalls */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.stall_at = R_DEVICE8;
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_STALL);
  /* SET_ADDRESS fails with errors */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.error_at = R_ADDRESS;
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_ERROR);
  /* SET_CONFIGURATION stalls */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.stall_at = R_SET_CONFIG;
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_STALL);
  /* A string request fails with errors: not optional */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.error_at = R_STRING1;
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_ERROR);
  /* A device descriptor of another type, too short, or without a
   * configuration */
  for (unsigned k = 0; k < 3u; k++) {
    if (!hstart("uac2-hs-default.txt"))
      return;
    if (k == 0u)
      u2ps_hmodel.device[1] = U2PS_DESC_CONFIGURATION;
    else if (k == 1u)
      u2ps_hmodel.device[0] = 8;
    else
      u2ps_hmodel.device[17] = 0;
    CHECK(henumerate());
    check_failed(U2PS_HOST_END_DESCRIPTOR);
  }
  /* A configuration header that is no configuration descriptor */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.config[0][1] = U2PS_DESC_DEVICE;
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_DESCRIPTOR);
  /* No playable audio function */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.config[0][9 + 8 + 9 + 9 + 8 + 4] = 0x03; /* the Input Terminal: no USB streaming */
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_NOT_PLAYABLE);
  CHECK_EQ(happ_log.refusal, U2PS_HOST_REFUSAL_NO_TERMINAL);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_CONTROL), R_CONFIG + 1u);
}

/* The device model leaves and comes back: at address 0, not configured */
static void replug(void)
{
  u2ps_hfake_disconnect();
  u2ps_hmodel.address = 0;
  u2ps_hmodel.configuration = 0;
  happ_clear();
}

/* A second session starts afresh: nothing of the first one's device stays
 * (strings, clock validity, refusal reason) */
static void test_second_session_fresh(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.config[0][9 + 8 + 9 + 9 + 8 + 4] = 0x03; /* no USB streaming terminal */
  CHECK(henumerate());
  CHECK_EQ(happ_log.refusal, U2PS_HOST_REFUSAL_NO_TERMINAL);
  replug();
  u2ps_hmodel.config[0][9 + 8 + 9 + 9 + 8 + 4] = 0x01;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK(happ_log.dev.manufacturer != NULL);
  CHECK_EQ(happ_log.dev.clock_validity, U2PS_CLOCK_VALIDITY_VALID);
  /* The next device has no strings and no readable Clock Validity
   * Control */
  replug();
  u2ps_hmodel.device[14] = u2ps_hmodel.device[15] = u2ps_hmodel.device[16] = 0;
  u2ps_hmodel.config[0][9 + 8 + 9 + 9 + 5] = 0x03;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK(happ_log.dev.manufacturer == NULL);
  CHECK(happ_log.dev.product == NULL);
  CHECK(happ_log.dev.serial_number == NULL);
  CHECK_EQ(happ_log.dev.clock_validity, U2PS_CLOCK_VALIDITY_UNKNOWN);
  /* A device that does not play: its own reason */
  replug();
  u2ps_hmodel.config[0][9 + 8 + 7] = 0x00; /* UAC1 */
  CHECK(henumerate());
  CHECK_EQ(happ_log.end, U2PS_HOST_END_NOT_PLAYABLE);
  CHECK_EQ(happ_log.refusal, U2PS_HOST_REFUSAL_UAC1);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* The refusal reason that comes with on_end(NOT_PLAYABLE): the walk's, of
 * the first configuration when none plays */
static void test_refusal(void)
{
  dl_t l;
  static uint8_t buf[600];
  /* Every alternate setting skipped: high bandwidth on each data endpoint */
  if (!hstart("uac2-hs-default.txt") || !dl_load(&l, "uac2-hs-default.txt"))
    return;
  for (int i, k = 0; (i = dl_data_ep(&l, (unsigned)k)) >= 0; k++)
    l.d[i][5] |= 0x08; /* wMaxPacketSize bits 12..11: 1 additional transaction */
  unsigned n = dl_join(&l, buf, sizeof buf, true);
  u2ps_hmodel_set_config(0, buf, (uint16_t)n);
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_NOT_PLAYABLE);
  CHECK_EQ(happ_log.refusal, U2PS_HOST_REFUSAL_ALL_SKIPPED);
  /* UAC1: the AudioControl interface's bInterfaceProtocol 0 */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.config[0][9 + 8 + 7] = 0x00;
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_NOT_PLAYABLE);
  CHECK_EQ(happ_log.refusal, U2PS_HOST_REFUSAL_UAC1);
  /* Two configurations, neither plays: the first's reason (no audio
   * function), not the second's (no USB streaming terminal) */
  if (!hstart("uac2-hs-default.txt") || !dl_load(&l, "uac2-hs-default.txt"))
    return;
  memcpy(u2ps_hmodel.config[1], u2ps_hmodel.config[0], u2ps_hmodel.config_len[0]);
  u2ps_hmodel.config_len[1] = u2ps_hmodel.config_len[0];
  u2ps_hmodel.config[1][9 + 8 + 9 + 9 + 8 + 4] = 0x03;
  dl_truncate(&l, 1);
  dl_insert_hex(&l, 1, "09 04 00 00 00 ff 00 00 00");
  l.d[0][4] = 1;
  n = dl_join(&l, buf, sizeof buf, true);
  u2ps_hmodel_set_config(0, buf, (uint16_t)n);
  u2ps_hmodel.device[17] = 2;
  CHECK(henumerate());
  check_failed(U2PS_HOST_END_NOT_PLAYABLE);
  CHECK_EQ(happ_log.refusal, U2PS_HOST_REFUSAL_NOT_AUDIO);
  /* A device that plays: no on_end; on another end, NONE (check_failed) */
}

/* Two configurations: the first has no audio function, the second plays
 * and is selected by its bConfigurationValue */
static void test_second_configuration(void)
{
  dl_t l;
  static uint8_t buf[600];
  if (!hstart("uac2-hs-24-3.txt") || !dl_load(&l, "uac2-hs-default.txt"))
    return;
  memcpy(u2ps_hmodel.config[1], u2ps_hmodel.config[0], u2ps_hmodel.config_len[0]);
  u2ps_hmodel.config_len[1] = u2ps_hmodel.config_len[0];
  u2ps_hmodel.config[1][5] = 7; /* bConfigurationValue */
  /* The first: a vendor interface only */
  dl_truncate(&l, 1);
  dl_insert_hex(&l, 1, "09 04 00 00 00 ff 00 00 00");
  l.d[0][4] = 1;
  unsigned n = dl_join(&l, buf, sizeof buf, true);
  u2ps_hmodel_set_config(0, buf, (uint16_t)n);
  u2ps_hmodel.device[17] = 2;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(u2ps_hmodel.configuration, 7);
  CHECK_EQ(happ_log.dev.alternate_setting_count, 1);
  /* GET_DESCRIPTOR(configuration 1) after the first */
  bool asked = false;
  for (unsigned k = 0; k < u2ps_hfake_count(U2PS_HFAKE_CONTROL); k++)
    asked = asked || req(k)->setup.wValue == 0x0201;
  CHECK(asked);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* The composite HID + audio device: the clock's requests go to the
 * function's AudioControl interface 1 */
static void test_composite(void)
{
  if (!hstart("composite-hid-uac2-hs.txt"))
    return;
  u2ps_hmodel.clock_windex = 0x0401;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(req(R_RANGE)->setup.wIndex, 0x0401);
  CHECK_EQ(happ_log.dev.alternate_setting_count, 1);
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_hmodel.unexpected, 0);
}

/* ---- The session's end by the app, VBUS, the port --------------------------------- */

static void test_connect_and_vbus(void)
{
  /* Without VBUS nothing happens; VBUS starts the debounce */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_host_vbus(false);
  u2ps_hfake_connect();
  u2ps_hfake_run_until(500);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_RESET), 0);
  u2ps_host_vbus(true);
  CHECK(u2ps_hfake_wait(happ_done, 2000));
  CHECK_EQ(happ_log.devices, 1);
  CHECK(u2ps_hfake_get(U2PS_HFAKE_RESET, 0).ms >= 600u);
  /* A VBUS loss ends the session in thread, masked */
  u2ps_host_vbus(false);
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, U2PS_HOST_END_VBUS);
  CHECK_EQ(happ_log.end_from, U2PS_HFAKE_FROM_MASKED);
  CHECK_EQ(u2ps_hfake_get(U2PS_HFAKE_DISABLE, happ_log.calls_at_end).from, U2PS_HFAKE_FROM_MASKED);
  /* The same level again: nothing */
  unsigned n = u2ps_hfake_ncalls;
  u2ps_host_vbus(false);
  CHECK_EQ(u2ps_hfake_ncalls, n);
  CHECK_EQ(happ_log.ends, 1);

  /* Sessions not allowed: nothing; allowed: the debounce starts */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_host_connect(false);
  u2ps_hfake_connect();
  u2ps_hfake_run_until(300);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_RESET), 0);
  CHECK_EQ(happ_log.ends, 0);
  u2ps_host_connect(true);
  CHECK(u2ps_hfake_wait(happ_done, 2000));
  CHECK_EQ(happ_log.devices, 1);
  /* connect(false) ends the session in thread, masked */
  u2ps_host_connect(false);
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, U2PS_HOST_END_DETACH);
  CHECK_EQ(happ_log.end_from, U2PS_HFAKE_FROM_MASKED);
  /* A second connect(false): harmless */
  u2ps_host_connect(false);
  CHECK_EQ(happ_log.ends, 1);
  /* connect(true) again enumerates the device still on the port */
  happ_clear();
  hmodel_load("uac2-hs-default.txt");
  u2ps_host_connect(true);
  CHECK(u2ps_hfake_wait(happ_done, 2000));
  CHECK_EQ(happ_log.devices, 1);

  /* connect(false) during the debounce: no session, no on_end */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hfake_connect();
  u2ps_hfake_run_until(50);
  u2ps_host_connect(false);
  u2ps_hfake_run_until(500);
  CHECK_EQ(happ_log.ends, 0);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_RESET), 0);

  /* After a failure the device is left alone until connect(false), (true) */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.stall_range = true;
  CHECK(henumerate());
  CHECK_EQ(happ_log.end, U2PS_HOST_END_CLOCK);
  u2ps_hfake_run_until(u2ps_hfake_now + 1000);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_RESET), 2);
  hmodel_load("uac2-hs-default.txt");
  happ_clear();
  u2ps_host_connect(false);
  CHECK_EQ(happ_log.ends, 0);
  u2ps_host_connect(true);
  CHECK(u2ps_hfake_wait(happ_done, 2000));
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

static void test_port_disabled_and_reinit(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  CHECK(henumerate());
  u2ps_hfake_disabled();
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, U2PS_HOST_END_DISABLED);
  CHECK_EQ(happ_log.end_from, U2PS_HFAKE_FROM_UPCALL);
  /* Re-init during a session */
  if (!hstart("uac2-hs-default.txt"))
    return;
  CHECK(henumerate());
  unsigned n = u2ps_hfake_ncalls;
  CHECK_EQ(u2ps_host_init(), U2PS_HOST_OK);
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, U2PS_HOST_END_REINIT);
  CHECK_EQ(happ_log.end_from, U2PS_HFAKE_FROM_MASKED);
  CHECK_EQ(u2ps_hfake_get(U2PS_HFAKE_INIT, n).kind, U2PS_HFAKE_INIT);
  /* The permission ended: no session until connect(true) */
  happ_clear();
  hmodel_load("uac2-hs-default.txt");
  u2ps_hfake_connect();
  u2ps_hfake_run_until(u2ps_hfake_now + 500);
  CHECK_EQ(happ_log.devices, 0);
  u2ps_host_connect(true);
  CHECK(u2ps_hfake_wait(happ_done, 2000));
  CHECK_EQ(happ_log.devices, 1);
  /* A failed init: nothing works */
  u2ps_hfake_reset_all();
  happ_clear();
  u2ps_hfake_init_err = U2PS_HOST_ERR_PHY;
  CHECK_EQ(u2ps_host_init(), U2PS_HOST_ERR_PHY);
  contract_expect(true);
  u2ps_host_connect(true);
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_HOST_CONNECT_BEFORE_INIT], 1);
  CHECK_EQ(contract_log.total, 1);
  contract_expect(false);
  CHECK(!u2ps_host_test_mode(4));
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_TEST_MODE), 0);
  /* The port's cycle counter does not count: its reason comes back, and
   * the stack stays down */
  u2ps_hfake_reset_all();
  u2ps_hfake_init_err = U2PS_HOST_ERR_TIMER;
  CHECK_EQ(u2ps_host_init(), U2PS_HOST_ERR_TIMER);
  CHECK(!u2ps_host_test_mode(4));
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_TEST_MODE), 0);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* test_mode(4) ends the session with on_end(TEST_MODE), then disables the
 * port, then hport_test_mode(4); connect(true) is refused until a re-init;
 * selectors 0 and 6 are refused */
static void test_test_mode(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  CHECK(!u2ps_host_test_mode(0));
  CHECK(!u2ps_host_test_mode(6));
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_TEST_MODE), 0);
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  unsigned n = u2ps_hfake_ncalls;
  CHECK(u2ps_host_test_mode(4));
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, U2PS_HOST_END_TEST_MODE);
  CHECK_EQ(happ_log.end_from, U2PS_HFAKE_FROM_MASKED);
  int dis = u2ps_hfake_find(U2PS_HFAKE_DISABLE, n), tm = u2ps_hfake_find(U2PS_HFAKE_TEST_MODE, n);
  CHECK(dis >= (int)happ_log.calls_at_end && tm > dis);
  CHECK_EQ(u2ps_hfake_test_selector, 4);
  CHECK_EQ(u2ps_hfake_get(U2PS_HFAKE_TEST_MODE, n).from, U2PS_HFAKE_FROM_MASKED);
  /* Refused: connect(true), a new connect; no port call */
  n = u2ps_hfake_ncalls;
  u2ps_host_connect(false);
  u2ps_host_connect(true);
  u2ps_hfake_disconnect();
  u2ps_hfake_connect();
  u2ps_hfake_run_until(u2ps_hfake_now + 1000);
  CHECK_EQ(u2ps_hfake_ncalls, n);
  CHECK_EQ(happ_log.ends, 1);
  CHECK(!u2ps_host_test_mode(0));
  /* A re-init ends it */
  happ_clear();
  hmodel_load("uac2-hs-default.txt");
  CHECK_EQ(u2ps_host_init(), U2PS_HOST_OK);
  CHECK_EQ(happ_log.ends, 0);
  u2ps_host_connect(true);
  u2ps_hfake_connect();
  CHECK(u2ps_hfake_wait(happ_done, 2000));
  CHECK_EQ(happ_log.devices, 1);
  /* Test mode with no session: no on_end */
  if (!hstart("uac2-hs-default.txt"))
    return;
  CHECK(u2ps_host_test_mode(5));
  CHECK_EQ(happ_log.ends, 0);
  CHECK_EQ(u2ps_hfake_test_selector, 5);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

int main(void)
{
  test_requests();
  test_waits();
  test_debounce_restarts();
  test_reset_segments();
  test_string_stall();
  test_string_text();
  test_clock_validity();
  test_clock_failures();
  test_range_kept();
  test_timeout();
  test_disconnect_mid_transfer();
  test_full_speed();
  test_ep0_mps();
  test_too_large();
  test_other_failures();
  test_second_configuration();
  test_refusal();
  test_second_session_fresh();
  test_composite();
  test_connect_and_vbus();
  test_port_disabled_and_reinit();
  test_test_mode();
  return check_done();
}
