/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_desc.c: the descriptor builder. From a description it must give
 * each of the four high-speed fixtures in tests/fixtures byte for byte, the
 * minimal FS configuration uac2-fs-minimal.txt, both as other_speed
 * configurations too (USB 2.0 9.6.4), and the device, device_qualifier and
 * string descriptors (9.6.1, 9.6.2, 9.6.7; UAC2 4.2, 4.3). Each field of the
 * description reaches its bytes, and u2ps_dev_init() refuses a
 * description out of range. An IEEE_FLOAT alternate setting gives
 * uac2-hs-16-2_float32-4.txt.
 */
#include <string.h>

#include "check.h"
#include "dev_support.h"
#include "u2ps_uac2.h"

static uint8_t buf[600];
static uint16_t n;

/* got == want, or print where they first differ */
static void check_bytes(const uint8_t *got, unsigned got_len, const uint8_t *want,
                        unsigned want_len, const char *what)
{
  CHECK_EQ(got_len, want_len);
  unsigned len = got_len < want_len ? got_len : want_len;
  for (unsigned i = 0; i < len; i++) {
    if (got[i] != want[i]) {
      printf("  %s: byte %u is %02x, want %02x\n", what, i, got[i], want[i]);
      CHECK(got[i] == want[i]);
      return;
    }
  }
}

/* GET_DESCRIPTOR(type) at the current speed, all of it */
static void check_get(uint8_t type, const uint8_t *want, uint16_t len, const char *what)
{
  memset(buf, 0, sizeof buf);
  CHECK_EQ(get_descriptor(type, 0, 0, 0xFFFF, buf, &n), U2PS_FAKE_HOST_OK);
  check_bytes(buf, n, want, len, what);
}

