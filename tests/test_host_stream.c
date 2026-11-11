/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_host_stream.c: the host stack's stream against the fake host port
 * and the device model:
 * - the start's requests in order, with their fields: SET CUR of the
 *   sampling frequency (only for a programmable clock), GET CUR, CUR of
 *   the Clock Validity Control, SET_INTERFACE; the port's stream with the
 *   feedback endpoint for explicit feedback only; on_stream_start's info;
 * - the start returns the stream's number, 0 if the stack refuses it at
 *   once; info carries the clock's validity;
 * - every accepted start gets exactly one on_stream_stop: refusals by the
 *   device, a port that refuses the stream, the app's stop at each step,
 *   and the session's end in each way; starts the stack refuses get none;
 * - a stop during a start cancels it at each of its requests, with
 *   SET_INTERFACE 0 only after an accepted SET_INTERFACE;
 * - the stop: the close, SET_INTERFACE 0, then on_stream_stop;
 * - the feedback check: length 4, top 4 bits zero, within +-12.5 % of the
 *   nominal rate; zero-length and rejected values counted;
 * - a CORRUPTED packet: reported before its on_sent, counts as sent;
 * - the cap by max_packet_size, counted in slots_clamped;
 * - queue errors, the latch, and uframe_back.
 * The loopback with the device stack, and the accumulator's numbers, are
 * test_host_loopback.c.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "host_support.h"
#include "u2ps_ch9.h"
#include "u2ps_uac2.h"

/* The feedback the device model sends: a value of 'fb_len' bytes, or no
 * answer with fb_len < 0 */
static uint32_t fb_value;
static int fb_len = 4;

static int bus_fb(uint8_t *data, uint16_t max, u2ps_uframe_t uframe)
{
  (void)uframe;
  if (fb_len < 0)
    return -1;
  for (int i = 0; i < fb_len && i < (int)max; i++)
    data[i] = (uint8_t)(fb_value >> (8 * (i & 3)));
  return fb_len;
}

static bool stream_decided(void)
{
  return happ_log.stream_starts > 0u || happ_log.stream_stops > 0u;
}

static bool stream_stopped(void)
{
  return happ_log.stream_stops > 0u;
}

/* Microframes until on_stream_stop, at most 100 ms */
static bool wait_stopped(void)
{
  for (unsigned i = 0; i < 800u && !stream_stopped(); i++)
    u2ps_hfake_uframes(1);
  return stream_stopped();
}

/* The device configured, the player and the sink on the bus */
static bool configured(const char *fixture)
{
  if (!hstart(fixture))
    return false;
  if (!henumerate() || happ_log.devices != 1u) {
    printf("  %s: not configured\n", fixture);
    return false;
  }
  hplayer_start();
  hsink_clear();
  u2ps_hfake_bus_out = hsink_packet;
  u2ps_hfake_bus_corrupt = hsink_lost;
  u2ps_hfake_bus_fb = bus_fb;
  fb_len = 4;
  fb_value = 0;
  return true;
}

/* A start, then 1 ms steps until on_stream_start or on_stream_stop: the
 * number the start returned, or 0 */
static uint32_t start(uint8_t alt, uint32_t hz)
{
  uint32_t n = u2ps_host_stream_start(alt, hz);
  if (n == 0u || !u2ps_hfake_wait(stream_decided, 100))
    return 0;
  return n;
}

/* The control transfers from 'from' on, in order */
static const u2ps_hfake_call_t *ctl_from(unsigned from, unsigned nth)
{
  for (unsigned i = from; i < u2ps_hfake_ncalls; i++)
    if (u2ps_hfake_calls[i].kind == U2PS_HFAKE_CONTROL && nth-- == 0u)
      return &u2ps_hfake_calls[i];
  return NULL;
}

static void check_req(const u2ps_hfake_call_t *c, uint8_t bm, uint8_t req, uint16_t value,
                      uint16_t index, uint16_t length)
{
  CHECK(c != NULL);
  if (c == NULL)
    return;
  CHECK_EQ(c->arg, 1);
  CHECK_EQ(c->setup.bmRequestType, bm);
  CHECK_EQ(c->setup.bRequest, req);
  CHECK_EQ(c->setup.wValue, value);
  CHECK_EQ(c->setup.wIndex, index);
  CHECK_EQ(c->setup.wLength, length);
}

static u2ps_host_stats_t stats(void)
{
  u2ps_host_stats_t st;
  u2ps_host_stats(&st);
  return st;
}

/* ---- The start ------------------------------------------------------------------ */

