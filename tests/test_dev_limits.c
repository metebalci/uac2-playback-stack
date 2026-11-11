/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_limits.c: the stream limits U2PS_ISO_OUT_MPS_MAX and
 * U2PS_ISO_OUT_COUNT_MAX, against a device library built with 388 and 4.
 * u2ps_dev_init() refuses a description whose largest wMaxPacketSize is
 * above the first; a stream lent more buffers than the second does not open.
 */
#include "check.h"
#include "dev_support.h"

static void test_mps_max(void)
{
  u2ps_dev_description_t d = test_desc;

  CHECK_EQ(U2PS_ISO_OUT_MPS_MAX, 388);
  /* The test description reaches 776 (24/4 at 768 kHz) */
  u2ps_fake_reset_all();
  CHECK_EQ(u2ps_dev_init(&d), U2PS_DEV_ERR_DESCRIPTION);
  CHECK_EQ(u2ps_dev_description_error(), U2PS_DESCRIPTION_MAX_PACKET_SIZE);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_INIT), 0);
  /* 16/2 alone: (96 + 1) * 2 * 2 = 388 */
  d.format_count = 1;
  u2ps_fake_reset_all();
  CHECK_EQ(u2ps_dev_init(&d), U2PS_DEV_OK);
  /* 24/3 at 768 kHz is 582; at 384 kHz (49 * 6 = 294) it fits */
  static const u2ps_format_t f243[1] = {{3, 24, U2PS_AUDIO_DATA_FORMAT_PCM}};
  d.formats = f243;
  u2ps_fake_reset_all();
  CHECK_EQ(u2ps_dev_init(&d), U2PS_DEV_ERR_DESCRIPTION);
  CHECK_EQ(u2ps_dev_description_error(), U2PS_DESCRIPTION_MAX_PACKET_SIZE);
  d.sampling_frequency_count = 8;
  u2ps_fake_reset_all();
  CHECK_EQ(u2ps_dev_init(&d), U2PS_DEV_OK);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static void test_count_max(void)
{
  CHECK_EQ(U2PS_ISO_OUT_COUNT_MAX, 4);
  test_desc.format_count = 1;
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  app_cfg.count = 5;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(app_log.faults[U2PS_FAULT_OPEN], 1);
  CHECK_EQ(app_log.stops, 1);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), 0);
  app_cfg.count = 4;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_OPEN);
  CHECK_EQ(u2ps_fake_stream.count, 4);
  CHECK_EQ(u2ps_fake_violations, 0);
  test_desc_load();
}

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
  test_mps_max();
  test_count_max();
  return check_done();
}
