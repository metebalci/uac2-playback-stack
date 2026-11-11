/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_host_loopback.c: the host stack and the u2ps device
 * stack in one binary. The fake host port's bus steps microframes: each
 * packet the host sends goes, with its bytes and its microframe, into the
 * fake device port, and each feedback poll reads the device stack's
 * feedback value. Control transfers go to the device stack as in
 * test_host_roundtrip.c. Checked:
 * - at every sampling frequency from 44.1 to 768 kHz, 8,000 microframes:
 *   the device receives exactly the bytes the host sent, each packet in
 *   the microframe of the host's on_sent; every packet is INT(nav) or
 *   INT(nav) + 1 slots; the slots differ from the feedback's rate by less
 *   than one slot;
 * - one LATE microframe: the host's hole before the packet's on_sent, the
 *   device's NO_PACKET of 1 microframe there, no byte lost, the debt
 *   repaid by packets of INT(nav) + 1;
 * - NO_BUFFER for 1, 5 and 500 microframes, with the debt capped at
 *   U2PS_HOST_DEBT_MAX (8) microframes' worth: at 48 kHz (6 slots per
 *   microframe), 6, 30 and 48 slots repaid by packets of 7, and 0, 0 and
 *   2,952 slots forgiven; the device's NO_PACKET hole, or a pause for 500;
 * - a drain of 5 and 500 microframes: no hole, drained counts them, the
 *   accumulator does not move (every packet stays at 6 slots), the device
 *   reports a NO_PACKET hole or a pause;
 * - the feedback stepped within +-12.5 % of nominal, and once beyond it;
 * - a tight wMaxPacketSize at the window's top: no packet longer, the cut
 *   counted in slots_clamped;
 * - adaptive and synchronous alternate settings (uac2-hs-sync-types.txt
 *   through the device model): no feedback endpoint, the nominal rate
 *   exactly:
 *   441 x k slots after 80 x k packets at 44.1 kHz, and after n packets
 *   INT(n * fs / 8000) slots at every frequency.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "dev_support.h"
#include "host_support.h"

/* ---- The bus between the two stacks ------------------------------------------------ */

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

static void bus_out(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  u2ps_fake_packet_bytes(data, len, uframe);
}

/* The device's feedback endpoint sends its current value */
static int bus_fb(uint8_t *data, uint16_t max, u2ps_uframe_t uframe)
{
  (void)uframe;
  if (max < 4u || u2ps_fake_stream_state != U2PS_FAKE_ST_OPEN)
    return -1;
  for (unsigned i = 0; i < 4u; i++)
    data[i] = (uint8_t)(u2ps_fake_fb >> (8u * i));
  return 4;
}

/* The device's tick; its port ends a close within 1 ms; its call log, of
 * a release per packet, is not kept */
static void ms_tick(uint32_t now)
{
  if (u2ps_fake_stream_state == U2PS_FAKE_ST_CLOSING)
    u2ps_fake_closed();
  (void)u2ps_dev_tick(now);
  u2ps_fake_clear_log();
}

/* The device app's packets go to the sink */
static void device_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  hsink_packet(data, len, uframe);
}

static bool stream_decided(void)
{
  return happ_log.stream_starts > 0u || happ_log.stream_stops > 0u;
}

/* The device stack with description d, enumerated by the host stack */
static bool connect_device(const u2ps_dev_description_t *d)
{
  test_desc = *d;
  dev_start(U2PS_SPEED_HIGH);
  if (!hstart(NULL))
    return false;
  u2ps_hfake_device = bridge;
  if (!henumerate() || happ_log.devices != 1u) {
    printf("  the device stack did not enumerate\n");
    return false;
  }
  return true;
}

/* A stream of alternate setting 'alt' at 'hz', with the player and the
 * sink, from a clean slate on both sides */
