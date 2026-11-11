/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_host_walk.c: the host stack's descriptor walk and RANGE reader. The
 * fixtures of four format lists; the same descriptors made malformed, which
 * the walk must refuse with the right reason, or unplayable in one
 * alternate setting, which it must skip with the right reason; the scope
 * of an audio IAD; the path to the Output Terminal; the synchronization
 * types; the clock's RANGE reply; and, as a test tool, the choice of an
 * alternate setting for a file. Built twice: against the default library,
 * and against one built with U2PS_HOST_ACCEPT_NO_IAD=1, where only the
 * cases without an IAD expect another result.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "host_support.h"
#include "u2ps_ch9.h"
#include "u2ps_host_priv.h"
#include "u2ps_uac2.h"

/* The fixtures carry four streaming alternate settings, and the checks
 * name them: with fewer kept the test has nothing to check, and is
 * skipped (CTest's SKIP_RETURN_CODE) */
#if U2PS_HOST_ALT_MAX < 4u
int main(void)
{
  printf("test_host_walk: skipped: the library keeps %u alternate settings (U2PS_HOST_ALT_MAX), "
         "the fixtures have 4\n", (unsigned)U2PS_HOST_ALT_MAX);
  return 77;
}
#else

#define F_DEFAULT "uac2-hs-default.txt"     /* 16/2, 24/4, 32/4, 24/3 */
#define F_243     "uac2-hs-24-3.txt"        /* 24/3 */
#define F_162_324 "uac2-hs-16-2_32-4.txt"   /* 16/2, 32/4 */
#define F_243_162 "uac2-hs-24-3_16-2.txt"   /* 24/3, 16/2 */
#define F_FLOAT   "uac2-hs-16-2_float32-4.txt"
#define F_SYNC    "uac2-hs-sync-types.txt"
#define F_HID     "composite-hid-uac2-hs.txt"

#define CS_INTERFACE U2PS_CS_INTERFACE
#define ENDPOINT     U2PS_DESC_ENDPOINT
#define INTERFACE    U2PS_DESC_INTERFACE

static u2ps_host_walk_t w;
static uint8_t bytes[4096];
static dl_t l, l2;

/* wMaxPacketSize per subslot size: 97 samples (768 kHz, rounded up, plus
 * one) of two channels */
static uint16_t out_size(uint8_t slot)
{
  return (uint16_t)(97u * 2u * slot);
}

static u2ps_host_refusal_t walk_list(const dl_t *list, bool fix_total)
{
  unsigned n = dl_join(list, bytes, sizeof bytes, fix_total);
  return u2ps_host_walk(bytes, (uint16_t)n, &w);
}

static bool load(const char *fixture)
{
  bool ok = dl_load(&l, fixture);
  CHECK(ok);
  return ok;
}

/* The kept alternate settings' formats as "<res>/<slot>,...", with "!" after
 * a skipped one */
static const char *formats(void)
{
  static char text[256];
  unsigned n = 0;
  text[0] = '\0';
  for (unsigned i = 0; i < w.alt_count && n + 12u < sizeof text; i++)
    n += (unsigned)snprintf(text + n, sizeof text - n, "%s%u/%u%s", i ? "," : "",
                            w.alts[i].format.bit_resolution, w.alts[i].format.subslot_size,
                            w.alts[i].skip != U2PS_HOST_SKIP_NONE ? "!" : "");
  return text;
}

#define CHECK_FORMATS(want)                                                       \
  do {                                                                            \
    const char *got_ = formats();                                                 \
    CHECK(strcmp(got_, want) == 0);                                               \
    if (strcmp(got_, want) != 0)                                                  \
      printf("  formats %s, want %s\n", got_, want);                              \
  } while (0)

/* ---- The fixtures --------------------------------------------------------------- */

static const struct {
  const char *file;
  const char *formats;
} lists[] = {{F_DEFAULT, "16/2,24/4,32/4,24/3"},
             {F_243, "24/3"},
             {F_162_324, "16/2,32/4"},
             {F_243_162, "24/3,16/2"}};

static void test_fixture_format_lists(void)
{
  for (unsigned f = 0; f < sizeof lists / sizeof lists[0]; f++) {
    if (!load(lists[f].file))
      continue;
    CHECK_EQ(walk_list(&l, false), U2PS_HOST_REFUSAL_NONE);
    CHECK_EQ(w.itf_ac, 0);
    CHECK_EQ(w.itf_as, 1);
    CHECK_EQ(w.clock, 4);
    CHECK(w.clock_programmable);
    CHECK(w.clock_valid_readable);
    CHECK_EQ(w.terminal, 1);
    CHECK_EQ(w.terminal_type, 0x0301);
    CHECK_EQ(w.alt_dropped, 0);
    CHECK_FORMATS(lists[f].formats);
    for (unsigned i = 0; i < w.alt_count; i++) {
      const u2ps_host_alternate_setting_t *a = &w.alts[i];
      CHECK_EQ(a->alternate_setting, i + 1u);
      CHECK_EQ(a->format.audio_data_format, U2PS_AUDIO_DATA_FORMAT_PCM);
      CHECK_EQ(a->nr_channels, 2);
      CHECK_EQ(a->channel_config, 0x3);
      CHECK_EQ(a->max_packet_size, out_size(a->format.subslot_size));
      CHECK_EQ(a->synchronization_type, U2PS_SYNCHRONIZATION_TYPE_ASYNCHRONOUS);
      CHECK(a->explicit_feedback);
      CHECK_EQ(a->skip, U2PS_HOST_SKIP_NONE);
      CHECK_EQ(w.eps[i].ep_out, 0x01);
      CHECK_EQ(w.eps[i].ep_fb, 0x81);
      CHECK_EQ(w.eps[i].fb_mps, 4);
      CHECK_EQ(w.eps[i].fb_interval, 4);
    }
  }
}

static void test_fixtures_hold_no_strings(void)
{
  for (unsigned f = 0; f < sizeof lists / sizeof lists[0]; f++) {
    if (!load(lists[f].file))
      continue;
    CHECK_EQ(l.d[0][1], U2PS_DESC_CONFIGURATION);
    CHECK_EQ(dl_find(&l, U2PS_DESC_STRING, -1, 0), -1);
    unsigned n = dl_join(&l, bytes, sizeof bytes, false);
    CHECK_EQ(bytes[2] | bytes[3] << 8, n);
  }
}