static void test_start_requests(void)
{
  if (!configured("uac2-hs-default.txt"))
    return;
  const u2ps_host_device_t *dev = happ_log.dev_given;
  CHECK_EQ(dev->clock_validity, U2PS_CLOCK_VALIDITY_VALID);
  u2ps_hmodel.valid = 0;
  unsigned from = u2ps_hfake_ncalls;
  CHECK_EQ(start(1, 96000), 1);
  CHECK_EQ(happ_log.stream_starts, 1);
  CHECK_EQ(happ_log.stream_stops, 0);
  check_req(ctl_from(from, 0), 0x21, U2PS_REQ_CUR, 0x0100, 0x0400, 4);
  check_req(ctl_from(from, 1), 0xA1, U2PS_REQ_CUR, 0x0100, 0x0400, 4);
  check_req(ctl_from(from, 2), 0xA1, U2PS_REQ_CUR, 0x0200, 0x0400, 1);
  check_req(ctl_from(from, 3), 0x01, U2PS_SET_INTERFACE, 1, 1, 0);
  CHECK(ctl_from(from, 4) == NULL);
  CHECK_EQ(u2ps_hmodel.cur, 96000);
  CHECK_EQ(u2ps_hmodel.alt, 1);
  /* The clock's validity read at the start, in the device the app has */
  CHECK_EQ(dev->clock_validity, U2PS_CLOCK_VALIDITY_INVALID);
  CHECK_EQ(dev->sampling_frequency, 96000);
  /* The port's stream after SET_INTERFACE */
  int open = u2ps_hfake_find(U2PS_HFAKE_STREAM_OPEN, from);
  CHECK(open > (int)(ctl_from(from, 3) - u2ps_hfake_calls));
  CHECK_EQ(u2ps_hfake_stream.addr, 1);
  CHECK_EQ(u2ps_hfake_stream.ep_out, 1);
  CHECK_EQ(u2ps_hfake_stream.ep_fb, 1);
  CHECK_EQ(u2ps_hfake_stream.fb_interval, 4);
  CHECK_EQ(u2ps_hfake_stream.mps, 388);
  CHECK_EQ(u2ps_hfake_stream.fb_mps, 4);
  /* What the app plays */
  const u2ps_host_stream_info_t *i = &happ_log.info;
  CHECK_EQ(i->stream, 1);
  CHECK_EQ(i->alternate_setting, 1);
  CHECK_EQ(i->format.subslot_size, 2);
  CHECK_EQ(i->format.bit_resolution, 16);
  CHECK_EQ(i->nr_channels, 2);
  CHECK_EQ(i->sampling_frequency, 96000);
  CHECK_EQ(i->max_packet_size, 388);
  CHECK_EQ(i->synchronization_type, U2PS_SYNCHRONIZATION_TYPE_ASYNCHRONOUS);
  CHECK(i->explicit_feedback);
  CHECK_EQ(i->nominal_rate, 786432); /* 12 slots, 16.16 */
  CHECK_EQ(i->clock_validity, U2PS_CLOCK_VALIDITY_INVALID);
  /* The device adjusts the frequency: the stream runs at the one read
   * back; 44.1 kHz in 16.16 rounds to the nearest */
  u2ps_host_stream_stop(i->stream);
  CHECK(wait_stopped());
  happ_clear();
  hplayer_start();
  u2ps_hmodel.fixed_cur = true;
  u2ps_hmodel.cur = 44100;
  u2ps_hmodel.valid = 1;
  CHECK_EQ(start(2, 48000), 2);
  CHECK_EQ(happ_log.info.clock_validity, U2PS_CLOCK_VALIDITY_VALID);
  CHECK_EQ(happ_log.info.sampling_frequency, 44100);
  CHECK_EQ(happ_log.info.nominal_rate, 361267); /* 361267.2 */
  CHECK_EQ(happ_log.info.stream, 2);
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_hmodel.unexpected, 0);
}

/* Adaptive and synchronous alternate settings open without the feedback
 * endpoint, an asynchronous one with it; a clock that is not
 * programmable gets no SET CUR; a STALL of its validity leaves it
 * unknown, and the stream starts */
