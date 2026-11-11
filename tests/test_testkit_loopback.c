/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_testkit_loopback.c: the PC test kit's loopback
 * (u2ps_testkit_loopback.h), through its public API only, with a small
 * player app. Checked:
 * - the u2ps device stack, with the kit's DAC app and its default
 *   description, enumerates: host_on_device with VID, PID and 3
 *   alternate settings;
 * - a stream at 192 kHz: the DAC's on_stream_start, every byte the player
 *   queued arrives at the DAC in order, in the microframe of its on_sent,
 *   and the device stack's feedback reaches the host at every poll and
 *   keeps the rate (no hole);
 * - an injected LATE hole: the player's host_on_hole(LATE) and the DAC's
 *   on_hole(NO_PACKET) of the same length;
 * - the player's stop: the DAC's on_stream_stop; no violation anywhere;
 * - the device stack ticks every millisecond: a description changed after
 *   the reset is reported (DEV_DESCRIPTION_CHANGED) within 2 ms;
 * - a description the device stack refuses: the reset returns false.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "u2ps_testkit_loopback.h"

static _Alignas(U2PS_HPORT_ALIGN) uint8_t buf[U2PS_HOST_QUEUE_MAX + 1u][1024];

static struct {
  bool device;
  uint32_t stream;
  uint16_t slot_bytes;
  unsigned in_flight, next;
  uint64_t bytes;
  u2ps_uframe_t sent_uframe;
} app;

static struct {
  uint64_t bytes;
  unsigned bad;
  u2ps_uframe_t uframe;
} rx;

static uint8_t pattern(uint64_t i)
{
  return (uint8_t)(i * 13u + (i >> 9));
}

static void fill(void)
{
  while (app.in_flight < 4u) {
    uint16_t n = u2ps_host_slots_next(app.stream);
    uint8_t *p = buf[app.next];
    for (unsigned i = 0; i < (unsigned)n * app.slot_bytes; i++)
      p[i] = pattern(app.bytes + i);
    if (!u2ps_host_queue(app.stream, p))
      return;
    app.bytes += (uint64_t)n * app.slot_bytes;
    app.next = (app.next + 1u) % (U2PS_HOST_QUEUE_MAX + 1u);
    app.in_flight++;
  }
}

void u2ps_app_host_on_device(const u2ps_host_device_t *dev)
{
  (void)dev;
  app.device = true;
}

void u2ps_app_host_on_end(u2ps_host_end_t why, u2ps_host_refusal_t refusal)
{
  (void)why;
  (void)refusal;
  app.device = false;
}

void u2ps_app_host_on_stream_start(const u2ps_host_stream_info_t *info)
{
  app.stream = info->stream;
  app.slot_bytes = (uint16_t)(info->nr_channels * info->format.subslot_size);
  fill();
}

void u2ps_app_host_on_sent(u2ps_uframe_t uframe)
{
  app.sent_uframe = uframe;
  app.in_flight--;
  fill();
}

void u2ps_app_host_on_hole(uint16_t uframes, u2ps_miss_t why)
{
  (void)uframes;
  (void)why;
}

void u2ps_app_host_on_stream_stop(u2ps_host_stop_t why, u2ps_host_stop_request_t request)
{
  (void)why;
  (void)request;
}

void u2ps_app_host_on_test_device(uint16_t id_product, uint16_t bcd_device)
{
  (void)id_product;
  (void)bcd_device;
}

void u2ps_app_host_on_resumed(bool remote_wakeup)
{
  (void)remote_wakeup;
}

void u2ps_app_on_contract(u2ps_contract_t violation)
{
  (void)violation;
}

static void on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  for (unsigned i = 0; i < len; i++)
    rx.bad += data[i] != pattern(rx.bytes + i);
  rx.bytes += len;
  rx.uframe = uframe;
}

static bool device_found(void)
{
  return app.device;
}

static int find(u2ps_testkit_callback_t callback, unsigned from)
{
  for (unsigned i = from; i < u2ps_testkit_trace_count(); i++) {
    const u2ps_testkit_call_t *c = u2ps_testkit_trace_get(i);
    if (c != NULL && c->callback == callback)
      return (int)i;
  }
  return -1;
}