/* A vendor interface before the audio function and after it is skipped */
static void test_other_interfaces_before_and_after(void)
{
  if (!load(F_162_324))
    return;
  l2.n = 0;
  dl_append(&l2, &l, 0, 1);
  dl_insert_hex(&l2, l2.n, "09 04 02 00 01 ff 00 00 00");
  dl_insert_hex(&l2, l2.n, "07 05 82 02 00 02 00");
  dl_append(&l2, &l, 1, l.n);
  dl_insert_hex(&l2, l2.n, "09 04 02 00 01 ff 00 00 00");
  dl_insert_hex(&l2, l2.n, "07 05 82 02 00 02 00");
  l2.d[0][4] = 3;
  CHECK_EQ(walk_list(&l2, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.itf_ac, 0);
  CHECK_EQ(w.itf_as, 1);
  CHECK_EQ(w.clock, 4);
  CHECK(w.clock_programmable);
  CHECK_EQ(w.terminal, 1);
  CHECK_FORMATS("16/2,32/4");
}

/* Terminal -> selector 9 -> Clock Source 4: the requests go to the source.
 * With two inputs the device picks the clock, so it is refused. */
static void test_clock_selector_with_one_input(void)
{
  if (!load(F_243))
    return;
  int it = dl_find(&l, CS_INTERFACE, U2PS_AC_INPUT_TERMINAL, 0);
  l.d[it][7] = 9;
  dl_insert_hex(&l, (unsigned)it, "08 24 0b 09 01 04 03 00");
  int hdr = dl_find(&l, CS_INTERFACE, U2PS_AC_HEADER, 0);
  dl_put16(&l, (unsigned)hdr, 6, (uint16_t)((l.d[hdr][6] | l.d[hdr][7] << 8) + 8));
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.itf_ac, 0);
  CHECK_EQ(w.itf_as, 1);
  CHECK_EQ(w.clock, 4);
  dl_remove(&l, (unsigned)it);
  dl_insert_hex(&l, (unsigned)it, "09 24 0b 09 02 04 05 03 00");
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_CLOCK);
}

static void test_clock_read_only(void)
{
  if (!load(F_243))
    return;
  int cs = dl_find(&l, CS_INTERFACE, U2PS_AC_CLOCK_SOURCE, 0);
  l.d[cs][5] = 0x05; /* frequency read-only, validity read-only */
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK(!w.clock_programmable);
  CHECK(w.clock_valid_readable);
  l.d[cs][5] = 0x03; /* frequency programmable, no validity */
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK(w.clock_programmable);
  CHECK(!w.clock_valid_readable);
}

/* An adaptive endpoint needs no feedback endpoint */
static void test_adaptive_without_feedback(void)
{
  if (!load(F_243))
    return;
  l.d[dl_data_ep(&l, 0)][3] = 0x09; /* isochronous, adaptive */
  dl_remove(&l, (unsigned)dl_find(&l, ENDPOINT, -1, 1));
  l.d[dl_find(&l, INTERFACE, -1, 2)][4] = 1;
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alt_count, 1);
  CHECK_EQ(w.alts[0].alternate_setting, 1);
  CHECK_EQ(w.alts[0].format.subslot_size, 3);
  CHECK_EQ(w.alts[0].format.bit_resolution, 24);
  CHECK_EQ(w.alts[0].synchronization_type, U2PS_SYNCHRONIZATION_TYPE_ADAPTIVE);
  CHECK(!w.alts[0].explicit_feedback);
  CHECK_EQ(w.alts[0].skip, U2PS_HOST_SKIP_NONE);
  CHECK_EQ(w.alts[0].max_packet_size, 582);
  CHECK_EQ(w.eps[0].ep_out, 0x01);
  CHECK_EQ(w.eps[0].ep_fb, 0);
  CHECK_EQ(w.eps[0].fb_mps, 0);
  CHECK_EQ(w.eps[0].fb_interval, 0);
}

/* ---- Malformed descriptors -------------------------------------------------------- */

static void test_length_below_two(void)
{
  for (unsigned n = 0; n < 2u; n++) {
    static const int at[] = {1, 5, 9, -1};
    for (unsigned k = 0; k < 4u; k++) {
      if (!load(F_DEFAULT))
        return;
      unsigned i = at[k] < 0 ? l.n - 1u : (unsigned)at[k];
      l.d[i][0] = (uint8_t)n;
      /* wTotalLength stays: the walk stops at the bad descriptor */
      CHECK_EQ(walk_list(&l, false), U2PS_HOST_REFUSAL_LENGTH);
    }
  }
  /* A one-byte descriptor between two good ones, wTotalLength counting it */
  static const unsigned at1[] = {1, 5, 9};
  for (unsigned k = 0; k < 3u; k++) {
    if (!load(F_DEFAULT))
      return;
    dl_insert_hex(&l, at1[k], "01");
    CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_LENGTH);
  }
}

static void test_total_length_cut(void)
{
  if (!load(F_DEFAULT))
    return;
  static uint8_t data[1024];
  unsigned len = dl_join(&l, data, sizeof data, false);
  const unsigned cuts[] = {len - 1u, len - 7u, 100, 10};
  for (unsigned k = 0; k < 4u; k++)
    CHECK_EQ(u2ps_host_walk(data, (uint16_t)cuts[k], &w), U2PS_HOST_REFUSAL_CUT);
  /* A descriptor that reaches past wTotalLength */
  if (!load(F_243))
    return;
  len = dl_join(&l, data, sizeof data, true);
  data[len - 7u] = 9; /* the last endpoint's bLength */
  CHECK_EQ(u2ps_host_walk(data, (uint16_t)len, &w), U2PS_HOST_REFUSAL_CUT);
  /* Too short for a configuration descriptor at all */
  for (uint16_t n = 0; n < 9u; n++)
    CHECK_EQ(u2ps_host_walk(data, n, &w), U2PS_HOST_REFUSAL_CUT);
}

static void test_not_a_configuration(void)
{
  if (!load(F_243))
    return;
  l.d[0][1] = U2PS_DESC_DEVICE;
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NOT_CONFIG);
  /* An other-speed configuration has a configuration's layout (USB 2.0
   * 9.6.4) and walks as one */
  l.d[0][1] = U2PS_DESC_OTHER_SPEED;
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
}

