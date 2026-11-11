/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * host_support.c: see host_support.h.
 */
#include "host_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u2ps_ch9.h"
#include "u2ps_uac2.h"

/* ---- The app ---------------------------------------------------------------- */

happ_log_t happ_log;
void (*happ_stream_start_hook)(const u2ps_host_stream_info_t *info);
void (*happ_sent_hook)(u2ps_uframe_t uframe);
void (*happ_hole_hook)(uint16_t uframes, u2ps_miss_t why);
void (*happ_stream_stop_hook)(u2ps_host_stop_t why);
void (*happ_end_hook)(u2ps_host_end_t why);

static void event(char c)
{
  if (happ_log.nevents + 1u < sizeof happ_log.events)
    happ_log.events[happ_log.nevents++] = c;
}

static u2ps_hfake_from_t from_now(void)
{
  return u2ps_hfake_in_upcall() ? U2PS_HFAKE_FROM_UPCALL
         : u2ps_hfake_masked()  ? U2PS_HFAKE_FROM_MASKED
                                : U2PS_HFAKE_FROM_THREAD;
}

void happ_clear(void)
{
  memset(&happ_log, 0, sizeof happ_log);
  happ_stream_start_hook = NULL;
  happ_sent_hook = NULL;
  happ_hole_hook = NULL;
  happ_stream_stop_hook = NULL;
  happ_end_hook = NULL;
}

bool happ_done(void)
{
  return happ_log.devices > 0u || happ_log.ends > 0u;
}

static const char *copy_string(char *to, const char *s)
{
  if (s == NULL)
    return NULL;
  snprintf(to, 128, "%s", s);
  return to;
}

void u2ps_app_host_on_device(const u2ps_host_device_t *dev)
{
  event('D');
  happ_log.devices++;
  happ_log.device_from = from_now();
  happ_log.dev = *dev;
  happ_log.dev_given = dev;
  happ_log.dev.manufacturer = copy_string(happ_log.manufacturer, dev->manufacturer);
  happ_log.dev.product = copy_string(happ_log.product, dev->product);
  happ_log.dev.serial_number = copy_string(happ_log.serial_number, dev->serial_number);
  unsigned n = dev->num_sub_ranges;
  if (n > sizeof happ_log.sub_ranges / sizeof happ_log.sub_ranges[0]) {
    printf("on_device: %u sub-ranges, more than the log holds\n", n);
    n = 0;
  }
  if (n > 0u)
    memcpy(happ_log.sub_ranges, dev->sub_ranges, n * sizeof dev->sub_ranges[0]);
  happ_log.dev.sub_ranges = happ_log.sub_ranges;
  n = dev->alternate_setting_count;
  if (n > sizeof happ_log.alternate_settings / sizeof happ_log.alternate_settings[0]) {
    printf("on_device: %u alternate settings, more than the log holds\n", n);
    n = 0;
  }
  if (n > 0u)
    memcpy(happ_log.alternate_settings, dev->alternate_settings,
           n * sizeof dev->alternate_settings[0]);
  happ_log.dev.alternate_settings = happ_log.alternate_settings;
}

void u2ps_app_host_on_end(u2ps_host_end_t why, u2ps_host_refusal_t refusal)
{
  event('E');
  happ_log.ends++;
  happ_log.end = why;
  happ_log.refusal = refusal;
  happ_log.end_from = from_now();
  happ_log.calls_at_end = u2ps_hfake_ncalls;
  if (happ_end_hook != NULL)
    happ_end_hook(why);
}

void u2ps_app_host_on_test_device(uint16_t id_product, uint16_t bcd_device)
{
  event('T');
  happ_log.test_devices++;
  happ_log.test_pid = id_product;
  happ_log.test_bcd = bcd_device;
  happ_log.test_from = from_now();
  happ_log.test_ms = u2ps_hfake_now;
  happ_log.calls_at_test = u2ps_hfake_ncalls;
}

void u2ps_app_host_on_resumed(bool remote_wakeup)
{
  event('R');
  happ_log.resumes++;
  happ_log.resumed_remote = remote_wakeup;
  happ_log.resumed_from = from_now();
  happ_log.resumed_ms = u2ps_hfake_now;
  happ_log.calls_at_resumed = u2ps_hfake_ncalls;
}

void u2ps_app_host_on_stream_start(const u2ps_host_stream_info_t *info)
{
  event('S');
  happ_log.stream_starts++;
  happ_log.info = *info;
  if (happ_stream_start_hook != NULL)
    happ_stream_start_hook(info);
}

void u2ps_app_host_on_sent(u2ps_uframe_t uframe)
{
  event('s');
  happ_log.sent++;
  happ_log.sent_uframe = uframe;
  if (happ_sent_hook != NULL)
    happ_sent_hook(uframe);
}

