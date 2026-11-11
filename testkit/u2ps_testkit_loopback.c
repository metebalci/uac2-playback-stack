/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_testkit_loopback.c: the PC test kit's loopback: the fake host
 * port's bus leads to the fake device port, and the kit's DAC app runs
 * the device stack. See u2ps_testkit_loopback.h.
 */
#include <stdio.h>
#include <string.h>

#include "fake_dport.h"
#include "fake_hport.h"
#include "u2ps_testkit_dev.h"
#include "u2ps_testkit_loopback.h"
#include "u2ps_testkit_priv.h"

/* ---- The default description -------------------------------------------------- */

static const u2ps_format_t default_formats[3] = {{2, 16, U2PS_AUDIO_DATA_FORMAT_PCM},
                                                 {4, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
                                                 {4, 32, U2PS_AUDIO_DATA_FORMAT_PCM}};
static const uint32_t default_rates[10] = {44100,  48000,  88200,  96000,  176400,
                                           192000, 352800, 384000, 705600, 768000};
static const u2ps_dev_description_t default_desc = {
  .id_vendor = 0x1234,
  .id_product = 0x5678,
  .bcd_device = 0x0100,
  .manufacturer = "u2ps",
  .product = "u2ps testkit DAC",
  .serial_number = "0001",
  .self_powered = false,
  .max_power_ma = 100,
  .category = 0x08,
  .terminal_type = 0x0301,
  .nr_channels = 2,
  .channel_config = 0x3,
  .formats = default_formats,
  .format_count = 3,
  .sampling_frequencies = default_rates,
  .sampling_frequency_count = 10,
};

/* ---- The bus between the two stacks -------------------------------------------- */

static u2ps_hfake_answer_t bridge(uint8_t addr, const u2ps_hfake_setup_t *s, uint8_t *data,
                                  uint16_t *len)
{
  (void)addr;
  uint16_t actual = 0;
  u2ps_setup_t setup = {s->bmRequestType, s->bRequest, s->wValue, s->wIndex, s->wLength};
  u2ps_testkit_control_t r =
    u2ps_testkit_dev_control(&setup, s->wLength > 0u ? data : NULL, &actual);
  if (r == U2PS_TESTKIT_CONTROL_OK) {
    *len = (s->bmRequestType & 0x80u) != 0u ? actual : s->wLength;
    return U2PS_HFAKE_ANSWER_OK;
  }
  return r == U2PS_TESTKIT_CONTROL_STALL ? U2PS_HFAKE_ANSWER_STALL : U2PS_HFAKE_ANSWER_ERROR;
}

static void bus_out(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  u2ps_testkit_dev_packet_bytes(data, len, uframe);
}

static void bus_corrupt(uint16_t len, u2ps_uframe_t uframe)
{
  (void)len;
  (void)uframe;
  u2ps_testkit_host_deliver_corrupted();
}

/* The device's feedback endpoint sends its current value */
static int bus_fb(uint8_t *data, uint16_t max, u2ps_uframe_t uframe)
{
  (void)uframe;
  if (max < 4u || !u2ps_testkit_dev_streaming())
    return -1;
  uint32_t v = u2ps_testkit_dev_feedback();
  for (unsigned i = 0; i < 4u; i++)
    data[i] = (uint8_t)(v >> (8u * i));
  return 4;
}

/* The DAC app's thread at each millisecond: a close due ends, the device
 * stack's tick */
static void ms_tick(uint32_t now)
{
  if (u2ps_fake_stream_state == U2PS_FAKE_ST_CLOSING)
    u2ps_fake_closed();
  (void)u2ps_dev_tick(now);
}

bool u2ps_testkit_loopback_reset(const u2ps_dev_description_t *desc)
{
  u2ps_testkit_host_reset();
  u2ps_testkit_dev_reset();
  u2ps_testkit_dev_vbus(true);
  u2ps_dev_err_t e = u2ps_dev_init(desc != NULL ? desc : &default_desc);
  if (e != U2PS_DEV_OK) {
    printf("u2ps testkit: loopback: u2ps_dev_init() returned %d (description error %d)\n", (int)e,
           (int)u2ps_dev_description_error());
    return false;
  }
  u2ps_dev_connect(true);
  u2ps_testkit_dev_bus_reset(U2PS_SPEED_HIGH);
  u2ps_hfake_device = bridge;
  u2ps_hfake_bus_out = bus_out;
  u2ps_hfake_bus_corrupt = bus_corrupt;
  u2ps_hfake_bus_fb = bus_fb;
  u2ps_hfake_ms_hook = ms_tick;
  return true;
}

/* ---- The kit's DAC app ---------------------------------------------------------- */

#define BUFFERS 8u
static _Alignas(U2PS_DPORT_ALIGN) uint8_t class_mem[BUFFERS * 1024u];
static uint32_t stream;

int u2ps_app_on_request(const u2ps_request_t *req)
{
  (void)req;
  return U2PS_REQ_STALL;
}

int u2ps_app_on_sampling_frequency(uint32_t requested, uint32_t chosen, uint32_t id)
{
  (void)requested;
  (void)chosen;
  (void)id;
  return 0;
}

void u2ps_app_on_configured(bool configured, u2ps_config_end_t why)
{
  (void)configured;
  (void)why;
}

u2ps_stream_start_t u2ps_app_on_stream_start(const u2ps_stream_info_t *info,
                                             u2ps_stream_cfg_t *cfg)
{
  stream = info->stream;
  cfg->mem = class_mem;
  cfg->stride = U2PS_DPORT_STRIDE(info->max_packet_size);
  cfg->count = BUFFERS;
  return U2PS_STREAM_OPEN;
}

void u2ps_app_on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  u2ps_testkit_host_deliver(data, len, uframe);
  u2ps_dev_release(stream, 1);
}

void u2ps_app_on_hole(uint16_t uframes, u2ps_miss_t why)
{
  (void)uframes;
  (void)why;
}

void u2ps_app_on_pause(uint16_t uframes)
{
  (void)uframes;
}

void u2ps_app_on_stream_stop(void)
{
}

void u2ps_app_on_fault(u2ps_fault_t fault)
{
  (void)fault;
}

void u2ps_app_on_reset(void)
{
}

void u2ps_app_on_suspend(void)
{
}

void u2ps_app_on_resume(void)
{
}

#if U2PS_VBUS != U2PS_VBUS_BUS
void u2ps_app_on_vbus(bool present)
{
  (void)present;
}
#endif

#if U2PS_CONTROL_HOOK
void u2ps_app_on_control(const u2ps_setup_t *setup, int result)
{
  (void)setup;
  (void)result;
}
#endif