static void test_short_for_its_type(void)
{
  static const struct {
    uint8_t type;
    int subtype;
    unsigned nth;
  } cases[] = {{INTERFACE, -1, 0},
               {CS_INTERFACE, U2PS_AC_HEADER, 0},
               {CS_INTERFACE, U2PS_AC_CLOCK_SOURCE, 0},
               {CS_INTERFACE, U2PS_AC_INPUT_TERMINAL, 0},
               {CS_INTERFACE, U2PS_AS_GENERAL, 1},
               {CS_INTERFACE, U2PS_AS_FORMAT_TYPE, 1},
               {ENDPOINT, -1, 0},
               /* New in u2ps: the IAD, the Output Terminal, EP_GENERAL */
               {U2PS_DESC_IAD, -1, 0},
               {CS_INTERFACE, U2PS_AC_OUTPUT_TERMINAL, 0},
               {U2PS_CS_ENDPOINT, U2PS_EP_GENERAL, 0}};
  for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
    if (!load(F_243))
      return;
    int i = dl_find(&l, cases[k].type, cases[k].subtype, cases[k].nth);
    CHECK(i > 0);
    if (i <= 0)
      continue;
    dl_shorten(&l, (unsigned)i, cases[k].subtype >= 0 ? 3u : 2u);
    u2ps_host_refusal_t e = walk_list(&l, true);
    CHECK_EQ(e, U2PS_HOST_REFUSAL_LENGTH);
    if (e != U2PS_HOST_REFUSAL_LENGTH)
      printf("  short descriptor type 0x%02x subtype %d\n", cases[k].type, cases[k].subtype);
  }
}

static void test_unknown_class(void)
{
  if (!load(F_DEFAULT))
    return;
  for (unsigned i = 0; i < l.n; i++)
    if (l.d[i][1] == INTERFACE)
      l.d[i][5] = 0xFF;
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NOT_AUDIO);
}

static void test_uac1(void)
{
  if (!load(F_243))
    return;
  for (unsigned i = 0; i < l.n; i++)
    if (l.d[i][1] == INTERFACE)
      l.d[i][7] = 0x00; /* bInterfaceProtocol: UAC1 */
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_UAC1);
  if (!load(F_243))
    return;
  dl_put16(&l, (unsigned)dl_find(&l, CS_INTERFACE, U2PS_AC_HEADER, 0), 3, 0x0100);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_VERSION);
}

/* Asynchronous OUT without a feedback endpoint: skipped (implicit
 * feedback), and with no other alternate setting nothing plays */
static void test_implicit_feedback(void)
{
  if (!load(F_243))
    return;
  dl_remove(&l, (unsigned)dl_find(&l, ENDPOINT, -1, 1));
  l.d[dl_find(&l, INTERFACE, -1, 2)][4] = 1;
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_ALL_SKIPPED);
  CHECK_EQ(w.alt_count, 1);
  CHECK_EQ(w.alts[0].skip, U2PS_HOST_SKIP_IMPLICIT_FEEDBACK);
  CHECK_EQ(w.alts[0].synchronization_type, U2PS_SYNCHRONIZATION_TYPE_ASYNCHRONOUS);
  CHECK(!w.alts[0].explicit_feedback);
  /* The second endpoint is an implicit feedback data endpoint (usage 2) */
  if (!load(F_243))
    return;
  l.d[dl_find(&l, ENDPOINT, -1, 1)][3] = 0x25;
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_ALL_SKIPPED);
  CHECK_EQ(w.alts[0].skip, U2PS_HOST_SKIP_IMPLICIT_FEEDBACK);
  /* In one alternate setting of four: the others play */
  if (!load(F_DEFAULT))
    return;
  dl_remove(&l, (unsigned)dl_find(&l, ENDPOINT, -1, 5));
  l.d[dl_find(&l, INTERFACE, -1, 4)][4] = 1;
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_FORMATS("16/2,24/4,32/4!,24/3");
  CHECK_EQ(w.alts[2].skip, U2PS_HOST_SKIP_IMPLICIT_FEEDBACK);
}

static void test_high_bandwidth(void)
{
  for (unsigned extra = 1; extra <= 2u; extra++) {
    if (!load(F_DEFAULT))
      return;
    dl_put16(&l, (unsigned)dl_data_ep(&l, 2), 4, (uint16_t)(776u | extra << 11));
    CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
    CHECK_FORMATS("16/2,24/4,32/4!,24/3");
    CHECK_EQ(w.alts[2].skip, U2PS_HOST_SKIP_HIGH_BANDWIDTH);
  }
}

static void test_interval(void)
{
  static const uint8_t intervals[] = {0, 2, 4};
  for (unsigned k = 0; k < 3u; k++) {
    if (!load(F_DEFAULT))
      return;
    l.d[dl_data_ep(&l, 1)][6] = intervals[k];
    CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
    CHECK_FORMATS("16/2,24/4!,32/4,24/3");
    CHECK_EQ(w.alts[1].skip, U2PS_HOST_SKIP_INTERVAL);
  }
}

/* Any channel count plays, with its channel configuration */
static void test_channels(void)
{
  static const struct {
    uint8_t n;
    uint32_t config;
  } cases[] = {{1, 0x4}, {6, 0x3F}, {8, 0x63F}};
  for (unsigned k = 0; k < 3u; k++) {
    if (!load(F_DEFAULT))
      return;
    int g = dl_find(&l, CS_INTERFACE, U2PS_AS_GENERAL, 2); /* AS general of alternate setting 2 */
    l.d[g][10] = cases[k].n;
    dl_put32(&l, (unsigned)g, 11, cases[k].config);
    CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
    CHECK_FORMATS("16/2,24/4,32/4,24/3");
    CHECK_EQ(w.alts[1].nr_channels, cases[k].n);
    CHECK_EQ(w.alts[1].channel_config, cases[k].config);
    CHECK_EQ(w.alts[0].nr_channels, 2);
  }
}