/* The four high-speed fixtures and the formats of their alternate settings */
static const struct {
  const char *file;
  u2ps_format_t formats[4];
  uint8_t count;
} fixtures[] = {
  {"uac2-hs-default.txt",
   {{2, 16, U2PS_AUDIO_DATA_FORMAT_PCM},
    {4, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
    {4, 32, U2PS_AUDIO_DATA_FORMAT_PCM},
    {3, 24, U2PS_AUDIO_DATA_FORMAT_PCM}},
   4},
  {"uac2-hs-16-2_32-4.txt",
   {{2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}, {4, 32, U2PS_AUDIO_DATA_FORMAT_PCM}},
   2},
  {"uac2-hs-24-3.txt", {{3, 24, U2PS_AUDIO_DATA_FORMAT_PCM}}, 1},
  {"uac2-hs-24-3_16-2.txt",
   {{3, 24, U2PS_AUDIO_DATA_FORMAT_PCM}, {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}},
   2},
};

static void test_fixtures(void)
{
  uint8_t hs[512], fs[512], other[512];
  int fs_len = fixture_load("uac2-fs-minimal.txt", fs, sizeof fs);
  CHECK(fs_len > 0);

  for (unsigned f = 0; f < sizeof fixtures / sizeof fixtures[0]; f++) {
    int hs_len = fixture_load(fixtures[f].file, hs, sizeof hs);
    CHECK(hs_len > 0);
    if (hs_len <= 0 || fs_len <= 0)
      continue;
    printf("%s\n", fixtures[f].file);
    test_desc.formats = fixtures[f].formats;
    test_desc.format_count = fixtures[f].count;

    /* High speed: the fixture; other speed: the minimal FS configuration */
    dev_start(U2PS_SPEED_HIGH);
    check_get(U2PS_DESC_CONFIGURATION, hs, (uint16_t)hs_len, "HS configuration");
    memcpy(other, fs, (size_t)fs_len);
    other[1] = U2PS_DESC_OTHER_SPEED;
    check_get(U2PS_DESC_OTHER_SPEED, other, (uint16_t)fs_len, "FS as other speed");

    /* Full speed: the minimal FS configuration; other speed: the fixture */
    u2ps_fake_bus_reset(U2PS_SPEED_FULL);
    check_get(U2PS_DESC_CONFIGURATION, fs, (uint16_t)fs_len, "FS configuration");
    memcpy(other, hs, (size_t)hs_len);
    other[1] = U2PS_DESC_OTHER_SPEED;
    check_get(U2PS_DESC_OTHER_SPEED, other, (uint16_t)hs_len, "HS as other speed");
    CHECK_EQ(u2ps_fake_violations, 0);
  }
  test_desc_load();
}

/* The device descriptor and device_qualifier; strings in UTF-16LE */
static void test_device_and_strings(void)
{
  dev_start(U2PS_SPEED_HIGH);
  check_get(U2PS_DESC_DEVICE, test_device, 18, "device");
  const uint8_t qualifier[10] = {10, 6, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 1, 0};
  check_get(U2PS_DESC_DEVICE_QUALIFIER, qualifier, 10, "device_qualifier");
  for (uint8_t i = 0; i < 4; i++) {
    memset(buf, 0, sizeof buf);
    CHECK_EQ(get_descriptor(U2PS_DESC_STRING, i, i ? TEST_LANGID : 0, 255, buf, &n), U2PS_FAKE_HOST_OK);
    check_bytes(buf, n, test_strings[i], test_strings[i][0], "string");
  }

  /* IDs and bcdDevice */
  test_desc.id_vendor = 0xA1B2;
  test_desc.id_product = 0xC3D4;
  test_desc.bcd_device = 0x0217;
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 18, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[8] | buf[9] << 8, 0xA1B2);
  CHECK_EQ(buf[10] | buf[11] << 8, 0xC3D4);
  CHECK_EQ(buf[12] | buf[13] << 8, 0x0217);

  /* Only a product string: the other indices are 0 and stall */
  test_desc_load();
  test_desc.manufacturer = NULL;
  test_desc.serial_number = NULL;
  test_desc.product = "u2ps";
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 18, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[14], 0);
  CHECK_EQ(buf[15], 2);
  CHECK_EQ(buf[16], 0);
  const uint8_t product[10] = {10, 3, 'u', 0, '2', 0, 'p', 0, 's', 0};
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 2, TEST_LANGID, 255, buf, &n), U2PS_FAKE_HOST_OK);
  check_bytes(buf, n, product, sizeof product, "product");
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 1, TEST_LANGID, 255, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 3, TEST_LANGID, 255, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 0, 0, 255, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 4);

  /* No strings at all: every index 0, and no string descriptor, not even
   * the LANGIDs (USB 2.0 9.6.7) */
  test_desc.product = NULL;
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(get_descriptor(U2PS_DESC_DEVICE, 0, 0, 18, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[14] | buf[15] | buf[16], 0);
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 0, 0, 255, buf, &n), U2PS_FAKE_HOST_STALL);
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 2, TEST_LANGID, 255, buf, &n), U2PS_FAKE_HOST_STALL);

  /* The longest string: 126 characters, 254 bytes */
  static char longest[127];
  memset(longest, 'x', 126);
  longest[126] = '\0';
  test_desc.serial_number = longest;
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(get_descriptor(U2PS_DESC_STRING, 3, TEST_LANGID, 255, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 254);
  CHECK_EQ(buf[0], 254);
  CHECK_EQ(buf[253], 0);
  CHECK_EQ(buf[252], 'x');
  CHECK_EQ(u2ps_fake_violations, 0);
  test_desc_load();
}

/* Offset of the first descriptor of 'type' (and subtype, if not 0) in the
 * configuration in buf, at or after 'from'; -1 if none */
static int find_desc(uint8_t type, uint8_t subtype, int from)
{
  uint16_t total = (uint16_t)(buf[2] | buf[3] << 8);
  for (int off = 0; off < total && buf[off] != 0u; off += buf[off])
    if (off >= from && buf[off + 1] == type && (subtype == 0u || buf[off + 2] == subtype))
      return off;
  return -1;
}