void u2ps_app_host_on_hole(uint16_t uframes, u2ps_miss_t why)
{
  event('h');
  if (happ_log.nholes < HAPP_HOLES) {
    happ_log.holes[happ_log.nholes].uframes = uframes;
    happ_log.holes[happ_log.nholes].why = why;
    happ_log.holes[happ_log.nholes].sent_before = happ_log.sent;
  }
  happ_log.nholes++;
  if (happ_hole_hook != NULL)
    happ_hole_hook(uframes, why);
}

void u2ps_app_host_on_stream_stop(u2ps_host_stop_t why, u2ps_host_stop_request_t request)
{
  event('X');
  happ_log.stream_stops++;
  happ_log.stop = why;
  happ_log.stop_request = request;
  happ_log.stop_from = from_now();
  happ_log.calls_at_stop = u2ps_hfake_ncalls;
  happ_log.queued_at_stop = u2ps_hfake_queued;
  if (happ_stream_stop_hook != NULL)
    happ_stream_stop_hook(why);
}

/* ---- A player and a sink for the stream ------------------------------------------ */

hplayer_t hplayer;
hsink_t hsink;

uint8_t hgen(uint64_t i)
{
  uint32_t x = (uint32_t)i * 2654435761u;
  return (uint8_t)((x >> 24) ^ (uint32_t)(i >> 32));
}

/* Packet buffers, one more than the queue holds, each aligned */
static _Alignas(32) uint8_t player_buf[U2PS_HOST_QUEUE_MAX + 1u][1056];
static unsigned player_next;

void hplayer_fill(void)
{
  while (hplayer.on && u2ps_hfake_queued < hplayer.depth) {
    uint16_t n = u2ps_host_slots_next(hplayer.stream);
    uint8_t *p = player_buf[player_next];
    unsigned len = (unsigned)n * hplayer.slot_bytes;
    if (len > sizeof player_buf[0]) {
      printf("player: a packet of %u bytes\n", len);
      hplayer.refused++;
      return;
    }
    for (unsigned i = 0; i < len; i++)
      p[i] = hgen(hplayer.bytes + i);
    if (!u2ps_host_queue(hplayer.stream, p)) {
      hplayer.refused++;
      return;
    }
    player_next = (player_next + 1u) % (U2PS_HOST_QUEUE_MAX + 1u);
    hplayer.bytes += len;
    hplayer.slots += n;
    if (hplayer.packets < HPLAYER_LOG)
      hplayer.log[hplayer.packets] = n;
    hplayer.packets++;
  }
}

static void player_started(const u2ps_host_stream_info_t *info)
{
  hplayer.stream = info->stream;
  hplayer.slot_bytes = (uint16_t)(info->nr_channels * info->format.subslot_size);
  hplayer_fill();
}

static void player_sent(u2ps_uframe_t uframe)
{
  if (hplayer.sent < HPLAYER_LOG && hplayer.sent < hplayer.packets) {
    hplayer.sent_uframe[hplayer.sent] = uframe;
    hplayer.bytes_sent += (uint64_t)hplayer.log[hplayer.sent] * hplayer.slot_bytes;
  }
  hplayer.sent++;
  hplayer_fill();
}

void hplayer_start(void)
{
  memset(&hplayer, 0, sizeof hplayer);
  hplayer.on = true;
  hplayer.depth = 4;
  player_next = 0;
  happ_stream_start_hook = player_started;
  happ_sent_hook = player_sent;
}

void hsink_clear(void)
{
  memset(&hsink, 0, sizeof hsink);
}

static void sink_log(uint16_t len, u2ps_uframe_t uframe)
{
  if (hsink.packets < HPLAYER_LOG) {
    hsink.len[hsink.packets] = len;
    hsink.uframe[hsink.packets] = uframe;
  }
  hsink.packets++;
}

void hsink_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  bool good = true;
  for (unsigned i = 0; i < len; i++)
    good = good && data[i] == hgen(hsink.bytes + i);
  if (!good)
    hsink.bad++;
  hsink.bytes += len;
  sink_log(len, uframe);
}

void hsink_lost(uint16_t len, u2ps_uframe_t uframe)
{
  hsink.bytes += len;
  sink_log(0, uframe);
}

/* ---- The device model ------------------------------------------------------ */

const uint32_t test_rates_host[10] = {44100,  48000,  88200,  96000,  176400,
                                      192000, 352800, 384000, 705600, 768000};

bool hmodel_load(const char *fixture)
{
  u2ps_hmodel_default();
  u2ps_hmodel.config_len[0] = 0;
  if (fixture != NULL) {
    int n = fixture_load(fixture, u2ps_hmodel.config[0], U2PS_HMODEL_CONFIG_MAX);
    if (n < 0)
      return false;
    u2ps_hmodel.config_len[0] = (uint16_t)n;
  }
  return true;
}

bool hmodel_load_other(uint8_t index, const char *fixture)
{
  static uint8_t buf[U2PS_HMODEL_CONFIG_MAX];
  int n = fixture_load(fixture, buf, sizeof buf);
  if (n < 2)
    return false;
  u2ps_hmodel_set_other(index, buf, (uint16_t)n);
  return true;
}