static void test_format(void)
{
  /* Another format type, as AS general or format type says: skipped */
  for (unsigned k = 0; k < 3u; k++) {
    if (!load(F_243))
      return;
    int g = dl_find(&l, CS_INTERFACE, U2PS_AS_GENERAL, 1);
    int t = dl_find(&l, CS_INTERFACE, U2PS_AS_FORMAT_TYPE, 1);
    if (k == 0u) {
      dl_remove(&l, (unsigned)t);
      dl_insert_hex(&l, (unsigned)t, "06 24 02 02 03 18"); /* FORMAT_TYPE_II */
    } else if (k == 1u) {
      l.d[g][5] = 0x02; /* bFormatType */
    } else {
      dl_put32(&l, (unsigned)g, 6, 1u << 31); /* bmFormats: TYPE_I_RAW_DATA only */
    }
    CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_ALL_SKIPPED);
    CHECK_EQ(w.alts[0].skip, U2PS_HOST_SKIP_FORMAT);
  }
  /* A subslot or a resolution that does not fit: refused */
  if (!load(F_243))
    return;
  l.d[dl_find(&l, CS_INTERFACE, U2PS_AS_FORMAT_TYPE, 1)][4] = 5;
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_FORMAT);
  if (!load(F_243))
    return;
  l.d[dl_find(&l, CS_INTERFACE, U2PS_AS_FORMAT_TYPE, 1)][5] = 25;
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_FORMAT);
  /* An alternate setting without its format type descriptor */
  if (!load(F_243))
    return;
  dl_remove(&l, (unsigned)dl_find(&l, CS_INTERFACE, U2PS_AS_FORMAT_TYPE, 1));
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_FORMAT);
}

static void test_endpoints(void)
{
  for (unsigned k = 0; k < 4u; k++) {
    if (!load(F_243))
      return;
    int data = dl_data_ep(&l, 0), fb = dl_find(&l, ENDPOINT, -1, 1);
    switch (k) {
    case 0:
      l.d[data][2] = 0x82; /* IN */
      break;
    case 1:
      l.d[data][3] = 0x02; /* bulk */
      break;
    case 2:
      l.d[fb][2] = 0x02; /* feedback OUT */
      break;
    default:
      dl_put16(&l, (unsigned)fb, 4, 3); /* feedback of 3 bytes */
      break;
    }
    u2ps_host_refusal_t e = walk_list(&l, true);
    CHECK_EQ(e, U2PS_HOST_REFUSAL_ENDPOINT);
    if (e != U2PS_HOST_REFUSAL_ENDPOINT)
      printf("  endpoint case %u\n", k);
  }
}

static void test_no_terminal_no_streaming(void)
{
  if (!load(F_243))
    return;
  dl_put16(&l, (unsigned)dl_find(&l, CS_INTERFACE, U2PS_AC_INPUT_TERMINAL, 0), 4, 0x0301);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NO_TERMINAL);
  if (!load(F_243))
    return;
  l.d[dl_find(&l, CS_INTERFACE, U2PS_AS_GENERAL, 1)][3] = 3; /* bTerminalLink: the Output Terminal */
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NO_STREAMING);
  if (!load(F_243))
    return;
  dl_remove(&l, (unsigned)dl_find(&l, INTERFACE, -1, 1)); /* no alternate setting 0 */
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NO_STREAMING);
  if (!load(F_243))
    return;
  dl_truncate(&l, (unsigned)dl_find(&l, INTERFACE, -1, 2)); /* alternate setting 0 only: no link */
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NO_STREAMING);
  if (!load(F_243))
    return;
  int g = dl_find(&l, CS_INTERFACE, U2PS_AS_GENERAL, 1);
  uint8_t general[16];
  memcpy(general, l.d[g], 16);
  dl_truncate(&l, (unsigned)dl_find(&l, INTERFACE, -1, 2));
  dl_insert(&l, l.n, general, 16); /* linked, but alternate setting 0 only */
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NO_ALT);
  if (!load(F_243))
    return;
  l.d[dl_find(&l, CS_INTERFACE, U2PS_AC_CLOCK_SOURCE, 0)][3] = 7; /* the terminal's clock 4 is gone */
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_CLOCK);
}

/* Appends alternate settings first..last as copies of the fixture's
 * alternate setting 1 (from its interface descriptor to the end) */
static void add_alts(dl_t *list, const dl_t *src, unsigned first, unsigned last)
{
  int a = dl_find(src, INTERFACE, -1, 2);
  for (unsigned n = first; n <= last; n++) {
    unsigned at = list->n;
    dl_append(list, src, (unsigned)a, src->n);
    list->d[at][3] = (uint8_t)n;
  }
}