static void test_start_sync_types(void)
{
  if (!configured("uac2-hs-sync-types.txt"))
    return;
  static const struct {
    uint8_t alt;
    uint8_t ep_fb;
    u2ps_synchronization_type_t type;
  } cases[] = {{1, 1, U2PS_SYNCHRONIZATION_TYPE_ASYNCHRONOUS},
               {2, 0, U2PS_SYNCHRONIZATION_TYPE_ADAPTIVE},
               {3, 0, U2PS_SYNCHRONIZATION_TYPE_SYNCHRONOUS}};
  for (unsigned k = 0; k < 3u; k++) {
    happ_clear();
    hplayer_start();
    CHECK(start(cases[k].alt, 48000));
    CHECK_EQ(happ_log.stream_starts, 1);
    CHECK_EQ(u2ps_hfake_stream.ep_fb, cases[k].ep_fb);
    CHECK_EQ(happ_log.info.synchronization_type, cases[k].type);
    CHECK_EQ(happ_log.info.explicit_feedback, cases[k].ep_fb != 0u);
    u2ps_hfake_uframes(80);
    u2ps_host_stream_stop(happ_log.info.stream);
    CHECK(wait_stopped());
    CHECK_EQ(happ_log.stream_stops, 1);
  }
  /* No feedback poll on the adaptive and synchronous streams */
  CHECK_EQ(stats().fb_polls, 10);
  /* A clock that is not programmable, a validity that stalls */
  if (!hstart("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.config[0][9 + 8 + 9 + 9 + 5] = 0x05; /* frequency read-only, validity read-only */
  CHECK(henumerate());
  hplayer_start();
  u2ps_hmodel.stall_valid = true;
  unsigned from = u2ps_hfake_ncalls;
  CHECK(start(1, 96000));
  CHECK_EQ(happ_log.stream_starts, 1);
  check_req(ctl_from(from, 0), 0xA1, U2PS_REQ_CUR, 0x0100, 0x0400, 4);
  check_req(ctl_from(from, 1), 0xA1, U2PS_REQ_CUR, 0x0200, 0x0400, 1);
  check_req(ctl_from(from, 2), 0x01, U2PS_SET_INTERFACE, 1, 1, 0);
  CHECK_EQ(happ_log.info.sampling_frequency, 48000);
  CHECK_EQ(happ_log.dev_given->clock_validity, U2PS_CLOCK_VALIDITY_UNKNOWN);
  CHECK_EQ(happ_log.info.clock_validity, U2PS_CLOCK_VALIDITY_UNKNOWN);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* Starts the stack refuses: 0, and no on_stream_stop follows */
static void test_start_refused_by_stack(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  CHECK_EQ(u2ps_host_stream_start(1, 48000), 0); /* no device */
  dl_t l;
  static uint8_t buf[600];
  if (!dl_load(&l, "uac2-hs-default.txt"))
    return;
  l.d[dl_data_ep(&l, 1)][5] |= 0x08; /* alternate setting 2: high bandwidth */
  unsigned n = dl_join(&l, buf, sizeof buf, true);
  u2ps_hmodel_set_config(0, buf, (uint16_t)n);
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  hplayer_start();
  CHECK_EQ(u2ps_host_stream_start(0, 48000), 0);
  CHECK_EQ(u2ps_host_stream_start(2, 48000), 0); /* skipped */
  CHECK_EQ(u2ps_host_stream_start(5, 48000), 0); /* not kept */
  uint32_t num = u2ps_host_stream_start(1, 48000);
  CHECK(num != 0u);
  CHECK_EQ(u2ps_host_stream_start(3, 48000), 0); /* while a start runs */
  CHECK(u2ps_hfake_wait(stream_decided, 100));
  CHECK_EQ(u2ps_host_stream_start(3, 48000), 0); /* while a stream runs */
  u2ps_hfake_uframes(16);
  CHECK_EQ(happ_log.info.stream, num);
  CHECK_EQ(happ_log.stream_starts, 1);
  CHECK_EQ(happ_log.stream_stops, 0);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* The device refuses a request of the start, or the port the stream:
 * exactly one on_stream_stop, whose request names the one refused, and
 * the device stays configured. A stalled Clock Validity read refuses
 * nothing. */
static void test_start_refused(void)
{
  /* Per case: the reason, the request, and the index of the last control
   * transfer of the start (SET CUR 0, GET CUR 1, validity 2, SET_INTERFACE
   * 3, SET_INTERFACE 0 4) */
  static const struct {
    u2ps_host_stop_t stop;
    u2ps_host_stop_request_t request;
    unsigned last;
  } cases[] = {
    {U2PS_HOST_STOP_REFUSED, U2PS_HOST_STOP_REQUEST_SET_SAMPLING_FREQUENCY, 0},
    {U2PS_HOST_STOP_REFUSED, U2PS_HOST_STOP_REQUEST_GET_SAMPLING_FREQUENCY, 1},
    {U2PS_HOST_STOP_REFUSED, U2PS_HOST_STOP_REQUEST_SAMPLING_FREQUENCY_VALUE, 1},
    {U2PS_HOST_STOP_REFUSED, U2PS_HOST_STOP_REQUEST_SAMPLING_FREQUENCY_VALUE, 1},
    {U2PS_HOST_STOP_REFUSED, U2PS_HOST_STOP_REQUEST_SET_INTERFACE, 3},
    {U2PS_HOST_STOP_OPEN, U2PS_HOST_STOP_REQUEST_NONE, 4}};
  for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
    if (!configured("uac2-hs-default.txt"))
      return;
    if (k == 0u)
      u2ps_hmodel.stall_set_cur = true;
    else if (k == 1u)
      u2ps_hmodel.stall_cur = true;
    else if (k == 2u)
      u2ps_hmodel.fixed_cur = true, u2ps_hmodel.cur = 0; /* a frequency of 0 Hz read back */
    else if (k == 3u)
      u2ps_hmodel.short_cur = true; /* 3 bytes read back */
    else if (k == 4u)
      u2ps_hmodel.stall_alt = 1;
    else
      u2ps_hfake_open_result = false;
    unsigned from = u2ps_hfake_ncalls;
    CHECK(u2ps_host_stream_start(1, 48000));
    u2ps_hfake_run_until(u2ps_hfake_now + 100);
    CHECK_EQ(happ_log.stream_starts, 0);
    CHECK_EQ(happ_log.stream_stops, 1);
    CHECK_EQ(happ_log.stop, cases[k].stop);
    CHECK_EQ(happ_log.stop_request, cases[k].request);
    CHECK_EQ(happ_log.stop_from, U2PS_HFAKE_FROM_UPCALL);
    CHECK_EQ(happ_log.ends, 0);
    /* The port refused: SET_INTERFACE 0 before on_stream_stop */
    const u2ps_hfake_call_t *last = ctl_from(from, cases[k].last);
    CHECK(last != NULL);
    if (cases[k].stop == U2PS_HOST_STOP_OPEN)
      check_req(last, 0x01, U2PS_SET_INTERFACE, 0, 1, 0);
    CHECK(ctl_from(from, cases[k].last + 1u) == NULL);
    if (cases[k].stop == U2PS_HOST_STOP_OPEN)
      CHECK(happ_log.calls_at_stop > (unsigned)(last - u2ps_hfake_calls));
    /* Still configured: another start works */
    u2ps_hmodel.stall_set_cur = u2ps_hmodel.stall_cur = u2ps_hmodel.fixed_cur = u2ps_hmodel.short_cur = false;
    u2ps_hmodel.stall_alt = -1;
    u2ps_hfake_open_result = true;
    happ_clear();
    hplayer_start();
    CHECK(start(1, 48000));
    CHECK_EQ(happ_log.stream_starts, 1);
    CHECK_EQ(u2ps_hfake_violations, 0);
  }
}

/* ---- The stop ------------------------------------------------------------------- */

static void test_stop(void)
{
  if (!configured("uac2-hs-default.txt") || !start(1, 48000))
    return;
  u2ps_hfake_uframes(40);
  unsigned sent = happ_log.sent, from = u2ps_hfake_ncalls;
  CHECK(sent >= 30u);
  u2ps_host_stream_stop(happ_log.info.stream - 1u); /* another stream's number */
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_STREAM_CLOSE), 0);
  u2ps_host_stream_stop(happ_log.info.stream);
  int close = u2ps_hfake_find(U2PS_HFAKE_STREAM_CLOSE, from);
  CHECK(close >= 0);
  CHECK(close >= 0 && u2ps_hfake_calls[close].from == U2PS_HFAKE_FROM_MASKED);
  /* No packet after the stop; on_closed at the next microframe, then
   * SET_INTERFACE 0, then on_stream_stop(APP) */
  static _Alignas(4) uint8_t pk[64];
  CHECK(!u2ps_host_queue(happ_log.info.stream, pk));
  CHECK(wait_stopped());
  CHECK_EQ(happ_log.stream_stops, 1);
  CHECK_EQ(happ_log.stop, U2PS_HOST_STOP_APP);
  CHECK_EQ(happ_log.stop_request, U2PS_HOST_STOP_REQUEST_NONE);
  CHECK_EQ(happ_log.stop_from, U2PS_HFAKE_FROM_UPCALL);
  check_req(ctl_from(from, 0), 0x01, U2PS_SET_INTERFACE, 0, 1, 0);
  CHECK_EQ(u2ps_hmodel.alt, 0);
  CHECK(happ_log.sent <= sent + 1u);
  CHECK_EQ(u2ps_hfake_stream_state, U2PS_HFAKE_ST_IDLE);
  /* A second stop does nothing */
  u2ps_host_stream_stop(happ_log.info.stream);
  u2ps_hfake_run_until(u2ps_hfake_now + 20);
  CHECK_EQ(happ_log.stream_stops, 1);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* A stop with the last stream's number during the next start changes
 * nothing: the start goes on to on_stream_start */
static void test_stop_during_start(void)
{
  if (!configured("uac2-hs-default.txt") || !start(1, 48000))
    return;
  uint32_t first = happ_log.info.stream;
  u2ps_host_stream_stop(first);
  CHECK(wait_stopped());
  happ_clear();
  hplayer_start();
  CHECK(u2ps_host_stream_start(2, 48000));
  for (unsigned k = 0; k < 4u; k++) {
    u2ps_host_stream_stop(first);
    u2ps_hfake_step();
  }
  CHECK(u2ps_hfake_wait(stream_decided, 100));
  CHECK_EQ(happ_log.stream_starts, 1);
  CHECK_EQ(happ_log.stream_stops, 0);
  CHECK_EQ(happ_log.info.stream, first + 1u);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* A stop with the start's own number cancels it, whichever of its requests
 * is in flight (0 SET CUR, 1 GET CUR, 2 the clock's validity, 3
 * SET_INTERFACE), and whether the device accepts or stalls it: that
 * request completes, no further one of the start follows, the port's
 * stream never opens, and on_stream_stop(APP) comes once, without
 * on_stream_start. A SET_INTERFACE the device accepted is undone with
 * SET_INTERFACE 0 before on_stream_stop; one it stalled is not. A second
 * stop changes nothing; the next start works and gets the next number. */
static void test_stop_cancels_start(void)
{
  static const struct {
    unsigned at; /* the request in flight at the stop */
    bool stall;  /* the device stalls it */
  } cases[] = {{0, false}, {1, false}, {2, false}, {3, false}, {0, true}, {1, true}, {3, true}};
  for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
    if (!configured("uac2-hs-default.txt"))
      return;
    unsigned at = cases[k].at;
    if (cases[k].stall) {
      u2ps_hmodel.stall_set_cur = at == 0u;
      u2ps_hmodel.stall_cur = at == 1u;
      u2ps_hmodel.stall_alt = at == 3u ? 1 : -1;
    }
    unsigned from = u2ps_hfake_ncalls;
    uint32_t n = u2ps_host_stream_start(1, 96000);
    CHECK(n != 0u);
    for (unsigned i = 0; i < at; i++)
      u2ps_hfake_step();
    /* The start's request 'at' is in flight */
    CHECK(u2ps_hfake_control_pending());
    CHECK(ctl_from(from, at) != NULL);
    CHECK(ctl_from(from, at + 1u) == NULL);
    u2ps_host_stream_stop(n);
    u2ps_host_stream_stop(n);
    CHECK_EQ(happ_log.stream_stops, 0); /* at the request's completion */
    u2ps_hfake_run_until(u2ps_hfake_now + 100);
    CHECK_EQ(happ_log.stream_starts, 0);
    CHECK_EQ(happ_log.stream_stops, 1);
    CHECK_EQ(happ_log.stop, U2PS_HOST_STOP_APP);
    CHECK_EQ(happ_log.stop_request, U2PS_HOST_STOP_REQUEST_NONE);
    CHECK_EQ(happ_log.stop_from, U2PS_HFAKE_FROM_UPCALL);
    CHECK_EQ(u2ps_hfake_find(U2PS_HFAKE_STREAM_OPEN, from), -1);
    CHECK_EQ(happ_log.ends, 0);
    if (at == 3u && !cases[k].stall) {
      /* SET_INTERFACE 0 after the accepted SET_INTERFACE, then the stop */
      const u2ps_hfake_call_t *alt0 = ctl_from(from, 4);
      check_req(alt0, 0x01, U2PS_SET_INTERFACE, 0, 1, 0);
      CHECK(alt0 != NULL && happ_log.calls_at_stop > (unsigned)(alt0 - u2ps_hfake_calls));
      CHECK(ctl_from(from, 5) == NULL);
    } else {
      CHECK(ctl_from(from, at + 1u) == NULL);
    }
    CHECK_EQ(u2ps_hmodel.alt, 0);
    /* A read back that completed updates the device's frequency; a cancel
     * right after SET CUR leaves the last one read */
    if (k == 0u)
      CHECK_EQ(happ_log.dev_given->sampling_frequency, 48000);
    if (k == 1u)
      CHECK_EQ(happ_log.dev_given->sampling_frequency, 96000);
    /* Still configured: the next start gets the next number */
    u2ps_hmodel.stall_set_cur = u2ps_hmodel.stall_cur = false;
    u2ps_hmodel.stall_alt = -1;
    happ_clear();
    hplayer_start();
    CHECK_EQ(start(1, 48000), n + 1u);
    CHECK_EQ(happ_log.stream_starts, 1);
    CHECK_EQ(u2ps_hfake_violations, 0);
    CHECK_EQ(u2ps_hmodel.unexpected, 0);
  }
}

/* A cancelled start whose request in flight fails ends the session as
 * without the stop: on_stream_stop(END), then on_end */
static void test_stop_cancel_then_error(void)
{
  if (!configured("uac2-hs-default.txt"))
    return;
  u2ps_hmodel.error_at = (int)u2ps_hmodel.requests + 1; /* the start's GET CUR */
  uint32_t n = u2ps_host_stream_start(1, 48000);
  u2ps_hfake_step();
  u2ps_host_stream_stop(n);
  u2ps_hfake_run_until(u2ps_hfake_now + 100);
  CHECK_EQ(happ_log.stream_starts, 0);
  CHECK_EQ(happ_log.stream_stops, 1);
  CHECK_EQ(happ_log.stop, U2PS_HOST_STOP_END);
  CHECK_EQ(happ_log.stop_request, U2PS_HOST_STOP_REQUEST_NONE);
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, U2PS_HOST_END_ERROR);
  CHECK(strstr(happ_log.events, "XE") != NULL);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* The session ends with a stream: on_stream_stop(END) before on_end; an
 * open stream's port stops before on_stream_stop, so its packets are the
 * app's there */
static void test_session_end(void)
{
  for (unsigned k = 0; k < 7u; k++) {
    if (!configured("uac2-hs-default.txt") || !start(1, 48000))
      return;
    u2ps_hfake_uframes(20);
    unsigned from = u2ps_hfake_ncalls;
    u2ps_host_end_t why = U2PS_HOST_END_DISCONNECT;
    switch (k) {
    case 0:
      u2ps_hfake_disconnect();
      break;
    case 1:
      u2ps_host_connect(false);
      why = U2PS_HOST_END_DETACH;
      break;
    case 2:
      u2ps_host_vbus(false);
      why = U2PS_HOST_END_VBUS;
      break;
    case 3:
      u2ps_hfake_disabled();
      why = U2PS_HOST_END_DISABLED;
      break;
    case 4:
      u2ps_host_init();
      why = U2PS_HOST_END_REINIT;
      break;
    case 5:
      u2ps_host_test_mode(4);
      why = U2PS_HOST_END_TEST_MODE;
      break;
    default:
      /* The stop's SET_INTERFACE 0 hangs: the timeout ends the session */
      u2ps_hmodel.hang_at = (int)u2ps_hmodel.requests;
      u2ps_host_stream_stop(happ_log.info.stream);
      u2ps_hfake_uframes(8u * (U2PS_HOST_CONTROL_MAX + 20u));
      why = U2PS_HOST_END_TIMEOUT;
      break;
    }
    CHECK_EQ(happ_log.stream_stops, 1);
    CHECK_EQ(happ_log.stop, U2PS_HOST_STOP_END);
    CHECK_EQ(happ_log.stop_request, U2PS_HOST_STOP_REQUEST_NONE);
    CHECK_EQ(happ_log.ends, 1);
    CHECK_EQ(happ_log.end, why);
    CHECK(strstr(happ_log.events, "XE") != NULL);
    CHECK_EQ(happ_log.queued_at_stop, 0);
    /* The port stopped by itself, or by the stack before on_stream_stop */
    int dis = u2ps_hfake_find(U2PS_HFAKE_DISABLE, from);
    if (k == 0u || k == 3u)
      CHECK_EQ(dis, -1);
    else if (k != 6u)
      CHECK(dis >= 0 && (unsigned)dis < happ_log.calls_at_stop);
    CHECK_EQ(u2ps_hfake_violations, 0);
  }
}

/* ---- Feedback -------------------------------------------------------------------- */

/* Run n microframes and return the slots queued meanwhile, and the packets */
static uint64_t run(unsigned n, unsigned *packets)
{
  uint64_t s0 = hplayer.slots;
  unsigned p0 = hplayer.packets;
  u2ps_hfake_uframes(n);
  *packets = hplayer.packets - p0;
  return hplayer.slots - s0;
}

/* Over 'packets' packets at 'ff' (16.16), the slots differ from the rate
 * by less than one */
static void check_rate(uint64_t slots, unsigned packets, uint32_t ff)
{
  int64_t d = (int64_t)(slots << 16) - (int64_t)packets * ff;
  CHECK(d > -(1 << 16) && d < (1 << 16));
  if (!(d > -(1 << 16) && d < (1 << 16)))
    printf("  %llu slots in %u packets at 0x%x\n", (unsigned long long)slots, packets, ff);
}

static void test_feedback(void)
{
  if (!configured("uac2-hs-default.txt"))
    return;
  fb_len = -1; /* no answer: the nominal rate */
  CHECK(start(1, 48000));
  unsigned p;
  uint64_t slots = run(800, &p);
  check_rate(slots, p, 6u << 16);
  u2ps_host_stats_t st = stats();
  CHECK_EQ(st.fb_polls, 100);
  CHECK_EQ(st.fb_missed, 100);
  /* A valid value: 6.5 slots */
  fb_len = 4;
  fb_value = 0x68000;
  u2ps_hfake_uframes(16);
  slots = run(800, &p);
  check_rate(slots, p, 0x68000);
  /* Zero-length, 3 bytes, the top bits set, beyond +-12.5 %: the rate
   * stays */
  static const struct {
    int len;
    uint32_t value;
  } bad[] = {{0, 0x60000}, {3, 0x60000}, {4, 0x10060000}, {4, 0x6C001}, {4, 0x53FFF}};
  for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
    st = stats();
    fb_len = bad[k].len;
    fb_value = bad[k].value;
    slots = run(80, &p);
    check_rate(slots, p, 0x68000);
    u2ps_host_stats_t st2 = stats();
    CHECK_EQ(st2.fb_zero_length - st.fb_zero_length, k == 0u ? 10u : 0u);
    CHECK_EQ(st2.fb_rejected - st.fb_rejected, k == 0u ? 0u : 10u);
  }
  /* The window's edges: 6.75 and 5.25 slots are valid */
  fb_len = 4;
  fb_value = 0x6C000;
  u2ps_hfake_uframes(16);
  slots = run(800, &p);
  check_rate(slots, p, 0x6C000);
  fb_value = 0x54000;
  u2ps_hfake_uframes(16);
  slots = run(800, &p);
  check_rate(slots, p, 0x54000);
  /* A skipped poll changes nothing */
  st = stats();
  u2ps_hfake_inject_fb_late();
  slots = run(80, &p);
  check_rate(slots, p, 0x54000);
  CHECK_EQ(stats().fb_missed - st.fb_missed, 1);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* ---- Holes the stack only passes on ------------------------------------------------- */

/* A CORRUPTED packet: on_hole(1, CORRUPTED) immediately before its
 * on_sent; it counts as sent, so the sizes go on as before */
static void test_corrupted(void)
{
  if (!configured("uac2-hs-default.txt"))
    return;
  fb_len = -1;
  CHECK(start(1, 48000));
  u2ps_hfake_uframes(40);
  unsigned sent = happ_log.sent;
  u2ps_hfake_inject_corrupt();
  u2ps_hfake_uframes(1);
  CHECK_EQ(happ_log.nholes, 1);
  CHECK_EQ(happ_log.holes[0].uframes, 1);
  CHECK_EQ(happ_log.holes[0].why, U2PS_MISS_CORRUPTED);
  CHECK_EQ(happ_log.holes[0].sent_before, sent);
  CHECK_EQ(happ_log.sent, sent + 1u);
  unsigned p;
  uint64_t slots = run(800, &p);
  check_rate(slots, p, 6u << 16);
  for (unsigned i = 0; i < hplayer.packets; i++)
    CHECK_EQ(hplayer.log[i], 6);
  u2ps_host_stats_t st = stats();
  CHECK_EQ(st.corrupted, 1);
  CHECK_EQ(st.errors, 1);
  CHECK_EQ(hsink.bad, 0);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* ---- The cap ----------------------------------------------------------------------- */

/* wMaxPacketSize 24 bytes (6 slots of 16/2) under a feedback of 6.75
 * slots: no packet is longer, and the cut slots are counted */
static void test_cap(void)
{
  dl_t l;
  static uint8_t buf[600];
  if (!hstart("uac2-hs-default.txt") || !dl_load(&l, "uac2-hs-default.txt"))
    return;
  dl_put16(&l, (unsigned)dl_data_ep(&l, 0), 4, 24);
  unsigned n = dl_join(&l, buf, sizeof buf, true);
  u2ps_hmodel_set_config(0, buf, (uint16_t)n);
  CHECK(henumerate());
  hplayer_start();
  hsink_clear();
  u2ps_hfake_bus_out = hsink_packet;
  u2ps_hfake_bus_fb = bus_fb;
  fb_len = 4;
  fb_value = 0x6C000;
  CHECK(start(1, 48000));
  u2ps_hfake_uframes(16);
  u2ps_host_stats_t st = stats();
  unsigned p;
  uint64_t slots = run(8000, &p);
  u2ps_host_stats_t st2 = stats();
  uint32_t clamped = st2.slots_clamped - st.slots_clamped;
  for (unsigned i = 0; i < hsink.packets; i++)
    CHECK(hsink.len[i] <= 24u);
  CHECK_EQ(slots, 6u * p);
  CHECK(clamped > 0u);
  check_rate(slots + clamped, p, 0x6C000);
  CHECK_EQ(hsink.bad, 0);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* ---- Queue errors, the latch, uframe_back ---------------------------------------------- */

static void test_queue_errors(void)
{
  if (!configured("uac2-hs-default.txt"))
    return;
  hplayer.on = false;
  CHECK(start(1, 44100));
  uint32_t s = happ_log.info.stream;
  static _Alignas(4) uint8_t pk[U2PS_HOST_QUEUE_MAX + 1u][64];
  /* Each refusal below breaks the contract, and so does each packet queued
   * without u2ps_host_slots_next() */
  contract_expect(true);
  /* The latch: slots_next() is the same until the packet is queued */
  uint16_t a = u2ps_host_slots_next(s);
  CHECK_EQ(u2ps_host_slots_next(s), a);
  CHECK_EQ(a, 5);
  CHECK_EQ(u2ps_host_slots_next(s + 1u), 0);
  CHECK(!u2ps_host_queue(s + 1u, pk[0]));
  CHECK(!u2ps_host_queue(s, pk[0] + 2));
  CHECK(!u2ps_host_queue(s, NULL));
  CHECK_EQ(stats().queue_errors, 3);
  /* The queue holds U2PS_HOST_QUEUE_MAX packets */
  for (unsigned i = 0; i < U2PS_HOST_QUEUE_MAX; i++)
    CHECK(u2ps_host_queue(s, pk[i]));
  CHECK(!u2ps_host_queue(s, pk[U2PS_HOST_QUEUE_MAX]));
  CHECK_EQ(stats().queue_errors, 4);
  CHECK_EQ(u2ps_hfake_queued, U2PS_HOST_QUEUE_MAX);
  /* 44.1 kHz: 5 slots, then 6 once the fraction reaches 1 */
  CHECK_EQ(hplayer.packets, 0);
  u2ps_hfake_uframes(1);
  CHECK(u2ps_host_queue(s, pk[U2PS_HOST_QUEUE_MAX]));
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_HOST_QUEUE_STREAM], CONTRACT_N(1));
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_HOST_QUEUE_DATA], CONTRACT_N(2));
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_HOST_QUEUE_FULL], CONTRACT_N(1));
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_HOST_QUEUE_UNSIZED], CONTRACT_N(16));
  CHECK_EQ(contract_log.total, CONTRACT_N(20));
  contract_expect(false);
  /* uframe_back: a microframe not after the last one's */
  u2ps_uframe_t last = happ_log.sent_uframe;
  u2ps_host_on_sent(last);
  CHECK_EQ(stats().uframe_back, 1);
  u2ps_host_on_sent(u2ps_uframe_add(last, 1));
  CHECK_EQ(stats().uframe_back, 1);
}