bool hstart(const char *fixture)
{
  u2ps_hfake_reset_all();
  if (!hmodel_load(fixture))
    return false;
  u2ps_hfake_device = u2ps_hmodel_answer;
  u2ps_host_vbus(true);
  /* A re-init after the first ends the last test's session: its on_end
   * and its port calls are not this test's */
  if (u2ps_host_init() != U2PS_HOST_OK)
    printf("hstart: u2ps_host_init failed\n");
  happ_clear();
  u2ps_hfake_clear_log();
  u2ps_host_connect(true);
  return true;
}

bool henumerate(void)
{
  u2ps_hfake_connect();
  return u2ps_hfake_wait(happ_done, 20000);
}

/* ---- Descriptor lists ---------------------------------------------------------- */

static void put16(uint8_t *p, unsigned v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
  put16(p, v & 0xFFFFu);
  put16(p + 2, v >> 16);
}

bool dl_from(dl_t *l, const uint8_t *bytes, unsigned len)
{
  l->n = 0;
  for (unsigned off = 0; off < len;) {
    unsigned bl = bytes[off];
    if (bl < 2u || off + bl > len || l->n >= DL_MAX)
      return false;
    memcpy(l->d[l->n], bytes + off, bl);
    l->len[l->n++] = (uint16_t)bl;
    off += bl;
  }
  return true;
}

bool dl_load(dl_t *l, const char *fixture)
{
  static uint8_t buf[DL_MAX * 16];
  int n = fixture_load(fixture, buf, sizeof buf);
  return n > 0 && dl_from(l, buf, (unsigned)n);
}

int dl_find(const dl_t *l, uint8_t type, int subtype, unsigned nth)
{
  for (unsigned i = 0; i < l->n; i++)
    if (l->d[i][1] == type && (subtype < 0 || l->d[i][2] == (uint8_t)subtype) && nth-- == 0u)
      return (int)i;
  return -1;
}

int dl_data_ep(const dl_t *l, unsigned nth)
{
  for (unsigned i = 0; i < l->n; i++)
    if (l->d[i][1] == U2PS_DESC_ENDPOINT && (l->d[i][2] & 0x80u) == 0u && nth-- == 0u)
      return (int)i;
  return -1;
}

void dl_insert(dl_t *l, unsigned at, const uint8_t *desc, unsigned len)
{
  if (l->n >= DL_MAX || at > l->n || len > 256u) {
    printf("dl_insert: no room\n");
    return;
  }
  memmove(l->d[at + 1], l->d[at], (l->n - at) * sizeof l->d[0]);
  memmove(&l->len[at + 1], &l->len[at], (l->n - at) * sizeof l->len[0]);
  memset(l->d[at], 0, sizeof l->d[0]);
  memcpy(l->d[at], desc, len);
  l->len[at] = (uint16_t)len;
  l->n++;
}

void dl_insert_hex(dl_t *l, unsigned at, const char *hex)
{
  uint8_t b[256];
  unsigned n = 0;
  for (const char *p = hex; *p != '\0' && n < sizeof b;) {
    char *end;
    unsigned long v = strtoul(p, &end, 16);
    if (end == p)
      break;
    b[n++] = (uint8_t)v;
    p = end;
  }
  dl_insert(l, at, b, n);
}

void dl_remove(dl_t *l, unsigned at)
{
  if (at >= l->n)
    return;
  memmove(l->d[at], l->d[at + 1], (l->n - at - 1u) * sizeof l->d[0]);
  memmove(&l->len[at], &l->len[at + 1], (l->n - at - 1u) * sizeof l->len[0]);
  l->n--;
}

void dl_shorten(dl_t *l, unsigned at, unsigned len)
{
  if (at < l->n && len <= l->len[at]) {
    l->len[at] = (uint16_t)len;
    l->d[at][0] = (uint8_t)len;
  }
}

void dl_truncate(dl_t *l, unsigned n)
{
  if (n < l->n)
    l->n = n;
}

void dl_append(dl_t *l, const dl_t *src, unsigned from, unsigned to)
{
  for (unsigned i = from; i < to && l->n < DL_MAX; i++) {
    memcpy(l->d[l->n], src->d[i], sizeof l->d[0]);
    l->len[l->n++] = src->len[i];
  }
}

unsigned dl_join(const dl_t *l, uint8_t *out, unsigned max, bool fix_total)
{
  unsigned n = 0;
  for (unsigned i = 0; i < l->n; i++) {
    unsigned bl = l->len[i];
    if (n + bl > max) {
      printf("dl_join: too long\n");
      return n;
    }
    memcpy(out + n, l->d[i], bl);
    n += bl;
  }
  if (fix_total && n >= 4u)
    put16(out + 2, n);
  return n;
}

void dl_put16(dl_t *l, unsigned at, unsigned off, uint16_t v)
{
  put16(l->d[at] + off, v);
}

void dl_put32(dl_t *l, unsigned at, unsigned off, uint32_t v)
{
  put32(l->d[at] + off, v);
}