static void test_alts(void)
{
  if (!load(F_162_324))
    return;
  l.d[dl_find(&l, INTERFACE, -1, 3)][3] = 1; /* alternate setting 2 numbered 1 again */
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_ALT_REPEATED);
  /* Nine alternate settings, more than the 8 the walk once kept */
  if (!load(F_243))
    return;
  l2 = l;
  add_alts(&l2, &l, 2, 9);
  CHECK_EQ(walk_list(&l2, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alt_count, 9);
  /* U2PS_HOST_ALT_MAX (16) kept: all; one more is not kept */
  l2 = l;
  add_alts(&l2, &l, 2, U2PS_HOST_ALT_MAX);
  CHECK_EQ(walk_list(&l2, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alt_count, U2PS_HOST_ALT_MAX);
  CHECK_EQ(w.alt_dropped, 0);
  add_alts(&l2, &l, U2PS_HOST_ALT_MAX + 1u, U2PS_HOST_ALT_MAX + 1u);
  CHECK_EQ(walk_list(&l2, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alt_count, U2PS_HOST_ALT_MAX);
  CHECK_EQ(w.alt_dropped, 1);
  CHECK_EQ(w.alts[0].alternate_setting, 1);
  CHECK_EQ(w.alts[U2PS_HOST_ALT_MAX - 1u].alternate_setting, U2PS_HOST_ALT_MAX);
  /* Listed highest first: the lowest-numbered 16 are kept all the same */
  l2 = l;
  dl_truncate(&l2, (unsigned)dl_find(&l, INTERFACE, -1, 2));
  add_alts(&l2, &l, U2PS_HOST_ALT_MAX + 1u, U2PS_HOST_ALT_MAX + 1u);
  add_alts(&l2, &l, 1, U2PS_HOST_ALT_MAX);
  CHECK_EQ(walk_list(&l2, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alt_count, U2PS_HOST_ALT_MAX);
  CHECK_EQ(w.alt_dropped, 1);
  CHECK_EQ(w.alts[U2PS_HOST_ALT_MAX - 1u].alternate_setting, U2PS_HOST_ALT_MAX);
}

/* Listed as alternate setting 2, then 1: kept in alternate setting order */
static void test_alts_out_of_order(void)
{
  if (!load(F_162_324))
    return;
  int a1 = dl_find(&l, INTERFACE, -1, 2), a2 = dl_find(&l, INTERFACE, -1, 3);
  l2.n = 0;
  dl_append(&l2, &l, 0, (unsigned)a1);
  dl_append(&l2, &l, (unsigned)a2, l.n);
  dl_append(&l2, &l, (unsigned)a1, (unsigned)a2);
  CHECK_EQ(walk_list(&l2, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_FORMATS("16/2,32/4");
  CHECK_EQ(w.alts[0].alternate_setting, 1);
  CHECK_EQ(w.alts[0].format.subslot_size, 2);
}

/* Any prefix and any single-byte change: an answer, never a crash or a read
 * past the end (the sanitizers would show one) */
static void test_truncation_and_noise(void)
{
  if (!load(F_DEFAULT))
    return;
  static uint8_t data[1024], noisy[1024];
  unsigned len = dl_join(&l, data, sizeof data, false);
  for (unsigned n = 0; n < len; n++) {
    /* A copy of exactly n bytes, so that ASan sees any read past them */
    uint8_t *cut = malloc(n > 0u ? n : 1u);
    memcpy(cut, data, n);
    u2ps_host_refusal_t e = u2ps_host_walk(cut, (uint16_t)n, &w);
    CHECK(e != U2PS_HOST_REFUSAL_NONE && e <= U2PS_HOST_REFUSAL_ALL_SKIPPED);
    free(cut);
  }
  uint32_t seed = 1;
  for (unsigned k = 0; k < 300u; k++) {
    memcpy(noisy, data, len);
    seed = seed * 1103515245u + 12345u;
    unsigned at = (seed >> 8) % len;
    seed = seed * 1103515245u + 12345u;
    noisy[at] = (uint8_t)(seed >> 16);
    uint8_t *copy = malloc(len);
    memcpy(copy, noisy, len);
    u2ps_host_refusal_t e = u2ps_host_walk(copy, (uint16_t)len, &w);
    CHECK(e <= U2PS_HOST_REFUSAL_ALL_SKIPPED);
    CHECK(w.alt_count <= U2PS_HOST_ALT_MAX);
    free(copy);
  }
}

/* ---- RANGE ------------------------------------------------------------------------ */

static const uint32_t rates[10] = {44100,  48000,  88200,  96000,  176400,
                                   192000, 352800, 384000, 705600, 768000};

static uint8_t reply[2 + 12 * 64];

/* A RANGE reply of n sub-ranges; returns its length */
static uint16_t range_reply(const uint32_t (*sub)[3], unsigned n)
{
  reply[0] = (uint8_t)n;
  reply[1] = (uint8_t)(n >> 8);
  for (unsigned i = 0; i < n; i++)
    for (unsigned j = 0; j < 3u; j++) {
      uint32_t v = sub[i][j];
      uint8_t *p = reply + 2 + 12 * i + 4 * j;
      p[0] = (uint8_t)v;
      p[1] = (uint8_t)(v >> 8);
      p[2] = (uint8_t)(v >> 16);
      p[3] = (uint8_t)(v >> 24);
    }
  return (uint16_t)(2u + 12u * n);
}

/* Test tool: bit i for each of the ten rates inside a sub-range. A
 * sub-range takes MIN, MIN + RES, ... up to MAX, and only MIN when RES is
 * 0. */
static uint32_t rate_mask(const u2ps_host_sub_range_t *s, unsigned n)
{
  uint32_t mask = 0;
  for (unsigned k = 0; k < n; k++)
    for (unsigned i = 0; i < 10u; i++) {
      uint32_t r = rates[i];
      if (s[k].res == 0u ? r == s[k].min
                         : (r >= s[k].min && r <= s[k].max && (r - s[k].min) % s[k].res == 0u))
        mask |= 1u << i;
    }
  return mask;
}

static u2ps_host_sub_range_t sub[64];

static void test_range_discrete(void)
{
  uint32_t r[10][3];
  for (unsigned i = 0; i < 10u; i++)
    r[i][0] = r[i][1] = rates[i], r[i][2] = 0;
  uint16_t n = range_reply(r, 10);
  CHECK_EQ(u2ps_host_range(reply, n, sub, 32), 10);
  CHECK_EQ(rate_mask(sub, 10), 0x3FF);
  for (unsigned i = 0; i < 10u; i++) {
    CHECK_EQ(sub[i].min, rates[i]);
    CHECK_EQ(sub[i].max, rates[i]);
    CHECK_EQ(sub[i].res, 0);
  }
}

static void test_range_continuous(void)
{
  static const uint32_t all[1][3] = {{8000, 768000, 100}};
  CHECK_EQ(u2ps_host_range(reply, range_reply(all, 1), sub, 32), 1);
  CHECK_EQ(rate_mask(sub, 1), 0x3FF);
  CHECK_EQ(sub[0].min, 8000);
  CHECK_EQ(sub[0].max, 768000);
  CHECK_EQ(sub[0].res, 100);
  static const uint32_t no_res[1][3] = {{44100, 96000, 0}};
  CHECK_EQ(u2ps_host_range(reply, range_reply(no_res, 1), sub, 32), 1);
  CHECK_EQ(rate_mask(sub, 1), 0x1);
  static const uint32_t steps[1][3] = {{32000, 96000, 16000}};
  CHECK_EQ(u2ps_host_range(reply, range_reply(steps, 1), sub, 32), 1);
  CHECK_EQ(rate_mask(sub, 1), 0x2 | 0x8);
  static const uint32_t none[2][3] = {{32000, 32000, 0}, {22050, 22050, 0}};
  CHECK_EQ(u2ps_host_range(reply, range_reply(none, 2), sub, 32), 2);
  CHECK_EQ(rate_mask(sub, 2), 0);
}

static void test_range_short(void)
{
  uint32_t r[10][3];
  for (unsigned i = 0; i < 10u; i++)
    r[i][0] = r[i][1] = rates[i], r[i][2] = 0;
  range_reply(r, 10);
  /* Truncated: the whole sub-ranges only */
  CHECK_EQ(u2ps_host_range(reply, 2 + 12 * 3 + 5, sub, 32), 3);
  CHECK_EQ(rate_mask(sub, 3), 0x7);
  static const uint16_t too_short[] = {0, 1, 2, 13};
  for (unsigned k = 0; k < 4u; k++)
    CHECK_EQ(u2ps_host_range(reply, too_short[k], sub, 32), 0);
  /* wNumSubRanges 0 */
  CHECK_EQ(u2ps_host_range(reply, range_reply(r, 0), sub, 32), 0);
  /* wNumSubRanges smaller than the reply: only that many */
  range_reply(r, 10);
  reply[0] = 2;
  CHECK_EQ(u2ps_host_range(reply, 2 + 12 * 10, sub, 32), 2);
  CHECK_EQ(rate_mask(sub, 2), 0x3);
}

/* More sub-ranges than kept: the first 'max' */
static void test_range_over_max(void)
{
  static uint32_t r[40][3];
  for (unsigned i = 0; i < 40u; i++)
    r[i][0] = r[i][1] = 8000u * (i + 1u), r[i][2] = 0;
  uint16_t n = range_reply(r, 40);
  CHECK_EQ(u2ps_host_range(reply, n, sub, 32), 32);
  CHECK_EQ(sub[31].min, 8000u * 32u);
  CHECK_EQ(u2ps_host_range(reply, n, sub, 40), 40);
}

/* ---- Test tool: the alternate setting for a file -------------------------------- */

/* The bytes of the largest packet at rate: the samples of a microframe
 * rounded up, plus one, in every channel */
static uint32_t packet_max(uint32_t rate, const u2ps_host_alternate_setting_t *a)
{
  return ((rate + 7999u) / 8000u + 1u) * a->format.subslot_size * a->nr_channels;
}

/* The playable alternate setting for a file of 'depth' bits at 'rate' (DoP:
 * 24), or 0: the smallest subslot that holds the depth, with a resolution
 * of at least the depth, whose wMaxPacketSize holds the largest packet;
 * among equal subslots the first */
static unsigned choose(uint32_t rate, uint8_t depth, bool dop)
{
  unsigned need = dop ? 24u : depth;
  int best = -1;
  for (unsigned i = 0; i < w.alt_count; i++) {
    const u2ps_host_alternate_setting_t *a = &w.alts[i];
    if (a->skip != U2PS_HOST_SKIP_NONE || a->format.bit_resolution < need ||
        8u * a->format.subslot_size < need || packet_max(rate, a) > a->max_packet_size)
      continue;
    if (best < 0 || a->format.subslot_size < w.alts[best].format.subslot_size)
      best = (int)i;
  }
  return best < 0 ? 0u : w.alts[best].alternate_setting;
}

static void test_choose(void)
{
  static const struct {
    const char *file;
    uint32_t rate;
    uint8_t depth;
    bool dop;
    unsigned want;
  } cases[] = {
    {F_DEFAULT, 48000, 16, false, 1},  {F_DEFAULT, 48000, 24, false, 4},
    {F_DEFAULT, 768000, 32, false, 3}, {F_DEFAULT, 176400, 24, true, 4},
    {F_243, 44100, 16, false, 1},      {F_243, 768000, 32, false, 0},
    {F_162_324, 96000, 24, false, 2},  {F_162_324, 705600, 24, true, 2},
    {F_243_162, 768000, 16, false, 2}, {F_243_162, 768000, 24, false, 1},
    {F_243_162, 352800, 24, true, 1},
  };
  for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
    if (!load(cases[k].file))
      return;
    CHECK_EQ(walk_list(&l, false), U2PS_HOST_REFUSAL_NONE);
    unsigned got = choose(cases[k].rate, cases[k].depth, cases[k].dop);
    CHECK_EQ(got, cases[k].want);
    if (got != cases[k].want)
      printf("  choose %s %u Hz %u bits%s\n", cases[k].file, (unsigned)cases[k].rate,
             cases[k].depth, cases[k].dop ? " DoP" : "");
  }
}

/* DoP needs 3 or 4 bytes: a 16/2-only device cannot take it */
static void test_choose_dop_slot(void)
{
  if (!load(F_162_324))
    return;
  dl_truncate(&l, (unsigned)dl_find(&l, INTERFACE, -1, 3));
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(choose(88200, 16, false), 1);
  CHECK_EQ(choose(88200, 24, true), 0);
}

/* A 24/3 alternate setting whose wMaxPacketSize holds 49 samples (384 kHz)
 * but not 97 */
static void test_choose_packet_size(void)
{
  if (!load(F_243))
    return;
  unsigned ep = (unsigned)dl_data_ep(&l, 0);
  dl_put16(&l, ep, 4, 49 * 6);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(choose(384000, 24, false), 1);
  CHECK_EQ(choose(705600, 24, false), 0);
  dl_put16(&l, ep, 4, 49 * 6 - 1);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(choose(384000, 24, false), 0);
  /* 44.1 kHz: 5.5125 samples, so 7 at most */
  dl_put16(&l, ep, 4, 7 * 6);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(choose(44100, 24, false), 1);
  dl_put16(&l, ep, 4, 7 * 6 - 1);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(choose(44100, 24, false), 0);
}

/* ---- The audio function's scope: IADs ------------------------------------------- */

/* Two audio functions, each with its IAD and the same entity IDs:
 * function A on interfaces 0 and 1 (clock read-only, 16/2), function B on
 * interfaces 2 and 3 (clock programmable, headphones, 24/3 and 32/4) */
static void two_functions(dl_t *list)
{
  dl_t b;
  dl_load(list, F_162_324);
  dl_truncate(list, (unsigned)dl_find(list, INTERFACE, -1, 3)); /* A: 16/2 only */
  list->d[dl_find(list, CS_INTERFACE, U2PS_AC_CLOCK_SOURCE, 0)][5] = 0x05;
  dl_load(&b, F_243);
  dl_append(&b, &b, (unsigned)dl_find(&b, INTERFACE, -1, 2), b.n); /* a second 24/3 */
  int a2 = dl_find(&b, INTERFACE, -1, 3);
  b.d[a2][3] = 2;
  b.d[dl_find(&b, CS_INTERFACE, U2PS_AS_FORMAT_TYPE, 2)][4] = 4;
  b.d[dl_find(&b, CS_INTERFACE, U2PS_AS_FORMAT_TYPE, 2)][5] = 32;
  dl_put16(&b, (unsigned)dl_data_ep(&b, 1), 4, 776);
  dl_put16(&b, (unsigned)dl_find(&b, CS_INTERFACE, U2PS_AC_OUTPUT_TERMINAL, 0), 4, 0x0302);
  for (unsigned i = 1; i < b.n; i++) {
    if (b.d[i][1] == U2PS_DESC_IAD)
      b.d[i][2] = 2; /* bFirstInterface */
    if (b.d[i][1] == INTERFACE)
      b.d[i][2] = (uint8_t)(b.d[i][2] + 2u);
  }
  dl_append(list, &b, 1, b.n);
  list->d[0][4] = 4;
}

static void test_iad_two_functions(void)
{
  /* Both playable: the first */
  two_functions(&l);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.itf_ac, 0);
  CHECK_EQ(w.itf_as, 1);
  CHECK(!w.clock_programmable);
  CHECK_FORMATS("16/2");
  /* The first not playable (native DSD only): the second, with its own
   * entities */
  two_functions(&l);
  dl_put32(&l, (unsigned)dl_find(&l, CS_INTERFACE, U2PS_AS_GENERAL, 1), 6, 1u << 31);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.itf_ac, 2);
  CHECK_EQ(w.itf_as, 3);
  CHECK_EQ(w.clock, 4);
  CHECK(w.clock_programmable);
  CHECK_EQ(w.terminal_type, 0x0302);
  CHECK_FORMATS("24/3,32/4");
  /* The second's AS interface linked to the first's terminal ID only:
   * entity IDs are unique only within a function, so the second has no
   * streaming interface, and the first plays nothing */
  two_functions(&l);
  dl_put32(&l, (unsigned)dl_find(&l, CS_INTERFACE, U2PS_AS_GENERAL, 1), 6, 1u << 31);
  /* Subtype 2 is also the format type: A's Input Terminal, A's format,
   * then B's Input Terminal */
  int it_b = dl_find(&l, CS_INTERFACE, U2PS_AC_INPUT_TERMINAL, 2);
  l.d[it_b][3] = 5; /* B's terminal is 5; its AS interfaces still link 1 */
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_ALL_SKIPPED);
  CHECK_EQ(w.itf_ac, 0);
}

/* The audio function without an IAD: refused by default; with
 * U2PS_HOST_ACCEPT_NO_IAD accepted when the configuration has exactly one
 * AudioControl interface */
static void test_no_iad(void)
{
  if (!load(F_243))
    return;
  dl_remove(&l, (unsigned)dl_find(&l, U2PS_DESC_IAD, -1, 0));
#if U2PS_HOST_ACCEPT_NO_IAD
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.itf_ac, 0);
  CHECK_EQ(w.itf_as, 1);
  CHECK_FORMATS("24/3");
#else
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NO_IAD);
#endif
  /* Two AudioControl interfaces without IADs: refused either way */
  two_functions(&l);
  for (int i = dl_find(&l, U2PS_DESC_IAD, -1, 0); i >= 0; i = dl_find(&l, U2PS_DESC_IAD, -1, 0))
    dl_remove(&l, (unsigned)i);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NO_IAD);
  /* A configuration with neither IAD nor AudioControl interface */
  if (!load(F_243))
    return;
  dl_remove(&l, (unsigned)dl_find(&l, U2PS_DESC_IAD, -1, 0));
  l.d[dl_find(&l, INTERFACE, -1, 0)][6] = 0x03; /* subclass MIDI streaming */
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NOT_AUDIO);
}

