/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_speed.c: the device at both speeds (USB 2.0 9.2.6.6, 9.6.2,
 * 9.6.4, 7.1.20). Built twice: against the default device library, which
 * enumerates at full speed with the minimal FS configuration and describes
 * the other speed, and against the one built with U2PS_REFUSE_FS=1, which
 * never enumerates at full speed and has no other-speed descriptors. The
 * descriptors are the builder's, from the test description; test_dev_desc
 * checks the builder itself.
 */
#include <string.h>

#include "check.h"
#include "dev_support.h"

static uint8_t buf[600];
static uint16_t n;

#if !U2PS_REFUSE_FS
/* The device_qualifier both speeds serve: the device descriptor's bcdUSB,
 * class, subclass and protocol (EF/02/01, UAC2 4.3), EP0's 64 bytes, one
 * configuration */
static void check_qualifier(void)
{
  const uint8_t want[10] = {10, U2PS_DESC_DEVICE_QUALIFIER, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 1, 0};
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE_QUALIFIER, 0, 0, 10, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 10);
  CHECK(memcmp(buf, want, 10) == 0);
  /* Linux asks for 10 bytes at full speed (recorded on the bus); a read
   * that asks for more gets 10 too */
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE_QUALIFIER, 0, 0, 64, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 10);
}
#endif

/* GET_DESCRIPTOR(type) returns cfg with byte 1 replaced by 'type' */
static void check_config(uint8_t type, const uint8_t *cfg, uint16_t len)
{
  memset(buf, 0, sizeof buf);
  CHECK_EQ(get_descriptor(type, 0, 0, 0xFFFF, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, len);
  CHECK_EQ(buf[0], cfg[0]);
  CHECK_EQ(buf[1], type);
  CHECK(memcmp(buf + 2, cfg + 2, len - 2u) == 0);
  /* The 9-byte header first, as hosts read it */
  CHECK_EQ(get_descriptor(type, 0, 0, 9, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 9);
  CHECK_EQ(buf[1], type);
  CHECK_EQ(buf[2] | buf[3] << 8, len);
}

static void check_test_mode_refused(void)
{
  u2ps_fake_clear_log();
  for (uint8_t sel = 1; sel <= 4; sel++)
    CHECK_EQ(u2ps_fake_host_control(0x00, U2PS_SET_FEATURE, U2PS_FEATURE_TEST_MODE,
                          (uint16_t)(sel << 8), 0, NULL, NULL),
             U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_TEST_MODE), 0);
}

static void test_high_speed(void)
{
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 18, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK(memcmp(buf, test_device, 18) == 0);
  check_config(U2PS_DESC_CONFIGURATION, test_config_hs, test_config_hs_len);
#if U2PS_REFUSE_FS
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE_QUALIFIER, 0, 0, 10, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_OTHER_SPEED, 0, 0, 0xFFFF, buf, &n), U2PS_FAKE_HOST_STALL);
#else
  check_qualifier();
  check_config(U2PS_DESC_OTHER_SPEED, test_config_fs, test_config_fs_len);
#endif
  /* Configured at high speed: the streaming interface is there */
  dev_configure();
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, 1, 1, 0, NULL, NULL), U2PS_FAKE_HOST_OK);
  /* Test mode is accepted (test_dev_std covers it) */
  u2ps_fake_clear_log();
  CHECK_EQ(u2ps_fake_host_control(0x00, U2PS_SET_FEATURE, U2PS_FEATURE_TEST_MODE, 0x0400, 0, NULL, NULL),
           U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_test_selector, 4);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static void test_full_speed(void)
{
  dev_start(U2PS_SPEED_FULL);
#if U2PS_REFUSE_FS
  /* Every descriptor is refused, so the device never enumerates */
  static const uint16_t lens[] = {8, 18, 64};
  for (unsigned i = 0; i < 3; i++)
    CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, lens[i], buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_CONFIGURATION, 0, 0, 9, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 0, 0, 255, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE_QUALIFIER, 0, 0, 10, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_OTHER_SPEED, 0, 0, 9, buf, &n), U2PS_FAKE_HOST_STALL);
  /* SET_ADDRESS still works; there is no configuration to set */
  CHECK_EQ(set_address(5), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_GET_STATUS, 0, 0, 2, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[0] | buf[1] << 8, 0);
#else
  /* The same device descriptor; the minimal FS configuration; the other
   * speed is the high-speed configuration */
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 64, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 18);
  CHECK(memcmp(buf, test_device, 18) == 0);
  check_config(U2PS_DESC_CONFIGURATION, test_config_fs, test_config_fs_len);
  check_qualifier();
  check_config(U2PS_DESC_OTHER_SPEED, test_config_hs, test_config_hs_len);
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 2, TEST_LANGID, 255, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 64);
  /* It configures, with the AudioControl interface only */
  dev_configure();
  CHECK_EQ(u2ps_fake_host_control(0x80, U2PS_GET_CONFIGURATION, 0, 0, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[0], 1);
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_INTERFACE, 0, 0, 1, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[0], 0);
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_INTERFACE, 0, 1, 1, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x01, U2PS_SET_INTERFACE, 1, 1, 0, NULL, NULL), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(u2ps_fake_host_control(0x82, U2PS_GET_STATUS, 0, 0x01, 2, buf, &n), U2PS_FAKE_HOST_STALL);
#endif
  /* Test mode is a high-speed request only (7.1.20) */
  check_test_mode_refused();
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* The speed follows each reset: FS, then HS, then FS again */
static void test_speed_change(void)
{
  dev_start(U2PS_SPEED_FULL);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  check_config(U2PS_DESC_CONFIGURATION, test_config_hs, test_config_hs_len);
  u2ps_fake_bus_reset(U2PS_SPEED_FULL);
#if U2PS_REFUSE_FS
  CHECK_EQ(get_descriptor(U2PS_DESC_CONFIGURATION, 0, 0, 9, buf, &n), U2PS_FAKE_HOST_STALL);
#else
  check_config(U2PS_DESC_CONFIGURATION, test_config_fs, test_config_fs_len);
#endif
  check_test_mode_refused();
  CHECK_EQ(u2ps_fake_violations, 0);
}

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
  test_high_speed();
  test_full_speed();
  test_speed_change();
  return check_done();
}