/* Each field of the description where the fixtures hold it fixed */
static void test_fields(void)
{
  /* Self-powered and the power: in both configurations */
  test_desc.self_powered = true;
  test_desc.max_power_ma = 500;
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(get_descriptor(U2PS_DESC_CONFIGURATION, 0, 0, 9, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[7], 0xC0);
  CHECK_EQ(buf[8], 250);
  CHECK_EQ(get_descriptor(U2PS_DESC_OTHER_SPEED, 0, 0, 9, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[7], 0xC0);
  CHECK_EQ(buf[8], 250);
  test_desc.self_powered = false;
  test_desc.max_power_ma = 99; /* rounded up to 2 mA units */
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(get_descriptor(U2PS_DESC_CONFIGURATION, 0, 0, 9, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(buf[7], 0x80);
  CHECK_EQ(buf[8], 50);
  /* Each odd value rounds up to the next 2 mA, each even one is exact */
  static const uint16_t ma[] = {0, 1, 2, 3, 100, 101, 499};
  static const uint8_t units[] = {0, 1, 1, 2, 50, 51, 250};
  for (unsigned i = 0; i < sizeof ma / sizeof ma[0]; i++) {
    test_desc.max_power_ma = ma[i];
    dev_start(U2PS_SPEED_HIGH);
    CHECK_EQ(get_descriptor(U2PS_DESC_CONFIGURATION, 0, 0, 9, buf, &n), U2PS_FAKE_HOST_OK);
    CHECK_EQ(buf[8], units[i]);
    CHECK_EQ(get_descriptor(U2PS_DESC_OTHER_SPEED, 0, 0, 9, buf, &n), U2PS_FAKE_HOST_OK);
    CHECK_EQ(buf[8], units[i]);
  }

  /* Category, terminal type, channels, channel configuration; the lock
   * delay is always 0 units and 0 delay (UAC2 4.10.1.2) */
  test_desc_load();
  test_desc.category = 0x0A;
  test_desc.terminal_type = 0x0602;
  test_desc.nr_channels = 8;
  test_desc.channel_config = 0x0000063F;
  test_desc.sampling_frequencies = test_rates;
  test_desc.sampling_frequency_count = 6; /* up to 192 kHz: 24 samples, + 1 */
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(get_descriptor(U2PS_DESC_CONFIGURATION, 0, 0, 0xFFFF, buf, &n), U2PS_FAKE_HOST_OK);
  int hdr = find_desc(U2PS_CS_INTERFACE, 0x01, 0);
  CHECK(hdr > 0);
  if (hdr > 0)
    CHECK_EQ(buf[hdr + 5], 0x0A);
  int it = find_desc(U2PS_CS_INTERFACE, 0x02, 0);
  CHECK(it > 0);
  if (it > 0) {
    CHECK_EQ(buf[it + 8], 8);
    CHECK_EQ(buf[it + 9] | buf[it + 10] << 8, 0x063F);
  }
  int ot = find_desc(U2PS_CS_INTERFACE, 0x03, 0);
  CHECK(ot > 0);
  if (ot > 0)
    CHECK_EQ(buf[ot + 4] | buf[ot + 5] << 8, 0x0602);
  int as = find_desc(U2PS_CS_INTERFACE, 0x01, it);
  CHECK(as > 0);
  if (as > 0) {
    CHECK_EQ(buf[as], 16);
    CHECK_EQ(buf[as + 10], 8);
    CHECK_EQ(buf[as + 11] | buf[as + 12] << 8, 0x063F);
  }
  /* Data endpoints: (24 + 1) * 8 channels * 2, 4, 4, 3 bytes */
  static const uint16_t mps[4] = {400, 800, 800, 600};
  int ep = 0;
  for (unsigned a = 0; a < 4; a++) {
    ep = find_desc(U2PS_DESC_ENDPOINT, 0, ep);
    CHECK(ep > 0);
    if (ep <= 0)
      break;
    CHECK_EQ(buf[ep + 2], 0x01);
    CHECK_EQ(buf[ep + 4] | buf[ep + 5] << 8, mps[a]);
    int cs = find_desc(U2PS_CS_ENDPOINT, 0, ep);
    CHECK_EQ(cs, ep + 7);
    if (cs > 0) {
      CHECK_EQ(buf[cs + 5], 0);
      CHECK_EQ(buf[cs + 6] | buf[cs + 7] << 8, 0);
    }
    ep = find_desc(U2PS_DESC_ENDPOINT, 0, ep + 1); /* its feedback endpoint */
    CHECK(ep > 0);
    if (ep <= 0)
      break;
    CHECK_EQ(buf[ep + 2], 0x81);
    ep++;
  }
  /* A rate of 44.1 kHz at most: 6 samples, rounded up, + 1 */
  static const uint32_t r44[1] = {44100};
  test_desc_load();
  test_desc.sampling_frequencies = r44;
  test_desc.sampling_frequency_count = 1;
  dev_start(U2PS_SPEED_HIGH);
  CHECK_EQ(get_descriptor(U2PS_DESC_CONFIGURATION, 0, 0, 0xFFFF, buf, &n), U2PS_FAKE_HOST_OK);
  ep = find_desc(U2PS_DESC_ENDPOINT, 0, 0);
  CHECK(ep > 0);
  if (ep > 0)
    CHECK_EQ(buf[ep + 4] | buf[ep + 5] << 8, 7 * 2 * 2);
  CHECK_EQ(u2ps_fake_violations, 0);
  test_desc_load();
}

/* u2ps_dev_init(d) accepts d if 'want' is U2PS_DESCRIPTION_OK, and
 * otherwise refuses it with U2PS_DEV_ERR_DESCRIPTION, and
 * u2ps_dev_description_error() names rule 'want' */
static bool init_with(const u2ps_dev_description_t *d, u2ps_dev_description_error_t want)
{
  u2ps_fake_reset_all();
  u2ps_dev_err_t e = u2ps_dev_init(d);
  bool ok = e == U2PS_DEV_OK;
  /* The port starts, or restarts, only with a valid description */
  CHECK_EQ(e, want == U2PS_DESCRIPTION_OK ? U2PS_DEV_OK : U2PS_DEV_ERR_DESCRIPTION);
  CHECK_EQ(u2ps_dev_description_error(), want);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_INIT) + u2ps_fake_count(U2PS_FAKE_REINIT), ok ? 1 : 0);
  return ok;
}

/* u2ps_dev_init() refuses a description out of range, naming the rule,
 * and accepts the limits */
static void test_validation(void)
{
  u2ps_dev_description_t d;
  static const struct {
    u2ps_format_t format[1];
    u2ps_dev_description_error_t rule;
  } bad_formats[] = {
    {{{0, 8, U2PS_AUDIO_DATA_FORMAT_PCM}}, U2PS_DESCRIPTION_SUBSLOT_SIZE},
    {{{5, 32, U2PS_AUDIO_DATA_FORMAT_PCM}}, U2PS_DESCRIPTION_SUBSLOT_SIZE},
    {{{2, 0, U2PS_AUDIO_DATA_FORMAT_PCM}}, U2PS_DESCRIPTION_BIT_RESOLUTION},
    {{{2, 17, U2PS_AUDIO_DATA_FORMAT_PCM}}, U2PS_DESCRIPTION_BIT_RESOLUTION},
    {{{1, 9, U2PS_AUDIO_DATA_FORMAT_PCM}}, U2PS_DESCRIPTION_BIT_RESOLUTION},
    {{{3, 25, U2PS_AUDIO_DATA_FORMAT_PCM}}, U2PS_DESCRIPTION_BIT_RESOLUTION}};
  static const u2ps_format_t eight[9] = {
    {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}, {4, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
    {4, 32, U2PS_AUDIO_DATA_FORMAT_PCM}, {3, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
    {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}, {4, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
    {4, 32, U2PS_AUDIO_DATA_FORMAT_PCM}, {3, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
    {1, 8, U2PS_AUDIO_DATA_FORMAT_PCM}};
  static const uint32_t unsorted[3] = {48000, 44100, 96000};
  static const uint32_t twice[2] = {48000, 48000};
  static const uint32_t zero[2] = {0, 48000};
  static const uint32_t edge_ok[1] = {1016000};  /* (127 + 1) * 2 * 4 = 1024 */
  static const uint32_t edge_bad[1] = {1016001}; /* 129 * 8 = 1032 */
  static uint32_t many[43];
  for (unsigned i = 0; i < 43; i++)
    many[i] = 8000u * (i + 1u);

  CHECK(init_with(&test_desc, U2PS_DESCRIPTION_OK));
  CHECK(!init_with(NULL, U2PS_DESCRIPTION_NULL));
  /* initial_sampling_frequency: 0 or one of the rates */
  static const uint32_t not_listed[] = {1, 44099, 50000, 768001, 0xFFFFFFFFu};
  for (unsigned i = 0; i < sizeof not_listed / sizeof not_listed[0]; i++) {
    d = test_desc;
    d.initial_sampling_frequency = not_listed[i];
    CHECK(!init_with(&d, U2PS_DESCRIPTION_INITIAL_SAMPLING_FREQUENCY));
  }
  for (unsigned i = 0; i < TEST_RATE_COUNT; i++) {
    d = test_desc;
    d.initial_sampling_frequency = test_rates[i];
    CHECK(init_with(&d, U2PS_DESCRIPTION_OK));
  }

  d = test_desc;
  d.formats = NULL;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_FORMATS));
  d = test_desc;
  d.format_count = 0;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_FORMAT_COUNT));
  for (unsigned i = 0; i < sizeof bad_formats / sizeof bad_formats[0]; i++) {
    d = test_desc;
    d.formats = bad_formats[i].format;
    d.format_count = 1;
    CHECK(!init_with(&d, bad_formats[i].rule));
  }
  /* 8 formats fill 505 bytes of the 512-byte control buffer; 9 do not fit */
  d = test_desc;
  d.formats = eight;
  d.format_count = 8;
  CHECK(init_with(&d, U2PS_DESCRIPTION_OK));
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EQ(get_descriptor(U2PS_DESC_CONFIGURATION, 0, 0, 0xFFFF, buf, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 505);
  d.format_count = 9;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_CONFIGURATION_SIZE));

  d = test_desc;
  d.nr_channels = 0;
  d.channel_config = 0;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_NR_CHANNELS));
  d = test_desc;
  d.sampling_frequencies = NULL;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_SAMPLING_FREQUENCIES));
  d = test_desc;
  d.sampling_frequency_count = 0;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_SAMPLING_FREQUENCY_COUNT));
  d.sampling_frequencies = unsorted;
  d.sampling_frequency_count = 3;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_SAMPLING_FREQUENCY_ORDER));
  d.sampling_frequencies = twice;
  d.sampling_frequency_count = 2;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_SAMPLING_FREQUENCY_ORDER));
  d.sampling_frequencies = zero;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_SAMPLING_FREQUENCY_ORDER));
  /* RANGE of 42 rates is 506 bytes; 43 do not fit the control buffer */
  d.sampling_frequencies = many;
  d.sampling_frequency_count = 42;
  d.nr_channels = 1;
  d.channel_config = 0x1;
  CHECK(init_with(&d, U2PS_DESCRIPTION_OK));
  d.sampling_frequency_count = 43;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_SAMPLING_FREQUENCY_COUNT));
  /* wMaxPacketSize at most 1024 (USB 2.0 5.6.3) */
  static const u2ps_format_t f32[1] = {{4, 32, U2PS_AUDIO_DATA_FORMAT_PCM}};
  d = test_desc;
  d.formats = f32;
  d.format_count = 1;
  d.sampling_frequencies = edge_ok;
  d.sampling_frequency_count = 1;
  CHECK(init_with(&d, U2PS_DESCRIPTION_OK));
  d.sampling_frequencies = edge_bad;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_MAX_PACKET_SIZE));
  d = test_desc;
  d.nr_channels = 4; /* 97 * 4 * 4 = 1552 at 768 kHz */
  CHECK(!init_with(&d, U2PS_DESCRIPTION_MAX_PACKET_SIZE));

  /* Power and strings */
  d = test_desc;
  d.max_power_ma = 500;
  CHECK(init_with(&d, U2PS_DESCRIPTION_OK));
  d.max_power_ma = 501;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_MAX_POWER_MA));
  static char too_long[128];
  memset(too_long, 'y', 127);
  too_long[127] = '\0';
  d = test_desc;
  d.product = too_long;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_PRODUCT));
  d.product = "tab\there";
  CHECK(!init_with(&d, U2PS_DESCRIPTION_PRODUCT));
  d.product = "caf\xc3\xa9";
  CHECK(!init_with(&d, U2PS_DESCRIPTION_PRODUCT));
  d.product = "";
  CHECK(init_with(&d, U2PS_DESCRIPTION_OK));
  too_long[126] = '\0'; /* 126 characters fit */
  d.manufacturer = too_long;
  CHECK(init_with(&d, U2PS_DESCRIPTION_OK));
  too_long[126] = 'y';
  CHECK(!init_with(&d, U2PS_DESCRIPTION_MANUFACTURER));
  d.manufacturer = "del\x7f";
  CHECK(!init_with(&d, U2PS_DESCRIPTION_MANUFACTURER));
  d.manufacturer = NULL;
  d.serial_number = too_long;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_SERIAL_NUMBER));
  d.serial_number = "SN\n1";
  CHECK(!init_with(&d, U2PS_DESCRIPTION_SERIAL_NUMBER));

  /* The cluster (UAC2 4.1, p. 43): no more spatial locations than channels,
   * no reserved bits D27..D30, no raw data D31 (it needs 0 channels) */
  static const struct {
    uint8_t channels;
    uint32_t config;
    bool ok;
  } clusters[] = {{2, 0x3, true},         {2, 0x0, true},         {1, 0x3, false},
                  {2, 0x7, false},        {8, 0x0000063F, true},  {5, 0x51, true},
                  {6, 0x0000063F, false}, {2, 0x08000001, false}, {2, 0x40000001, false},
                  {2, 0x80000000u, false}, {2, 0x04000001, true}};
  for (unsigned i = 0; i < sizeof clusters / sizeof clusters[0]; i++) {
    d = test_desc;
    d.nr_channels = clusters[i].channels;
    d.channel_config = clusters[i].config;
    d.format_count = 1; /* 16/2 */
    d.sampling_frequency_count = 6;   /* up to 192 kHz, so 8 channels fit */
    bool ok = init_with(&d, clusters[i].ok ? U2PS_DESCRIPTION_OK : U2PS_DESCRIPTION_CHANNEL_CONFIG);
    if (ok != clusters[i].ok)
      printf("  cluster %u channels, 0x%08x\n", clusters[i].channels,
             (unsigned)clusters[i].config);
  }

  /* Output terminal types: not a USB terminal (0x01xx) and none the
   * Terminal Types tables mark input-only: an input terminal (0x02xx,
   * Table 2-2) and the embedded CD player, Phonograph to Radio Receiver
   * (0x0703, 0x0708..0x0710, Table 2-7); the embedded types around them
   * are I/O or O */
  static const struct {
    uint16_t type;
    bool ok;
  } terminals[] = {{0x0101, false}, {0x01FF, false}, {0x0100, false}, {0x0201, false},
                   {0x02FF, false}, {0x0301, true},  {0x0302, true},  {0x0400, true},
                   {0x0602, true},  {0x0700, true},  {0x0701, true},  {0x0702, true},
                   {0x0703, false}, {0x0704, true},  {0x0707, true},  {0x0708, false},
                   {0x0709, false}, {0x070A, false}, {0x070B, false}, {0x070C, false},
                   {0x070D, false}, {0x070E, false}, {0x070F, false}, {0x0710, false},
                   {0x0711, true},  {0x0712, true},  {0x0717, true}};
  for (unsigned i = 0; i < sizeof terminals / sizeof terminals[0]; i++) {
    d = test_desc;
    d.terminal_type = terminals[i].type;
    bool ok = init_with(&d, terminals[i].ok ? U2PS_DESCRIPTION_OK : U2PS_DESCRIPTION_TERMINAL_TYPE);
    if (ok != terminals[i].ok)
      printf("  terminal type 0x%04x\n", terminals[i].type);
  }
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Breaks rule 'rule' of description d, and no other; false for a rule
 * this test has no case for */