static void test_composite_hid(void)
{
  if (!load(F_HID))
    return;
  CHECK_EQ(walk_list(&l, false), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.itf_ac, 1);
  CHECK_EQ(w.itf_as, 2);
  CHECK_EQ(w.clock, 4);
  CHECK_FORMATS("24/3");
}

/* ---- Skipped alternate settings and the formats ---------------------------------- */

static void test_skips(void)
{
  /* Native DSD (bmFormats D31) skipped; D31 with PCM plays as PCM */
  if (!load(F_DEFAULT))
    return;
  int g = dl_find(&l, CS_INTERFACE, U2PS_AS_GENERAL, 4);
  dl_put32(&l, (unsigned)g, 6, 1u << 31);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_FORMATS("16/2,24/4,32/4,24/3!");
  CHECK_EQ(w.alts[3].skip, U2PS_HOST_SKIP_FORMAT);
  dl_put32(&l, (unsigned)g, 6, 1u << 31 | 1u);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alts[3].skip, U2PS_HOST_SKIP_NONE);
  CHECK_EQ(w.alts[3].format.audio_data_format, U2PS_AUDIO_DATA_FORMAT_PCM);
  /* IEEE_FLOAT 32/4 plays; IEEE_FLOAT 24/3 is skipped */
  if (!load(F_FLOAT))
    return;
  CHECK_EQ(walk_list(&l, false), U2PS_HOST_REFUSAL_NONE);
  CHECK_FORMATS("16/2,32/4");
  CHECK_EQ(w.alts[0].format.audio_data_format, U2PS_AUDIO_DATA_FORMAT_PCM);
  CHECK_EQ(w.alts[1].format.audio_data_format, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT);
  CHECK_EQ(w.alts[1].skip, U2PS_HOST_SKIP_NONE);
  int t = dl_find(&l, CS_INTERFACE, U2PS_AS_FORMAT_TYPE, 2);
  l.d[t][4] = 3;
  l.d[t][5] = 24;
  CHECK_EQ(walk_list(&l, false), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alts[1].skip, U2PS_HOST_SKIP_FORMAT);
  /* MaxPacketsOnly (EP_GENERAL D7) */
  if (!load(F_DEFAULT))
    return;
  l.d[dl_find(&l, U2PS_CS_ENDPOINT, U2PS_EP_GENERAL, 1)][3] = 0x80;
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_FORMATS("16/2,24/4!,32/4,24/3");
  CHECK_EQ(w.alts[1].skip, U2PS_HOST_SKIP_MAX_PACKETS_ONLY);
  /* A packet of 1,025 bytes is skipped; 1,024 plays */
  if (!load(F_DEFAULT))
    return;
  unsigned ep = (unsigned)dl_data_ep(&l, 0);
  dl_put16(&l, ep, 4, 1025);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alts[0].skip, U2PS_HOST_SKIP_PACKET_SIZE);
  dl_put16(&l, ep, 4, 1024);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alts[0].skip, U2PS_HOST_SKIP_NONE);
  CHECK_EQ(w.alts[0].max_packet_size, 1024);
  /* No channel: skipped */
  if (!load(F_DEFAULT))
    return;
  l.d[dl_find(&l, CS_INTERFACE, U2PS_AS_GENERAL, 1)][10] = 0;
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alts[0].skip, U2PS_HOST_SKIP_FORMAT);
  /* One channel and eight, and a 1-byte subslot */
  if (!load(F_243))
    return;
  g = dl_find(&l, CS_INTERFACE, U2PS_AS_GENERAL, 1);
  l.d[g][10] = 1;
  dl_put32(&l, (unsigned)g, 11, 0x4);
  t = dl_find(&l, CS_INTERFACE, U2PS_AS_FORMAT_TYPE, 1);
  l.d[t][4] = 1;
  l.d[t][5] = 8;
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alts[0].nr_channels, 1);
  CHECK_EQ(w.alts[0].channel_config, 0x4);
  CHECK_FORMATS("8/1");
  l.d[g][10] = 8;
  dl_put32(&l, (unsigned)g, 11, 0x63F);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alts[0].nr_channels, 8);
  CHECK_EQ(w.alts[0].channel_config, 0x63F);
}