static bool play(uint8_t alt, uint32_t hz)
{
  happ_clear();
  hplayer_start();
  hsink_clear();
  app_log.nholes = 0;
  app_log.npauses = 0;
  app_packet_hook = device_packet;
  u2ps_hfake_bus_out = bus_out;
  u2ps_hfake_bus_fb = bus_fb;
  u2ps_hfake_ms_hook = ms_tick;
  u2ps_hfake_bus_corrupt = NULL;
  bool started = u2ps_host_stream_start(alt, hz);
  CHECK(started);
  CHECK(u2ps_hfake_wait(stream_decided, 100));
  CHECK_EQ(happ_log.stream_starts, 1);
  if (!started || happ_log.stream_starts != 1u) {
    printf("  no stream at %u Hz: stop %d, %u violations\n", (unsigned)hz, (int)happ_log.stop,
           u2ps_hfake_violations);
    return false;
  }
  return true;
}

static void stop(void)
{
  u2ps_host_stream_stop(happ_log.info.stream);
  for (unsigned i = 0; i < 800u && happ_log.stream_stops == 0u; i++)
    u2ps_hfake_uframes(1);
  CHECK_EQ(happ_log.stream_stops, 1);
  CHECK_EQ(happ_log.stop, U2PS_HOST_STOP_APP);
}

static u2ps_host_stats_t stats(void)
{
  u2ps_host_stats_t st;
  u2ps_host_stats(&st);
  return st;
}

/* What the device received is exactly what the host reported sent: the
 * bytes in order, and each packet in its on_sent's microframe */
static void check_delivery(void)
{
  CHECK_EQ(hsink.bad, 0);
  CHECK_EQ(hsink.bytes, hplayer.bytes_sent);
  CHECK_EQ(hsink.packets, hplayer.sent);
  unsigned n = hsink.packets < HPLAYER_LOG ? hsink.packets : HPLAYER_LOG;
  unsigned wrong = 0;
  for (unsigned i = 0; i < n; i++)
    wrong += hsink.uframe[i] != hplayer.sent_uframe[i];
  CHECK_EQ(wrong, 0);
}

/* Slots of packets [a, b) */
static uint64_t slots(unsigned a, unsigned b)
{
  uint64_t n = 0;
  for (unsigned i = a; i < b && i < HPLAYER_LOG; i++)
    n += hplayer.log[i];
  return n;
}

/* Packets [a, b) at the rate ff (16.16): each INT(ff) or INT(ff) + 1
 * slots, and their sum within one slot of (b - a) * ff */
static void check_rate(unsigned a, unsigned b, uint32_t ff)
{
  unsigned odd = 0;
  for (unsigned i = a; i < b; i++)
    odd += hplayer.log[i] != (ff >> 16) && hplayer.log[i] != (ff >> 16) + 1u;
  CHECK_EQ(odd, 0);
  int64_t d = (int64_t)(slots(a, b) << 16) - (int64_t)(b - a) * ff;
  CHECK(d > -(1 << 16) && d < (1 << 16));
  if (odd != 0u || !(d > -(1 << 16) && d < (1 << 16)))
    printf("  packets %u..%u at 0x%x: %llu slots\n", a, b, ff, (unsigned long long)slots(a, b));
}

/* ---- Every sampling frequency ------------------------------------------------------- */