/* The feedback endpoint's bInterval: USB 2.0 allows 1..16 (9.6.6), the
 * DWC2 port only 1..13 (u2ps_dwc2_hcd.h), and the test kit's fake port
 * refuses 14..16 the same way (U2PS_HFAKE_FB_INTERVAL_MAX): the open is
 * refused, not a port violation, and the start ends with
 * U2PS_HOST_STOP_OPEN after SET_INTERFACE 0; 13 starts */
static void test_fb_interval(void)
{
  for (uint8_t iv = 12; iv <= 16; iv++) {
    if (!hstart("uac2-hs-default.txt"))
      return;
    /* Every feedback endpoint (IN, isochronous) of the configuration */
    unsigned n = 0;
    uint8_t *c = u2ps_hmodel.config[0];
    for (unsigned i = 0; i + 7u <= u2ps_hmodel.config_len[0] && c[i] != 0u; i += c[i]) {
      if (c[i] == 7u && c[i + 1u] == U2PS_DESC_ENDPOINT && (c[i + 2u] & 0x80u) != 0u &&
          (c[i + 3u] & 0x3u) == 1u) {
        c[i + 6u] = iv;
        n++;
      }
    }
    CHECK_EQ(n, 4);
    if (!henumerate() || happ_log.devices != 1u) {
      CHECK(false);
      return;
    }
    hplayer_start();
    u2ps_hfake_bus_fb = bus_fb;
    fb_len = 4;
    CHECK(u2ps_host_stream_start(1, 48000) != 0u);
    u2ps_hfake_run_until(u2ps_hfake_now + 100);
    CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_STREAM_OPEN), 1);
    if (iv <= U2PS_HFAKE_FB_INTERVAL_MAX) {
      CHECK_EQ(u2ps_hfake_stream.fb_interval, iv);
      CHECK_EQ(happ_log.stream_starts, 1);
      CHECK_EQ(happ_log.stream_stops, 0);
      CHECK_EQ(u2ps_hfake_stream_state, U2PS_HFAKE_ST_OPEN);
    } else {
      CHECK_EQ(happ_log.stream_starts, 0);
      CHECK_EQ(happ_log.stream_stops, 1);
      CHECK_EQ(happ_log.stop, U2PS_HOST_STOP_OPEN);
      CHECK_EQ(u2ps_hfake_stream_state, U2PS_HFAKE_ST_IDLE);
    }
    CHECK_EQ(u2ps_hfake_violations, 0);
  }
  CHECK_EQ(U2PS_HFAKE_FB_INTERVAL_MAX, 13);
}