/* One asynchronous, one adaptive (which also declares a feedback
 * endpoint) and one synchronous alternate setting: all play; only the
 * asynchronous one follows feedback. No Synchronization plays too. */
static void test_synchronization_types(void)
{
  if (!load(F_SYNC))
    return;
  CHECK_EQ(walk_list(&l, false), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alt_count, 3);
  static const u2ps_synchronization_type_t want[3] = {
    U2PS_SYNCHRONIZATION_TYPE_ASYNCHRONOUS, U2PS_SYNCHRONIZATION_TYPE_ADAPTIVE,
    U2PS_SYNCHRONIZATION_TYPE_SYNCHRONOUS};
  for (unsigned i = 0; i < 3u; i++) {
    CHECK_EQ(w.alts[i].synchronization_type, want[i]);
    CHECK_EQ(w.alts[i].explicit_feedback, i == 0u);
    CHECK_EQ(w.alts[i].skip, U2PS_HOST_SKIP_NONE);
    CHECK_EQ(w.eps[i].ep_out, 0x01);
    CHECK_EQ(w.eps[i].ep_fb, i == 0u ? 0x81 : 0);
  }
  /* The adaptive one's feedback endpoint is ignored, even a bad one */
  dl_put16(&l, (unsigned)dl_find(&l, ENDPOINT, -1, 3), 4, 2);
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alts[1].skip, U2PS_HOST_SKIP_NONE);
  /* No Synchronization */
  l.d[dl_data_ep(&l, 2)][3] = 0x01;
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.alts[2].synchronization_type, U2PS_SYNCHRONIZATION_TYPE_NONE);
  CHECK(!w.alts[2].explicit_feedback);
  CHECK_EQ(w.alts[2].skip, U2PS_HOST_SKIP_NONE);
}

