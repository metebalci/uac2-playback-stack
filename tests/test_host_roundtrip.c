/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_host_roundtrip.c: the host stack enumerates the u2ps device stack.
 * Every device description that the device tests have the builder accept
 * goes through the builder, and the host stack, with its walk, must give
 * the description back: IDs, strings, terminal type, the clock's sampling
 * frequencies and its current one, and per alternate setting the format,
 * the channels, wMaxPacketSize, an asynchronous endpoint with explicit
 * feedback. At full speed the host stack finds the device's audio function
 * in its other-speed configuration and ends with SPEED.
 *
 * The fake host port's device passes each control transfer to the fake
 * device port's host side (u2ps_fake_host_control), so that the device stack itself
 * answers it.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "dev_support.h"
#include "host_support.h"

/* The fake host port's device: the device stack, through the fake device
 * port */
static u2ps_hfake_answer_t bridge(uint8_t addr, const u2ps_hfake_setup_t *s, uint8_t *data, uint16_t *len)
{
  (void)addr;
  uint16_t actual = 0;
  u2ps_fake_host_result_t r = u2ps_fake_host_control(s->bmRequestType, s->bRequest, s->wValue, s->wIndex,
                                 s->wLength, s->wLength > 0u ? data : NULL, &actual);
  if (r == U2PS_FAKE_HOST_OK) {
    *len = (s->bmRequestType & 0x80u) != 0u ? actual : s->wLength;
    return U2PS_HFAKE_ANSWER_OK;
  }
  return r == U2PS_FAKE_HOST_STALL ? U2PS_HFAKE_ANSWER_STALL : U2PS_HFAKE_ANSWER_ERROR;
}

static bool same_string(const char *got, const char *want)
{
  if (want == NULL)
    return got == NULL;
  return got != NULL && strcmp(got, want) == 0;
}

/* The device stack with description d at high speed; the host stack
 * enumerates it and must give d back */
static void round_trip(const u2ps_dev_description_t *d, const char *what)
{
  unsigned failures = (unsigned)check_failures;
  test_desc = *d;
  dev_start(U2PS_SPEED_HIGH);
  if (!hstart(NULL))
    return;
  u2ps_hfake_device = bridge;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK_EQ(happ_log.ends, 0);
  if (happ_log.devices != 1u) {
    printf("  %s: on_end %d\n", what, (int)happ_log.end);
    return;
  }
  const u2ps_host_device_t *h = &happ_log.dev;
  CHECK_EQ(h->id_vendor, d->id_vendor);
  CHECK_EQ(h->id_product, d->id_product);
  CHECK_EQ(h->bcd_device, d->bcd_device);
  CHECK(same_string(h->manufacturer, d->manufacturer));
  CHECK(same_string(h->product, d->product));
  CHECK(same_string(h->serial_number, d->serial_number));
  CHECK_EQ(h->terminal_type, d->terminal_type);
  CHECK(h->sampling_frequency_host_programmable);
  uint32_t initial = d->initial_sampling_frequency != 0u ? d->initial_sampling_frequency
                                                         : d->sampling_frequencies[0];
  CHECK_EQ(h->sampling_frequency, initial);
  unsigned rates = d->sampling_frequency_count;
  CHECK_EQ(h->num_sub_ranges, rates < U2PS_HOST_SUB_RANGE_MAX ? rates : U2PS_HOST_SUB_RANGE_MAX);
  for (unsigned i = 0; i < h->num_sub_ranges && i < rates; i++) {
    CHECK_EQ(h->sub_ranges[i].min, d->sampling_frequencies[i]);
    CHECK_EQ(h->sub_ranges[i].max, d->sampling_frequencies[i]);
    CHECK_EQ(h->sub_ranges[i].res, 0);
  }
  CHECK_EQ(h->alternate_setting_count, d->format_count);
  for (unsigned i = 0; i < h->alternate_setting_count && i < d->format_count; i++) {
    const u2ps_host_alternate_setting_t *a = &h->alternate_settings[i];
    CHECK_EQ(a->alternate_setting, i + 1u);
    CHECK_EQ(a->format.subslot_size, d->formats[i].subslot_size);
    CHECK_EQ(a->format.bit_resolution, d->formats[i].bit_resolution);
    CHECK_EQ(a->format.audio_data_format, d->formats[i].audio_data_format);
    CHECK_EQ(a->nr_channels, d->nr_channels);
    CHECK_EQ(a->channel_config, d->channel_config);
    uint32_t top = d->sampling_frequencies[d->sampling_frequency_count - 1u];
    CHECK_EQ(a->max_packet_size,
             (top / 8000u + (top % 8000u != 0u) + 1u) * d->nr_channels * d->formats[i].subslot_size);
    CHECK_EQ(a->synchronization_type, U2PS_SYNCHRONIZATION_TYPE_ASYNCHRONOUS);
    CHECK(a->explicit_feedback);
    CHECK_EQ(a->skip, U2PS_HOST_SKIP_NONE);
  }
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
  CHECK_EQ(app_log.configured, 1);
  if ((unsigned)check_failures != failures)
    printf("  in round trip: %s\n", what);
}