static void test_frequencies(void)
{
  if (!test_desc_load())
    return;
  u2ps_dev_description_t d = test_desc;
  if (!connect_device(&d))
    return;
  for (unsigned k = 0; k < TEST_RATE_COUNT; k++) {
    uint32_t hz = test_rates[k];
    if (!play(3, hz)) /* 32/4, the largest packets */
      continue;
    u2ps_hfake_uframes(8000);
    uint32_t ff = u2ps_fake_fb;
    CHECK_EQ(ff, (uint32_t)((((uint64_t)hz << 16) + 4000u) / 8000u));
    /* The first feedback comes at the stream's first poll; the depth of
     * the queue was latched at the nominal rate before */
    check_rate(8, hplayer.packets, ff);
    CHECK(hplayer.packets >= 8000u);
    CHECK_EQ(app_log.nholes, 0);
    CHECK_EQ(app_log.npauses, 0);
    CHECK_EQ(happ_log.nholes, 0);
    CHECK_EQ(hplayer.refused, 0);
    stop();
    check_delivery();
    if (hsink.bad != 0u)
      printf("  at %u Hz\n", (unsigned)hz);
  }
  u2ps_host_stats_t st = stats();
  CHECK_EQ(st.fb_rejected, 0);
  CHECK_EQ(st.slots_clamped, 0);
  CHECK_EQ(st.uframe_back, 0);
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* ---- Holes ------------------------------------------------------------------------- */

/* Runs until the port's queue is empty with the player off */
static void starve(void)
{
  hplayer.on = false;
  for (unsigned i = 0; i < 64u && u2ps_hfake_queued > 0u; i++)
    u2ps_hfake_uframes(1);
}

/* The player queues again (thread) */
static void resume(void)
{
  hplayer.on = true;
  hplayer_fill();
}

/* Packets of 7 slots from 'from' on, all others 6 */
static unsigned sevens(unsigned from)
{
  unsigned n = 0, other = 0;
  for (unsigned i = from; i < hplayer.packets && i < HPLAYER_LOG; i++) {
    n += hplayer.log[i] == 7u;
    other += hplayer.log[i] != 7u && hplayer.log[i] != 6u;
  }
  CHECK_EQ(other, 0);
  return n;
}

/* One LATE microframe at 48 kHz */
static void test_late(void)
{
  if (!play(3, 48000))
    return;
  u2ps_hfake_uframes(400);
  unsigned sent = happ_log.sent, packets = hplayer.packets;
  u2ps_uframe_t x = u2ps_hfake_uframe_now;
  u2ps_hfake_inject_late(1);
  u2ps_hfake_uframes(1);
  CHECK_EQ(happ_log.sent, sent);
  u2ps_hfake_uframes(1);
  /* The host: LATE 1, before the on_sent of the packet, which went out in
   * X + 1 */
  CHECK_EQ(happ_log.nholes, 1);
  CHECK_EQ(happ_log.holes[0].uframes, 1);
  CHECK_EQ(happ_log.holes[0].why, U2PS_MISS_LATE);
  CHECK_EQ(happ_log.holes[0].sent_before, sent);
  CHECK_EQ(happ_log.sent, sent + 1u);
  CHECK_EQ(happ_log.sent_uframe, u2ps_uframe_add(x, 1));
  /* The device: NO_PACKET 1 at X */
  CHECK_EQ(app_log.nholes, 1);
  CHECK_EQ(app_log.holes[0].uframes, 1);
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_PACKET);
  CHECK_EQ(app_log.packet_uframe, u2ps_uframe_add(x, 1));
  u2ps_hfake_uframes(800);
  /* 6 slots owed, repaid by 6 packets of 7 after the ones latched before */
  CHECK_EQ(sevens(0), 6);
  CHECK_EQ(sevens(packets), 6);
  CHECK_EQ(hplayer.slots, 6u * (hplayer.packets + 1u));
  u2ps_host_stats_t st = stats();
  CHECK_EQ(st.late, 1);
  CHECK_EQ(st.slots_forgiven, 0);
  stop();
  check_delivery();
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* NO_BUFFER for k microframes at 48 kHz */
static void no_buffer(uint16_t k, unsigned owed, uint32_t forgiven)
{
  if (!play(3, 48000))
    return;
  u2ps_hfake_uframes(400);
  u2ps_host_stats_t st0 = stats();
  starve();
  unsigned sent = happ_log.sent, packets = hplayer.packets;
  u2ps_uframe_t last = happ_log.sent_uframe;
  u2ps_hfake_uframes(k);
  resume();
  u2ps_hfake_uframes(1);
  /* One hole of k, before the next on_sent */
  CHECK_EQ(happ_log.nholes, 1);
  CHECK_EQ(happ_log.holes[0].uframes, k);
  CHECK_EQ(happ_log.holes[0].why, U2PS_MISS_NO_BUFFER);
  CHECK_EQ(happ_log.holes[0].sent_before, sent);
  CHECK_EQ(happ_log.sent_uframe, u2ps_uframe_add(last, (uint16_t)(k + 1u)));
  u2ps_host_stats_t st = stats();
  CHECK_EQ(st.no_buffer - st0.no_buffer, k);
  CHECK_EQ(st.slots_forgiven - st0.slots_forgiven, forgiven);
  /* The device: NO_PACKET k, a pause for 500 */
  if (k > U2PS_GAP_PAUSE) {
    CHECK_EQ(app_log.nholes, 0);
    CHECK_EQ(app_log.npauses, 1);
    CHECK_EQ(app_log.pauses[0], k);
  } else {
    CHECK_EQ(app_log.npauses, 0);
    CHECK_EQ(app_log.nholes, 1);
    CHECK_EQ(app_log.holes[0].uframes, k);
    CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_PACKET);
  }
  /* The debt, capped, repaid by packets of 7 */
  u2ps_hfake_uframes(800);
  CHECK_EQ(sevens(0), owed);
  CHECK_EQ(sevens(packets), owed);
  CHECK_EQ(hplayer.slots + forgiven, 6u * (hplayer.packets + k));
  stop();
  check_delivery();
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static void test_no_buffer(void)
{
  no_buffer(1, 6, 0);
  no_buffer(5, 30, 0);
  /* 3,000 slots owed; 8 microframes' worth, 48, repaid; 2,952 forgiven */
  no_buffer(500, 48, 2952);
}

/* A drain of k microframes at 48 kHz */
static void drain(uint16_t k)
{
  if (!play(3, 48000))
    return;
  u2ps_hfake_uframes(400);
  u2ps_host_stats_t st0 = stats();
  u2ps_host_drain(happ_log.info.stream);
  starve();
  u2ps_hfake_uframes(k);
  resume();
  u2ps_hfake_uframes(800);
  CHECK_EQ(happ_log.nholes, 0);
  u2ps_host_stats_t st = stats();
  CHECK_EQ(st.drained - st0.drained, k);
  CHECK_EQ(st.no_buffer - st0.no_buffer, 0);
  CHECK_EQ(st.slots_forgiven - st0.slots_forgiven, 0);
  /* The accumulator did not move: no packet of 7 */
  CHECK_EQ(sevens(0), 0);
  CHECK_EQ(hplayer.slots, 6u * hplayer.packets);
  /* The device: a NO_PACKET hole of k, or a pause */
  if (k > U2PS_GAP_PAUSE) {
    CHECK_EQ(app_log.npauses, 1);
    CHECK_EQ(app_log.pauses[0], k);
  } else {
    CHECK_EQ(app_log.nholes, 1);
    CHECK_EQ(app_log.holes[0].uframes, k);
    CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_PACKET);
  }
  /* A second drain, and the stream goes on as before */
  u2ps_host_drain(happ_log.info.stream);
  starve();
  u2ps_hfake_uframes(3);
  resume();
  u2ps_hfake_uframes(80);
  CHECK_EQ(stats().drained - st0.drained, k + 3u);
  CHECK_EQ(happ_log.nholes, 0);
  CHECK_EQ(sevens(0), 0);
  stop();
  check_delivery();
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static void test_drain(void)
{
  drain(5);
  drain(500);
}

/* ---- The feedback stepped ------------------------------------------------------------ */

static void test_feedback_steps(void)
{
  if (!play(3, 48000))
    return;
  /* 6.0, then up and down within +-12.5 %, the edges included */
  static const uint32_t steps[] = {0x60000, 0x64CCD, 0x59999, 0x6C000, 0x54000, 0x60001};
  for (unsigned k = 0; k < sizeof steps / sizeof steps[0]; k++) {
    u2ps_dev_fb_set(steps[k]);
    u2ps_hfake_uframes(32);
    unsigned a = hplayer.packets;
    u2ps_hfake_uframes(8000);
    check_rate(a, hplayer.packets, steps[k]);
  }
  /* Beyond the window: rejected, the last rate stays */
  u2ps_host_stats_t st0 = stats();
  u2ps_dev_fb_set(0x6C001);
  u2ps_hfake_uframes(32);
  unsigned a = hplayer.packets;
  u2ps_hfake_uframes(8000);
  check_rate(a, hplayer.packets, 0x60001);
  CHECK_EQ(stats().fb_rejected - st0.fb_rejected, 1004);
  stop();
  check_delivery();
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* ---- A tight wMaxPacketSize ----------------------------------------------------------- */

/* A device of 96 kHz only: its packets carry 13 slots (12 + 1), so a
 * feedback of 13.5 at the window's top asks for packets of 14 */
static void test_tight(void)
{
  static const uint32_t r96[1] = {96000};
  u2ps_dev_description_t d = test_desc;
  d.sampling_frequencies = r96;
  d.sampling_frequency_count = 1;
  if (!connect_device(&d) || !play(3, 96000))
    return;
  CHECK_EQ(happ_log.info.max_packet_size, 13u * 8u);
  u2ps_dev_fb_set(0xD8000); /* 13.5 */
  u2ps_hfake_uframes(32);
  u2ps_host_stats_t st0 = stats();
  unsigned a = hplayer.packets;
  u2ps_hfake_uframes(8000);
  unsigned b = hplayer.packets;
  uint32_t clamped = stats().slots_clamped - st0.slots_clamped;
  unsigned over = 0;
  for (unsigned i = a; i < b; i++)
    over += hplayer.log[i] != 13u;
  CHECK_EQ(over, 0);
  CHECK(clamped > 0u);
  int64_t dd = (int64_t)((slots(a, b) + clamped) << 16) - (int64_t)(b - a) * 0xD8000;
  CHECK(dd > -(1 << 16) && dd < (1 << 16));
  for (unsigned i = 0; i < hsink.packets && i < HPLAYER_LOG; i++)
    CHECK(hsink.len[i] <= 13u * 8u);
  stop();
  check_delivery();
  CHECK_EQ(u2ps_hfake_violations, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* ---- Adaptive and synchronous: the nominal rate exactly ---------------------------------- */

static void test_nominal(void)
{
  for (uint8_t alt = 2; alt <= 3u; alt++) {
    for (unsigned k = 0; k < TEST_RATE_COUNT; k++) {
      uint32_t hz = test_rates[k];
      /* 24/3 stereo in 582 bytes: 97 slots, enough for 768 kHz */
      if (!hstart("uac2-hs-sync-types.txt") || !henumerate())
        return;
      hplayer_start();
      hsink_clear();
      u2ps_hfake_bus_out = hsink_packet;
      CHECK(u2ps_host_stream_start(alt, hz));
      CHECK(u2ps_hfake_wait(stream_decided, 100));
      CHECK_EQ(happ_log.stream_starts, 1);
      CHECK_EQ(u2ps_hfake_stream.ep_fb, 0);
      CHECK(!happ_log.info.explicit_feedback);
      u2ps_hfake_uframes(8000);
      /* After n packets, INT(n * fs / 8000) slots */
      unsigned bad = 0;
      uint64_t sum = 0;
      for (unsigned n = 1; n <= hplayer.packets && n <= HPLAYER_LOG; n++) {
        sum += hplayer.log[n - 1u];
        bad += sum != (uint64_t)n * hz / 8000u;
      }
      CHECK_EQ(bad, 0);
      if (hz == 44100u) {
        /* 441 x k after 80 x k */
        unsigned off = 0;
        for (unsigned j = 1; 80u * j <= hplayer.packets; j++)
          off += slots(0, 80u * j) != 441u * j;
        CHECK_EQ(off, 0);
      }
      CHECK(hplayer.packets >= 8000u);
      CHECK_EQ(stats().fb_polls, 0);
      CHECK_EQ(hsink.bad, 0);
      CHECK_EQ(u2ps_hfake_violations, 0);
      if (bad != 0u)
        printf("  alternate setting %u at %u Hz\n", alt, (unsigned)hz);
    }
  }
}

int main(void)
{
  test_frequencies();
  test_late();
  test_no_buffer();
  test_drain();
  test_feedback_steps();
  test_tight();
  test_nominal();
  return check_done();
}