/* ---- The Output Terminal on the path ----------------------------------------------- */

static void test_terminal_path(void)
{
  /* Input Terminal 1 -> Feature Unit 2 -> Output Terminal 3 */
  if (!load(F_243))
    return;
  int hdr = dl_find(&l, CS_INTERFACE, U2PS_AC_HEADER, 0);
  int ot = dl_find(&l, CS_INTERFACE, U2PS_AC_OUTPUT_TERMINAL, 0);
  l.d[ot][7] = 2;
  dl_insert_hex(&l, (unsigned)ot, "12 24 06 02 01 00 00 00 00 00 00 00 00 00 00 00 00 00");
  dl_put16(&l, (unsigned)hdr, 6, (uint16_t)(0x2e + 18));
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.terminal_type, 0x0301);
  /* Two Feature Units: 1 -> 2 -> 5 -> 3, the second listed first */
  l.d[ot + 1][7] = 5;
  dl_insert_hex(&l, (unsigned)ot, "12 24 06 05 02 00 00 00 00 00 00 00 00 00 00 00 00 00");
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.terminal_type, 0x0301);
  /* A Mixer Unit on the path: unknown */
  if (!load(F_243))
    return;
  ot = dl_find(&l, CS_INTERFACE, U2PS_AC_OUTPUT_TERMINAL, 0);
  l.d[ot][7] = 6;
  dl_insert_hex(&l, (unsigned)ot, "0d 24 04 06 01 01 02 00 00 00 00 00 00");
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.terminal_type, 0);
  /* An Output Terminal fed by another terminal, then the right one */
  if (!load(F_243))
    return;
  ot = dl_find(&l, CS_INTERFACE, U2PS_AC_OUTPUT_TERMINAL, 0);
  dl_insert_hex(&l, (unsigned)ot, "0c 24 03 07 02 06 00 08 04 00 00 00");
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.terminal_type, 0x0301);
  /* A Feature Unit feeding itself: no loop forever */
  if (!load(F_243))
    return;
  ot = dl_find(&l, CS_INTERFACE, U2PS_AC_OUTPUT_TERMINAL, 0);
  l.d[ot][7] = 2;
  dl_insert_hex(&l, (unsigned)ot, "12 24 06 02 02 00 00 00 00 00 00 00 00 00 00 00 00 00");
  CHECK_EQ(walk_list(&l, true), U2PS_HOST_REFUSAL_NONE);
  CHECK_EQ(w.terminal_type, 0);
}

int main(void)
{
  /* The cases ported from the earlier parser's tests */
  test_fixture_format_lists();
  test_fixtures_hold_no_strings();
  test_other_interfaces_before_and_after();
  test_clock_selector_with_one_input();
  test_clock_read_only();
  test_adaptive_without_feedback();
  test_length_below_two();
  test_total_length_cut();
  test_not_a_configuration();
  test_short_for_its_type();
  test_unknown_class();
  test_uac1();
  test_implicit_feedback();
  test_high_bandwidth();
  test_interval();
  test_channels();
  test_format();
  test_endpoints();
  test_no_terminal_no_streaming();
  test_alts();
  test_alts_out_of_order();
  test_truncation_and_noise();
  test_range_discrete();
  test_range_continuous();
  test_range_short();
  test_choose();
  test_choose_dop_slot();
  test_choose_packet_size();
  /* New for u2ps */
  test_range_over_max();
  test_iad_two_functions();
  test_no_iad();
  test_composite_hid();
  test_skips();
  test_synchronization_types();
  test_terminal_path();
  return check_done();
}
#endif
