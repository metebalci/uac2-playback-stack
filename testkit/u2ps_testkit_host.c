/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_testkit_host.c: the PC test kit's host side: the fake host port
 * (fake_hport.h) with the device model (hmodel.h) as the scripted device,
 * and the functions the kit's copy of the host stack calls instead of the
 * app's callbacks, which record each in the trace and then call it
 * (testkit/CMakeLists.txt). See u2ps_testkit_host.h.
 */
#include <string.h>

#include "fake_hport.h"
#include "hmodel.h"
#include "u2ps_testkit_host.h"
#include "u2ps_testkit_priv.h"

/* ---- The record of the fake port's violations -------------------------------- */

static unsigned port_count(void)
{
  return u2ps_hfake_violations;
}

static void port_clear(void)
{
  u2ps_hfake_violations = 0;
}

static const u2ps_testkit_port_t port = {port_count, port_clear};

/* ---- The scripted device's side of the bus ------------------------------------- */

static u2ps_testkit_received_t received;
static void (*packet_hook)(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe);
static uint32_t feedback; /* 0: nominal */

void u2ps_testkit_host_deliver(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  received.packets++;
  received.bytes += len;
  received.last_len = len;
  received.last_uframe = uframe;
  if (packet_hook != NULL)
    packet_hook(data, len, uframe);
}

void u2ps_testkit_host_deliver_corrupted(void)
{
  received.corrupted++;
}

static void bus_corrupt(uint16_t len, u2ps_uframe_t uframe)
{
  (void)len;
  (void)uframe;
  u2ps_testkit_host_deliver_corrupted();
}

/* The feedback endpoint: 16.16 audio slots per microframe (UAC2 5.12.4.2),
 * the nominal rate of the device's sampling frequency unless set */
static int bus_fb(uint8_t *data, uint16_t max, u2ps_uframe_t uframe)
{
  (void)uframe;
  if (max < 4u)
    return -1;
  uint32_t v = feedback != 0u ? feedback
                              : (uint32_t)(((uint64_t)u2ps_hmodel.cur * 65536u + 4000u) / 8000u);
  for (unsigned i = 0; i < 4u; i++)
    data[i] = (uint8_t)(v >> (8u * i));
  return 4;
}

/* The app's thread at each millisecond: its poll, or the stack's tick */
static void tick(uint32_t now_ms)
{
  u2ps_testkit_set_now(now_ms);
  if (!u2ps_testkit_poll_run(now_ms))
    u2ps_host_tick(now_ms);
}

void u2ps_testkit_host_reset(void)
{
  u2ps_testkit_side_reset(1, &port);
  u2ps_hfake_reset_all();
  u2ps_hmodel_default();
  u2ps_hfake_device = u2ps_hmodel_answer;
  u2ps_hfake_bus_out = u2ps_testkit_host_deliver;
  u2ps_hfake_bus_corrupt = bus_corrupt;
  u2ps_hfake_bus_fb = bus_fb;
  u2ps_hfake_tick = tick;
  memset(&received, 0, sizeof received);
  packet_hook = NULL;
  feedback = 0;
}

void u2ps_testkit_host_connect(void)
{
  u2ps_hfake_connect();
}

void u2ps_testkit_host_disconnect(void)
{
  u2ps_hfake_disconnect();
}

void u2ps_testkit_host_wakeup(void)
{
  u2ps_hfake_wakeup();
}

bool u2ps_testkit_host_suspended(void)
{
  return u2ps_hfake_suspended();
}

uint8_t u2ps_testkit_host_test_mode(void)
{
  return u2ps_hfake_test_selector > 0 ? (uint8_t)u2ps_hfake_test_selector : 0u;
}

void u2ps_testkit_host_speed(u2ps_speed_t speed)
{
  u2ps_hfake_speed = speed;
  u2ps_hmodel.device[7] = speed == U2PS_SPEED_LOW ? 8u : 64u;
}

void u2ps_testkit_host_device_ids(uint16_t id_vendor, uint16_t id_product, uint16_t bcd_device)
{
  u2ps_hmodel.device[8] = (uint8_t)id_vendor;
  u2ps_hmodel.device[9] = (uint8_t)(id_vendor >> 8);
  u2ps_hmodel.device[10] = (uint8_t)id_product;
  u2ps_hmodel.device[11] = (uint8_t)(id_product >> 8);
  u2ps_hmodel.device[12] = (uint8_t)bcd_device;
  u2ps_hmodel.device[13] = (uint8_t)(bcd_device >> 8);
}

void u2ps_testkit_host_run_uframes(uint32_t uframes)
{
  u2ps_hfake_uframes(uframes);
}

void u2ps_testkit_host_run_ms(uint32_t ms)
{
  for (uint32_t i = 0; i < ms; i++)
    u2ps_hfake_uframes(8);
}

bool u2ps_testkit_host_run_until(bool (*done)(void), uint32_t max_ms)
{
  for (uint32_t i = 0; i < max_ms; i++) {
    if (done())
      return true;
    u2ps_hfake_uframes(8);
  }
  return done();
}