/* The device stack at full speed: its configuration there holds no
 * AudioStreaming interface; the host stack reads the device_qualifier and
 * the other-speed configuration (USB 2.0 9.6.2, 9.6.4), and ends with
 * SPEED */
static void full_speed(void)
{
  dev_start(U2PS_SPEED_FULL);
  if (!hstart(NULL))
    return;
  u2ps_hfake_device = bridge;
  u2ps_hfake_speed = U2PS_SPEED_FULL;
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 0);
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, U2PS_HOST_END_SPEED);
  unsigned n = u2ps_hfake_count(U2PS_HFAKE_CONTROL);
  CHECK_EQ(n, 8);
  /* Device (8 bytes), SET_ADDRESS(1), device, configuration (head,
   * whole), device_qualifier, other-speed configuration (head, whole) */
  static const uint16_t w_value[8] = {0x0100, 1, 0x0100, 0x0200, 0x0200, 0x0600, 0x0700, 0x0700};
  for (unsigned k = 0; k < n && k < 8u; k++)
    CHECK_EQ(u2ps_hfake_control_nth(k)->setup.wValue, w_value[k]);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
  const u2ps_dev_description_t base = test_desc;
  u2ps_dev_description_t d;

  /* The test description, and the formats of the fixtures */
  round_trip(&base, "test description");
  static const u2ps_format_t f162_324[2] = {{2, 16, U2PS_AUDIO_DATA_FORMAT_PCM},
                                           {4, 32, U2PS_AUDIO_DATA_FORMAT_PCM}};
  static const u2ps_format_t f243[1] = {{3, 24, U2PS_AUDIO_DATA_FORMAT_PCM}};
  static const u2ps_format_t f243_162[2] = {{3, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
                                           {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}};
  d = base;
  d.formats = f162_324;
  d.format_count = 2;
  round_trip(&d, "16/2, 32/4");
  d.formats = f243;
  d.format_count = 1;
  round_trip(&d, "24/3");
  d.formats = f243_162;
  d.format_count = 2;
  round_trip(&d, "24/3, 16/2");

  /* IDs and strings */
  d = base;
  d.id_vendor = 0xA1B2;
  d.id_product = 0xC3D4;
  d.bcd_device = 0x0217;
  round_trip(&d, "IDs");
  d = base;
  d.manufacturer = NULL;
  d.serial_number = NULL;
  d.product = "u2ps";
  round_trip(&d, "product string only");
  d.product = NULL;
  round_trip(&d, "no strings");
  static char longest[127];
  memset(longest, 'x', 126);
  longest[126] = '\0';
  d.serial_number = longest;
  round_trip(&d, "the longest string");
  d = base;
  d.product = "";
  round_trip(&d, "an empty product string");

  /* Power, category, terminal type, channels, rates */
  d = base;
  d.self_powered = true;
  d.max_power_ma = 500;
  round_trip(&d, "self-powered, 500 mA");
  d = base;
  d.max_power_ma = 99;
  round_trip(&d, "99 mA");
  d = base;
  d.category = 0x0A;
  d.terminal_type = 0x0602;
  d.nr_channels = 8;
  d.channel_config = 0x0000063F;
  d.sampling_frequency_count = 6;
  round_trip(&d, "8 channels up to 192 kHz");
  static const uint32_t r44[1] = {44100};
  d = base;
  d.sampling_frequencies = r44;
  d.sampling_frequency_count = 1;
  round_trip(&d, "44.1 kHz only");
  for (unsigned i = 0; i < TEST_RATE_COUNT; i++) {
    d = base;
    d.initial_sampling_frequency = test_rates[i];
    round_trip(&d, "initial sampling frequency");
  }

  /* The limits the builder accepts: eight formats, 42 rates, 1024-byte
   * packets */
  static const u2ps_format_t eight[8] = {
    {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}, {4, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
    {4, 32, U2PS_AUDIO_DATA_FORMAT_PCM}, {3, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
    {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}, {4, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
    {4, 32, U2PS_AUDIO_DATA_FORMAT_PCM}, {3, 24, U2PS_AUDIO_DATA_FORMAT_PCM}};
  d = base;
  d.formats = eight;
  d.format_count = 8;
  round_trip(&d, "eight formats");
  static uint32_t many[42];
  for (unsigned i = 0; i < 42u; i++)
    many[i] = 8000u * (i + 1u);
  d = base;
  d.sampling_frequencies = many;
  d.sampling_frequency_count = 42;
  d.nr_channels = 1;
  d.channel_config = 0x1;
  round_trip(&d, "42 rates");
  static const u2ps_format_t f32[1] = {{4, 32, U2PS_AUDIO_DATA_FORMAT_PCM}};
  static const uint32_t edge[1] = {1016000};
  d = base;
  d.formats = f32;
  d.format_count = 1;
  d.sampling_frequencies = edge;
  d.sampling_frequency_count = 1;
  round_trip(&d, "1024-byte packets");
  static const u2ps_format_t f162[1] = {{2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}};
  d = base;
  d.formats = f162;
  d.format_count = 1;
  round_trip(&d, "16/2 alone");
  d.formats = f243;
  d.sampling_frequency_count = 8;
  round_trip(&d, "24/3 up to 384 kHz");

  /* Clusters and terminal types the builder accepts */
  static const struct {
    uint8_t nr_channels;
    uint32_t channel_config;
  } clusters[] = {{2, 0x3}, {2, 0x0}, {8, 0x0000063F}, {5, 0x51}, {2, 0x04000001}};
  for (unsigned i = 0; i < sizeof clusters / sizeof clusters[0]; i++) {
    d = base;
    d.nr_channels = clusters[i].nr_channels;
    d.channel_config = clusters[i].channel_config;
    d.format_count = 1;
    d.sampling_frequency_count = 6;
    round_trip(&d, "cluster");
  }
  static const uint16_t terminals[] = {0x0301, 0x0302, 0x0400, 0x0602, 0x0700, 0x0701,
                                       0x0702, 0x0704, 0x0707, 0x0711, 0x0712, 0x0717};
  for (unsigned i = 0; i < sizeof terminals / sizeof terminals[0]; i++) {
    d = base;
    d.terminal_type = terminals[i];
    round_trip(&d, "terminal type");
  }

  /* IEEE_FLOAT */
  static const u2ps_format_t pcm_float[2] = {{2, 16, U2PS_AUDIO_DATA_FORMAT_PCM},
                                             {4, 32, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}};
  static const u2ps_format_t float_only[1] = {{4, 32, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}};
  static const u2ps_format_t mixed[3] = {{4, 32, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT},
                                         {4, 32, U2PS_AUDIO_DATA_FORMAT_PCM},
                                         {4, 32, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}};
  d = base;
  d.formats = pcm_float;
  d.format_count = 2;
  round_trip(&d, "PCM and IEEE_FLOAT");
  d.formats = float_only;
  d.format_count = 1;
  round_trip(&d, "IEEE_FLOAT only");
  d.formats = mixed;
  d.format_count = 3;
  round_trip(&d, "IEEE_FLOAT, PCM, IEEE_FLOAT");
  full_speed();
  return check_done();
}
