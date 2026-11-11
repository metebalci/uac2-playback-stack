/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * dev_support.c: see dev_support.h.
 */
#include "dev_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* bcdUSB 0x0200, the IAD class codes EF/02/01 (UAC2 4.2), bMaxPacketSize0
 * 64, test IDs, bcdDevice 0x0100, strings 1, 2, 3, one configuration
 * (USB 2.0 9.6.1) */
uint8_t test_device[18] = {18,   U2PS_DESC_DEVICE, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 0x34,
                           0x12, 0x78,               0x56, 0x00, 0x01, 1,    2,    3,    1};
uint8_t test_config_hs[512];
uint16_t test_config_hs_len;
uint8_t test_config_fs[512];
uint16_t test_config_fs_len;

static const uint8_t string0[] = {4, U2PS_DESC_STRING, 0x09, 0x04};
static uint8_t string1[16];  /* 7 characters */
static uint8_t string2[64];  /* 31 characters: one full packet */
static uint8_t string3[128]; /* 63 characters: two full packets */
const uint8_t *const test_strings[4] = {string0, string1, string2, string3};
static char text1[8], text2[32], text3[64];

const u2ps_format_t test_formats[4] = {{2, 16, U2PS_AUDIO_DATA_FORMAT_PCM},
                                       {4, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
                                       {4, 32, U2PS_AUDIO_DATA_FORMAT_PCM},
                                       {3, 24, U2PS_AUDIO_DATA_FORMAT_PCM}};
const uint32_t test_rates[10] = {44100,  48000,  88200,  96000,  176400,
                                 192000, 352800, 384000, 705600, 768000};

u2ps_dev_description_t test_desc;

/* The string descriptor of len bytes and its text: letters from 'first' */
static void make_string(uint8_t *s, char *text, unsigned len, char first)
{
  s[0] = (uint8_t)len;
  s[1] = U2PS_DESC_STRING;
  for (unsigned i = 2; i + 1u < len; i += 2) {
    char c = (char)(first + (char)((i / 2u - 1u) % 26u));
    s[i] = (uint8_t)c;
    s[i + 1] = 0;
    text[i / 2u - 1u] = c;
  }
  text[len / 2u - 1u] = '\0';
}

bool test_desc_load(void)
{
  int hs = fixture_load("uac2-hs-default.txt", test_config_hs, sizeof test_config_hs);
  int fs = fixture_load("uac2-fs-minimal.txt", test_config_fs, sizeof test_config_fs);
  if (hs < 0 || fs < 0)
    return false;
  test_config_hs_len = (uint16_t)hs;
  test_config_fs_len = (uint16_t)fs;
  make_string(string1, text1, sizeof string1, 'a');
  make_string(string2, text2, sizeof string2, 'A');
  make_string(string3, text3, sizeof string3, 'a');
  test_desc = (u2ps_dev_description_t){
    .id_vendor = 0x1234,
    .id_product = 0x5678,
    .bcd_device = 0x0100,
    .manufacturer = text1,
    .product = text2,
    .serial_number = text3,
    .self_powered = false,
    .max_power_ma = 100,
    .category = 0x08,
    .terminal_type = 0x0301,
    .nr_channels = 2,
    .channel_config = 0x3,
    .formats = test_formats,
    .format_count = 4,
    .sampling_frequencies = test_rates,
    .sampling_frequency_count = TEST_RATE_COUNT,
  };
  return true;
}

/* A re-init keeps the clock's sampling frequency when the new description
 * lists it. After its first start, dev_start() re-inits with this one
 * first, whose only rate no test uses, so that every start begins at
 * test_desc's initial one. */
static const uint32_t start_rate[1] = {8000};
static u2ps_dev_description_t start_desc;
static bool started;

void dev_start(u2ps_speed_t speed)
{
  u2ps_fake_reset_all();
  u2ps_fake_vbus_set(true);
  app_handler = NULL;
  app_packet_hook = NULL;
  app_event_hook = NULL;
  app_cfg = (app_cfg_t){
    .decline = false, .defer = false, .release_each = true, .count = 4, .fb_value = 0,
    .rate_result = 0, .start_result = 0};
  if (started) {
    start_desc = test_desc;
    start_desc.sampling_frequencies = start_rate;
    start_desc.sampling_frequency_count = 1;
    start_desc.initial_sampling_frequency = 0;
    (void)u2ps_dev_init(&start_desc);
    u2ps_fake_clear_log();
  }
  if (u2ps_dev_init(&test_desc) != U2PS_DEV_OK)
    printf("dev_start: u2ps_dev_init failed\n");
  started = true;
  memset(&app_log, 0, sizeof app_log);
  u2ps_dev_connect(true);
  u2ps_fake_bus_reset(speed);
}

/* ---- App callbacks ---------------------------------------------------------- */

app_log_t app_log;
app_cfg_t app_cfg;
int (*app_handler)(const u2ps_request_t *req);
void (*app_packet_hook)(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe);
void (*app_event_hook)(char event);
_Alignas(32) uint8_t app_mem[8 * 1024];

static void event(char c)
{
  if (app_log.nevents + 1u < sizeof app_log.events)
    app_log.events[app_log.nevents++] = c;
  if (app_event_hook != NULL)
    app_event_hook(c);
}

int u2ps_app_on_request(const u2ps_request_t *req)
{
  app_log.requests++;
  app_log.last = *req;
  if (req->len > 0u)
    memcpy(app_log.last_data, req->buf, req->len);
  return app_handler != NULL ? app_handler(req) : U2PS_REQ_STALL;
}

int u2ps_app_on_sampling_frequency(uint32_t requested, uint32_t chosen, uint32_t id)
{
  event('R');
  app_log.rates++;
  app_log.rate = chosen;
  app_log.rate_requested = requested;
  app_log.rate_id = id;
  return app_cfg.rate_result;
}

void u2ps_app_on_configured(bool configured, u2ps_config_end_t why)
{
  event(configured ? 'C' : 'D');
  app_log.config_end = why;
  if (configured) {
    app_log.configured++;
    app_log.speed_at_configured = u2ps_dev_speed();
  } else {
    app_log.unconfigured++;
  }
}

/* The class memory the app lends, as on_stream_start fills it in */
static void fill_cfg(u2ps_stream_cfg_t *cfg)
{
  cfg->mem = app_mem;
  cfg->stride = U2PS_DPORT_STRIDE(app_log.info.max_packet_size);
  cfg->count = app_cfg.count;
  cfg->fb_value = app_cfg.fb_value != 0u ? app_cfg.fb_value : app_log.fb_offered;
  app_log.cfg = *cfg;
}

u2ps_stream_start_t u2ps_app_on_stream_start(const u2ps_stream_info_t *info,
                                             u2ps_stream_cfg_t *cfg)
{
  event('S');
  app_log.starts++;
  app_log.state_at_start = u2ps_fake_stream_state;
  app_log.opens_at_start = u2ps_fake_count(U2PS_FAKE_STREAM_OPEN);
  app_log.info = *info;
  app_log.fb_offered = cfg->fb_value;
  if (app_cfg.start_result != 0)
    return (u2ps_stream_start_t)app_cfg.start_result;
  if (app_cfg.decline)
    return U2PS_STREAM_REFUSE;
  if (app_cfg.defer)
    return U2PS_STREAM_DEFER;
  fill_cfg(cfg);
  return U2PS_STREAM_OPEN;
}

u2ps_open_result_t app_open_deferred(void)
{
  u2ps_stream_cfg_t cfg;
  fill_cfg(&cfg);
  return u2ps_dev_stream_open(app_log.info.stream, &cfg);
}

void u2ps_app_on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  event('P');
  app_log.packets++;
  app_log.packet_data = data;
  app_log.packet_len = len;
  app_log.packet_uframe = uframe;
  if (len > 0u)
    app_log.packet_first = data[0];
  if (app_packet_hook != NULL)
    app_packet_hook(data, len, uframe);
  if (app_cfg.release_each)
    u2ps_dev_release(app_log.info.stream, 1);
}