static bool break_rule(u2ps_dev_description_t *d, u2ps_dev_description_error_t rule)
{
  static const u2ps_format_t f5[1] = {{5, 32, U2PS_AUDIO_DATA_FORMAT_PCM}};
  static const u2ps_format_t f2_17[1] = {{2, 17, U2PS_AUDIO_DATA_FORMAT_PCM}};
  static const u2ps_format_t float24[1] = {{3, 24, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}};
  static const u2ps_format_t nine[9] = {
    {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}, {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM},
    {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}, {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM},
    {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}, {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM},
    {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}, {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM},
    {2, 16, U2PS_AUDIO_DATA_FORMAT_PCM}};
  static const uint32_t unsorted[2] = {96000, 48000};
  static uint32_t many[43];
  static char long_text[128];
  for (unsigned i = 0; i < 43; i++)
    many[i] = 8000u * (i + 1u);
  memset(long_text, 'x', 127);

  switch (rule) {
  case U2PS_DESCRIPTION_FORMATS:
    d->formats = NULL;
    return true;
  case U2PS_DESCRIPTION_FORMAT_COUNT:
    d->format_count = 0;
    return true;
  case U2PS_DESCRIPTION_SAMPLING_FREQUENCIES:
    d->sampling_frequencies = NULL;
    return true;
  case U2PS_DESCRIPTION_SAMPLING_FREQUENCY_COUNT:
    d->sampling_frequencies = many; /* 43 rates up to 344 kHz: the RANGE needs 518 bytes */
    d->sampling_frequency_count = 43;
    return true;
  case U2PS_DESCRIPTION_NR_CHANNELS:
    d->nr_channels = 0;
    d->channel_config = 0;
    return true;
  case U2PS_DESCRIPTION_MAX_POWER_MA:
    d->max_power_ma = 501;
    return true;
  case U2PS_DESCRIPTION_SELF_POWERED:
    return false; /* VBUS mode bus only: test_dev_vbus_bus */
  case U2PS_DESCRIPTION_MANUFACTURER:
    d->manufacturer = "bell\a";
    return true;
  case U2PS_DESCRIPTION_PRODUCT:
    d->product = long_text;
    return true;
  case U2PS_DESCRIPTION_SERIAL_NUMBER:
    d->serial_number = "caf\xc3\xa9";
    return true;
  case U2PS_DESCRIPTION_CHANNEL_CONFIG:
    d->channel_config = 0x7; /* three locations, two channels */
    return true;
  case U2PS_DESCRIPTION_TERMINAL_TYPE:
    d->terminal_type = 0x0201; /* a microphone: input only */
    return true;
  case U2PS_DESCRIPTION_SAMPLING_FREQUENCY_ORDER:
    d->sampling_frequencies = unsorted;
    d->sampling_frequency_count = 2;
    return true;
  case U2PS_DESCRIPTION_INITIAL_SAMPLING_FREQUENCY:
    d->initial_sampling_frequency = 50000;
    return true;
  case U2PS_DESCRIPTION_SUBSLOT_SIZE:
    d->formats = f5;
    d->format_count = 1;
    return true;
  case U2PS_DESCRIPTION_BIT_RESOLUTION:
    d->formats = f2_17;
    d->format_count = 1;
    return true;
  case U2PS_DESCRIPTION_AUDIO_DATA_FORMAT:
    d->formats = float24;
    d->format_count = 1;
    return true;
  case U2PS_DESCRIPTION_MAX_PACKET_SIZE:
    d->nr_channels = 4; /* 97 * 4 * 4 = 1552 at 768 kHz */
    return true;
  case U2PS_DESCRIPTION_CONFIGURATION_SIZE:
    d->formats = nine;
    d->format_count = 9;
    return true;
  default:
    return false;
  }
}