/* The DMA ranges of the port (u2ps_hport_dma_ranges): asked once per
 * successful init in a build with the contract checks, never without
 * them, and never after a failed init */
static void test_dma_ranges_asked(void)
{
  if (!hstart("uac2-hs-default.txt"))
    return;
  CHECK_EQ(u2ps_hfake_range_calls, U2PS_CONTRACT_CHECKS ? 1 : 0);
  u2ps_hfake_reset_all();
  u2ps_hfake_init_err = U2PS_HOST_ERR_PHY;
  CHECK_EQ(u2ps_host_init(), U2PS_HOST_ERR_PHY);
  CHECK_EQ(u2ps_hfake_range_calls, 0);
  u2ps_hfake_init_err = U2PS_HOST_OK;
  CHECK_EQ(u2ps_host_init(), U2PS_HOST_OK);
  CHECK_EQ(u2ps_hfake_range_calls, U2PS_CONTRACT_CHECKS ? 1 : 0);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

int main(void)
{
  test_start_requests();
  test_start_sync_types();
  test_start_refused_by_stack();
  test_start_refused();
  test_stop();
  test_stop_during_start();
  test_stop_cancels_start();
  test_stop_cancel_then_error();
  test_session_end();
  test_feedback();
  test_corrupted();
  test_cap();
  test_queue_errors();
  test_fb_interval();
  test_dma_ranges_asked();
  return check_done();
}