int main(void)
{
  printf("enumerate\n");
  CHECK(u2ps_testkit_loopback_reset(NULL));
  u2ps_testkit_host_on_packet(on_packet);
  CHECK(u2ps_host_init() == U2PS_HOST_OK);
  u2ps_host_vbus(true);
  u2ps_host_connect(true);
  u2ps_testkit_host_connect();
  CHECK(u2ps_testkit_host_run_until(device_found, 2000));
  int d = find(U2PS_TESTKIT_HOST_ON_DEVICE, 0);
  const u2ps_testkit_call_t *c = d >= 0 ? u2ps_testkit_trace_get((unsigned)d) : NULL;
  CHECK(c != NULL && c->arg[0] == 0x1234u && c->arg[1] == 0x5678u && c->arg[2] == 3u);

  printf("stream\n");
  u2ps_testkit_trace_clear();
  CHECK(u2ps_host_stream_start(3, 192000) != 0u);
  u2ps_testkit_host_run_ms(200);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_ON_STREAM_START) == 1u);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_HOST_ON_STREAM_START) == 1u);
  int s = find(U2PS_TESTKIT_ON_STREAM_START, 0);
  c = s >= 0 ? u2ps_testkit_trace_get((unsigned)s) : NULL;
  CHECK(c != NULL && c->arg[1] == 3u && c->arg[2] == 192000u);
  const u2ps_testkit_received_t *r = u2ps_testkit_host_received();
  CHECK(r->packets > 1500u);
  CHECK(r->packets == u2ps_testkit_calls(U2PS_TESTKIT_HOST_ON_SENT));
  CHECK(r->packets == u2ps_testkit_calls(U2PS_TESTKIT_ON_PACKET));
  CHECK(rx.bad == 0u && rx.bytes == r->bytes);
  CHECK(r->last_uframe == app.sent_uframe);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_HOST_ON_HOLE) == 0u);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_ON_HOLE) == 0u);
  /* The device stack's feedback reached the host at every poll */
  u2ps_host_stats_t st;
  u2ps_host_stats(&st);
  CHECK(st.fb_polls > 100u && st.fb_missed == 0u && st.fb_rejected == 0u);

  printf("late\n");
  u2ps_testkit_trace_clear();
  u2ps_testkit_host_inject_late(2);
  u2ps_testkit_host_run_uframes(16);
  int h = find(U2PS_TESTKIT_HOST_ON_HOLE, 0);
  c = h >= 0 ? u2ps_testkit_trace_get((unsigned)h) : NULL;
  CHECK(c != NULL && c->arg[0] == 2u && c->arg[1] == U2PS_MISS_LATE);
  h = find(U2PS_TESTKIT_ON_HOLE, 0);
  c = h >= 0 ? u2ps_testkit_trace_get((unsigned)h) : NULL;
  CHECK(c != NULL && c->arg[0] == 2u && c->arg[1] == U2PS_MISS_NO_PACKET);
  CHECK(rx.bad == 0u && rx.bytes == r->bytes);

  printf("stop\n");
  u2ps_testkit_trace_clear();
  u2ps_host_stream_stop(app.stream);
  u2ps_testkit_host_run_ms(10);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_HOST_ON_STREAM_STOP) == 1u);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_ON_STREAM_STOP) == 1u);
  CHECK(rx.bad == 0u && rx.bytes == r->bytes);

  printf("the device stack's tick\n");
  /* A description the test changes after the reset: the device stack
   * sees it at its next tick, which the kit's DAC app gives every ms */
  static const u2ps_format_t formats[1] = {{2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}};
  static const uint32_t rates[1] = {48000};
  static u2ps_dev_description_t changing = {
    .id_vendor = 0x1234, .id_product = 0x5678, .max_power_ma = 100, .category = 0x08,
    .terminal_type = 0x0301, .nr_channels = 2, .channel_config = 0x3, .formats = formats,
    .format_count = 1, .sampling_frequencies = rates, .sampling_frequency_count = 1};
  CHECK(u2ps_testkit_loopback_reset(&changing));
  changing.max_power_ma = 200;
  u2ps_testkit_host_run_ms(2);
  CHECK(u2ps_testkit_contract_take(U2PS_CONTRACT_DEV_DESCRIPTION_CHANGED) == 1u);
  changing.max_power_ma = 100;

  printf("refused description\n");
  static const u2ps_dev_description_t bad = {.id_vendor = 1, .nr_channels = 0};
  CHECK(!u2ps_testkit_loopback_reset(&bad));
  /* Nothing left in the record: the kit's own verdict passes */
  CHECK_EQ(u2ps_testkit_result(), 0);
  return check_done();
}