/* A description that breaks one rule only gives that rule's value, for
 * every rule; one that breaks two gives the first; a valid one gives
 * U2PS_DESCRIPTION_OK again */
static void test_description_errors(void)
{
  u2ps_dev_description_t d;
  CHECK(!init_with(NULL, U2PS_DESCRIPTION_NULL));
  CHECK(init_with(&test_desc, U2PS_DESCRIPTION_OK));
  unsigned cases = 0;
  for (int r = U2PS_DESCRIPTION_FORMATS; r <= U2PS_DESCRIPTION_CONFIGURATION_SIZE; r++) {
    u2ps_dev_description_error_t rule = (u2ps_dev_description_error_t)r;
    d = test_desc;
    if (!break_rule(&d, rule)) {
      /* Mode app accepts a self-powered device */
      CHECK_EQ(rule, U2PS_DESCRIPTION_SELF_POWERED);
      d.self_powered = true;
      CHECK(init_with(&d, U2PS_DESCRIPTION_OK));
      continue;
    }
    cases++;
    if (init_with(&d, rule) || u2ps_dev_description_error() != rule)
      printf("  rule %d: got %d\n", r, (int)u2ps_dev_description_error());
    CHECK(init_with(&test_desc, U2PS_DESCRIPTION_OK));
  }
  CHECK_EQ(cases, 18);
  d = test_desc;
  break_rule(&d, U2PS_DESCRIPTION_TERMINAL_TYPE);
  break_rule(&d, U2PS_DESCRIPTION_MANUFACTURER);
  CHECK(!init_with(&d, U2PS_DESCRIPTION_MANUFACTURER));
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* An IEEE_FLOAT alternate setting (Formats 2.3.1.7.3): bmFormats D2 in its
 * AS general descriptor (Formats Table A-2), everything else as for PCM,
 * byte for byte uac2-hs-16-2_float32-4.txt; u2ps_dev_init() refuses
 * IEEE_FLOAT other than 32 bits in 4-byte subslots, and an Audio Data
 * Format the stack does not know */
static void test_float(void)
{
  static const u2ps_format_t pcm_float[2] = {{2, 16, U2PS_AUDIO_DATA_FORMAT_PCM},
                                             {4, 32, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}};
  uint8_t hs[512], other[512];
  int hs_len = fixture_load("uac2-hs-16-2_float32-4.txt", hs, sizeof hs);
  CHECK(hs_len > 0);
  if (hs_len > 0) {
    test_desc.formats = pcm_float;
    test_desc.format_count = 2;
    dev_start(U2PS_SPEED_HIGH);
    check_get(U2PS_DESC_CONFIGURATION, hs, (uint16_t)hs_len, "HS configuration, float");
    u2ps_fake_bus_reset(U2PS_SPEED_FULL);
    memcpy(other, hs, (size_t)hs_len);
    other[1] = U2PS_DESC_OTHER_SPEED;
    check_get(U2PS_DESC_OTHER_SPEED, other, (uint16_t)hs_len, "HS as other speed, float");
    CHECK_EQ(u2ps_fake_violations, 0);
  }
  test_desc_load();

  u2ps_dev_description_t d;
  static const struct {
    u2ps_format_t format[1];
    u2ps_dev_description_error_t rule;
  } formats[] = {
    {{{4, 32, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}}, U2PS_DESCRIPTION_OK},
    {{{4, 32, U2PS_AUDIO_DATA_FORMAT_PCM}}, U2PS_DESCRIPTION_OK},
    {{{4, 24, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}}, U2PS_DESCRIPTION_AUDIO_DATA_FORMAT},
    {{{4, 31, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}}, U2PS_DESCRIPTION_AUDIO_DATA_FORMAT},
    {{{3, 24, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}}, U2PS_DESCRIPTION_AUDIO_DATA_FORMAT},
    {{{2, 16, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}}, U2PS_DESCRIPTION_AUDIO_DATA_FORMAT},
    {{{1, 8, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}}, U2PS_DESCRIPTION_AUDIO_DATA_FORMAT},
    {{{4, 33, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}}, U2PS_DESCRIPTION_BIT_RESOLUTION},
    {{{4, 32, (u2ps_audio_data_format_t)(U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT + 1)}},
     U2PS_DESCRIPTION_AUDIO_DATA_FORMAT},
    {{{2, 16, (u2ps_audio_data_format_t)(U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT + 1)}},
     U2PS_DESCRIPTION_AUDIO_DATA_FORMAT},
  };
  for (unsigned i = 0; i < sizeof formats / sizeof formats[0]; i++) {
    d = test_desc;
    d.formats = formats[i].format;
    d.format_count = 1;
    bool ok = init_with(&d, formats[i].rule);
    if (ok != (formats[i].rule == U2PS_DESCRIPTION_OK))
      printf("  format %u/%u, Audio Data Format %d\n", formats[i].format[0].bit_resolution,
             formats[i].format[0].subslot_size, (int)formats[i].format[0].audio_data_format);
  }
  /* A float alternate setting after valid PCM ones is checked too */
  static const u2ps_format_t late_bad[3] = {{2, 16, U2PS_AUDIO_DATA_FORMAT_PCM},
                                            {4, 32, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT},
                                            {4, 24, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}};
  d = test_desc;
  d.formats = late_bad;
  d.format_count = 3;
  CHECK(!init_with(&d, U2PS_DESCRIPTION_AUDIO_DATA_FORMAT));
  d.format_count = 2;
  CHECK(init_with(&d, U2PS_DESCRIPTION_OK));
  CHECK_EQ(u2ps_fake_violations, 0);
}

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
  test_fixtures();
  test_device_and_strings();
  test_fields();
  test_validation();
  test_description_errors();
  test_float();
  return check_done();
}