void u2ps_testkit_host_inject_late(uint16_t uframes)
{
  u2ps_hfake_inject_late(uframes);
}

void u2ps_testkit_host_inject_corrupted(void)
{
  u2ps_hfake_inject_corrupt();
}

unsigned u2ps_testkit_host_queued(void)
{
  return u2ps_hfake_queued;
}

void u2ps_testkit_host_configuration(const uint8_t *desc, uint16_t len)
{
  u2ps_hmodel_set_config(0, desc, len);
}

void u2ps_testkit_host_sampling_frequencies(const uint32_t *sampling_frequencies, unsigned n)
{
  u2ps_hmodel_set_rates(sampling_frequencies, n);
}

void u2ps_testkit_host_feedback(uint32_t value)
{
  feedback = value;
}

void u2ps_testkit_host_refuse_alternate_setting(int alternate_setting)
{
  u2ps_hmodel.stall_alt = alternate_setting;
}

void u2ps_testkit_host_refuse_sampling_frequency(bool refuse)
{
  u2ps_hmodel.stall_set_cur = refuse;
}

uint32_t u2ps_testkit_host_sampling_frequency(void)
{
  return u2ps_hmodel.cur;
}

uint8_t u2ps_testkit_host_alternate_setting(void)
{
  return u2ps_hmodel.alt;
}

const u2ps_testkit_received_t *u2ps_testkit_host_received(void)
{
  return &received;
}

void u2ps_testkit_host_on_packet(void (*on_packet)(const uint8_t *data, uint16_t len,
                                                   u2ps_uframe_t uframe))
{
  packet_hook = on_packet;
}

/* ---- The callbacks, traced ------------------------------------------------------
 * The kit's copy of the host stack calls these instead of the app's
 * callbacks (testkit/CMakeLists.txt) */

void u2ps_testkit_traced_host_on_device(const u2ps_host_device_t *dev);
void u2ps_testkit_traced_host_on_end(u2ps_host_end_t why, u2ps_host_refusal_t refusal);
void u2ps_testkit_traced_host_on_stream_start(const u2ps_host_stream_info_t *info);
void u2ps_testkit_traced_host_on_sent(u2ps_uframe_t uframe);
void u2ps_testkit_traced_host_on_hole(uint16_t uframes, u2ps_miss_t why);
void u2ps_testkit_traced_host_on_stream_stop(u2ps_host_stop_t why,
                                             u2ps_host_stop_request_t request);
void u2ps_testkit_traced_host_on_test_device(uint16_t id_product, uint16_t bcd_device);
void u2ps_testkit_traced_host_on_resumed(bool remote_wakeup);
void u2ps_testkit_traced_host_on_contract(u2ps_contract_t violation);

void u2ps_testkit_traced_host_on_device(const u2ps_host_device_t *dev)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_HOST_ON_DEVICE, dev->id_vendor, dev->id_product,
                            dev->alternate_setting_count, dev->sampling_frequency);
  u2ps_app_host_on_device(dev);
}

void u2ps_testkit_traced_host_on_end(u2ps_host_end_t why, u2ps_host_refusal_t refusal)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_HOST_ON_END, (uint32_t)why, (uint32_t)refusal, 0, 0);
  u2ps_app_host_on_end(why, refusal);
}

void u2ps_testkit_traced_host_on_stream_start(const u2ps_host_stream_info_t *info)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_HOST_ON_STREAM_START, info->stream,
                            info->alternate_setting, info->sampling_frequency,
                            info->max_packet_size);
  u2ps_app_host_on_stream_start(info);
}

void u2ps_testkit_traced_host_on_sent(u2ps_uframe_t uframe)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_HOST_ON_SENT, uframe, 0, 0, 0);
  u2ps_app_host_on_sent(uframe);
}

void u2ps_testkit_traced_host_on_hole(uint16_t uframes, u2ps_miss_t why)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_HOST_ON_HOLE, uframes, (uint32_t)why, 0, 0);
  u2ps_app_host_on_hole(uframes, why);
}

void u2ps_testkit_traced_host_on_stream_stop(u2ps_host_stop_t why,
                                             u2ps_host_stop_request_t request)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_HOST_ON_STREAM_STOP, (uint32_t)why, (uint32_t)request,
                            0, 0);
  u2ps_app_host_on_stream_stop(why, request);
}

void u2ps_testkit_traced_host_on_test_device(uint16_t id_product, uint16_t bcd_device)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_HOST_ON_TEST_DEVICE, id_product, bcd_device, 0, 0);
  u2ps_app_host_on_test_device(id_product, bcd_device);
}

void u2ps_testkit_traced_host_on_resumed(bool remote_wakeup)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_HOST_ON_RESUMED, remote_wakeup, 0, 0, 0);
  u2ps_app_host_on_resumed(remote_wakeup);
}

#if U2PS_CONTRACT_CHECKS
void u2ps_testkit_traced_host_on_contract(u2ps_contract_t violation)
{
  u2ps_testkit_record_contract(violation, 1);
  u2ps_app_on_contract(violation);
}
#endif