void u2ps_app_on_hole(uint16_t uframes, u2ps_miss_t why)
{
  event('H');
  if (app_log.nholes < sizeof app_log.holes / sizeof app_log.holes[0]) {
    app_log.holes[app_log.nholes].uframes = uframes;
    app_log.holes[app_log.nholes].why = why;
  }
  app_log.nholes++;
  if ((unsigned)why < 4u)
    app_log.hole_total[why] += uframes;
  app_log.hole_masked = u2ps_fake_masked();
}

void u2ps_app_on_pause(uint16_t uframes)
{
  event('Z');
  if (app_log.npauses < sizeof app_log.pauses / sizeof app_log.pauses[0])
    app_log.pauses[app_log.npauses] = uframes;
  app_log.npauses++;
}

void u2ps_app_on_stream_stop(void)
{
  event('X');
  app_log.stops++;
}

void u2ps_app_on_fault(u2ps_fault_t fault)
{
  event('F');
  app_log.fault_masked = u2ps_fake_masked();
  if ((unsigned)fault < 4u)
    app_log.faults[fault]++;
}

void u2ps_app_on_reset(void)
{
  event('B');
  app_log.resets++;
}

#if U2PS_VBUS != U2PS_VBUS_BUS
void u2ps_app_on_vbus(bool present)
{
  event(present ? 'V' : 'v');
  app_log.vbus_masked = u2ps_fake_masked();
  app_log.calls_at_vbus = u2ps_fake_ncalls;
}
#endif

void u2ps_app_on_suspend(void)
{
  event('U');
  app_log.suspends++;
}

void u2ps_app_on_resume(void)
{
  event('W');
  app_log.resumes++;
}

/* ---- Short forms ------------------------------------------------------------- */

u2ps_fake_host_result_t get_descriptor(uint8_t type, uint8_t index, uint16_t windex, uint16_t wlength,
                             uint8_t *data, uint16_t *actual)
{
  return u2ps_fake_host_control(0x80, U2PS_GET_DESCRIPTOR, (uint16_t)(type << 8 | index), windex,
                      wlength, data, actual);
}

u2ps_fake_host_result_t set_address(uint8_t addr)
{
  return u2ps_fake_host_control(0x00, U2PS_SET_ADDRESS, addr, 0, 0, NULL, NULL);
}

u2ps_fake_host_result_t set_configuration(uint8_t value)
{
  return u2ps_fake_host_control(0x00, U2PS_SET_CONFIGURATION, value, 0, 0, NULL, NULL);
}

u2ps_fake_host_result_t set_interface(uint8_t itf, uint8_t alt)
{
  return u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, alt, itf, 0, NULL, NULL);
}

/* Clock Source entity 4 on interface 0, CS_SAM_FREQ_CONTROL, CN 0 (UAC2
 * 5.2.2, 5.2.5.1.1) */
u2ps_fake_host_result_t set_rate(uint32_t rate)
{
  uint8_t b[4] = {(uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16),
                  (uint8_t)(rate >> 24)};
  uint16_t n;
  return u2ps_fake_host_control(0x21, 0x01, 0x0100, 0x0400, 4, b, &n);
}

u2ps_fake_host_result_t get_rate(uint32_t *rate)
{
  uint8_t b[4] = {0};
  uint16_t n = 0;
  u2ps_fake_host_result_t r = u2ps_fake_host_control(0xA1, 0x01, 0x0100, 0x0400, 4, b, &n);
  *rate = n == 4 ? (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
                       (uint32_t)b[3] << 24
                 : 0xFFFFFFFFu;
  return r;
}

void dev_configure(void)
{
  if (set_address(5) != U2PS_FAKE_HOST_OK || set_configuration(1) != U2PS_FAKE_HOST_OK)
    printf("dev_configure: failed\n");
}
