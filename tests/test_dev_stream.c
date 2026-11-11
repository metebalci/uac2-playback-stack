/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_stream.c: the stream. SET_INTERFACE of the AudioStreaming
 * interface opens a stream with the stream table taken from the description
 * and class memory the app lends; alternate setting 0 closes it, and the
 * close is asynchronous: SET_INTERFACE's status stage comes at once, only
 * the next open waits for on_closed, U2PS_CLOSE_MAX bounds that wait, and
 * a bus reset ends it (u2ps_dport.h). The first feedback equals the value
 * given at open (USB 2.0 5.12.4.2: 16.16 samples per microframe). Packets
 * reach the app where the port put them, a zero-length one as length 0
 * with the stream going on; holes are reported before the packet that ends
 * them, across the wrap of the microframe number at 0x3FFF, and a gap
 * longer than U2PS_GAP_PAUSE is a pause.
 */
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "dev_support.h"
#include "u2ps_uac2.h"

static uint8_t buf[512];
static uint16_t n;

/* The app's events so far, as a string */
static const char *events(void)
{
  app_log.events[app_log.nevents] = '\0';
  return app_log.events;
}

#define CHECK_EVENTS(want)                                         \
  do {                                                             \
    if (strcmp(events(), want) != 0)                               \
      printf("  events \"%s\", want \"%s\"\n", events(), want);    \
    CHECK(strcmp(events(), want) == 0);                            \
  } while (0)

static void clear_events(void)
{
  app_log.nevents = 0;
}

/* Close the stream and open alternate setting 1 again: the next packet has
 * no packet before it */
static void restart_stream(void)
{
  if (set_interface(1, 0) != U2PS_FAKE_HOST_OK)
    printf("restart_stream: alt 0 failed\n");
  u2ps_fake_closed();
  if (set_interface(1, 1) != U2PS_FAKE_HOST_OK)
    printf("restart_stream: alt 1 failed\n");
  clear_events();
}

/* Configured at high speed with the clock at 'rate' */
static void start_at(uint32_t rate)
{
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  if (set_rate(rate) != U2PS_FAKE_HOST_OK)
    printf("start_at: set_rate failed\n");
  clear_events();
  u2ps_fake_clear_log();
}

/* wMaxPacketSize of the data endpoint of alternate setting 'alt' in the
 * configuration the host reads */
static uint16_t desc_mps(uint8_t alt)
{
  if (get_descriptor(U2PS_DESC_CONFIGURATION, 0, 0, 0xFFFF, buf, &n) != U2PS_FAKE_HOST_OK)
    return 0;
  int cur = -1;
  for (unsigned off = 0; off < n && buf[off] != 0u; off += buf[off]) {
    if (buf[off + 1] == U2PS_DESC_INTERFACE)
      cur = buf[off + 2] == 1u ? buf[off + 3] : -1;
    else if (buf[off + 1] == U2PS_DESC_ENDPOINT && cur == alt && buf[off + 2] == 0x01u)
      return (uint16_t)(buf[off + 4] | buf[off + 5] << 8);
  }
  return 0;
}

/* The stream table comes from the description: every alternate setting
 * opens with its format and the wMaxPacketSize its descriptor shows; the
 * first feedback is the rate's nominal value */
static void test_open(void)
{
  start_at(768000);
  uint16_t want_mps = desc_mps(2);
  CHECK_EQ(want_mps, 776);
  u2ps_fake_clear_log();
  CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_OK);
  CHECK_EVENTS("S");
  CHECK_EQ(app_log.info.alternate_setting, 2);
  CHECK_EQ(app_log.info.format.subslot_size, 4);
  CHECK_EQ(app_log.info.format.bit_resolution, 24);
  CHECK_EQ(app_log.info.sampling_frequency, 768000);
  CHECK_EQ(app_log.info.max_packet_size, 776);
  int o = u2ps_fake_find(U2PS_FAKE_STREAM_OPEN, 0);
  CHECK(o >= 0);
  if (o >= 0)
    CHECK_EQ(u2ps_fake_calls[o].from, U2PS_FAKE_FROM_SETUP);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_OPEN);
  CHECK_EQ(u2ps_fake_stream.ep_out, 1);
  CHECK_EQ(u2ps_fake_stream.ep_fb, 1);
  CHECK_EQ(u2ps_fake_stream.fb_interval, 4);
  CHECK_EQ(u2ps_fake_stream.fb_size, 4);
  CHECK_EQ(u2ps_fake_stream.mps, 776);
  CHECK(u2ps_fake_stream.mem == app_mem);
  CHECK_EQ(u2ps_fake_stream.stride, 776);
  CHECK_EQ(u2ps_fake_stream.count, 4);
  /* First feedback: 768 kHz is 96 samples per microframe, 0x00600000 */
  CHECK_EQ(u2ps_fake_stream.fb_value, 0x00600000);
  CHECK_EQ(u2ps_fake_fb, 0x00600000);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_FB_SET), 0);

  /* Each alternate setting, at 44.1 kHz: 361267.2 rounds to 0x00058333 */
  for (uint8_t alt = 1; alt <= 4; alt++) {
    start_at(44100);
    uint16_t mps = desc_mps(alt);
    CHECK_EQ(set_interface(1, alt), U2PS_FAKE_HOST_OK);
    CHECK_EQ(app_log.info.alternate_setting, alt);
    CHECK_EQ(app_log.info.format.subslot_size, test_formats[alt - 1].subslot_size);
    CHECK_EQ(app_log.info.format.bit_resolution, test_formats[alt - 1].bit_resolution);
    CHECK_EQ(app_log.info.max_packet_size, mps);
    CHECK_EQ(u2ps_fake_stream.mps, mps);
    CHECK_EQ(u2ps_fake_stream.fb_value, 0x00058333);
    CHECK_EQ(u2ps_fake_fb, 0x00058333);
  }
  /* 48 kHz: exactly 6 samples */
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_fb, 0x00060000);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* The stream info carries the alternate setting's Audio Data Format: an
 * IEEE_FLOAT alternate setting opens as one, with the PCM sizes */
static void test_open_float(void)
{
  static const u2ps_format_t f[3] = {
    {4, 32, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}, {4, 32, U2PS_AUDIO_DATA_FORMAT_PCM},
    {4, 32, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}};
  test_desc.formats = f;
  test_desc.format_count = 3;
  for (uint8_t alt = 1; alt <= 3; alt++) {
    start_at(768000);
    CHECK_EQ(set_interface(1, alt), U2PS_FAKE_HOST_OK);
    CHECK_EVENTS("S");
    CHECK_EQ(app_log.info.alternate_setting, alt);
    CHECK_EQ(app_log.info.format.audio_data_format, f[alt - 1].audio_data_format);
    CHECK_EQ(app_log.info.format.subslot_size, 4);
    CHECK_EQ(app_log.info.format.bit_resolution, 32);
    CHECK_EQ(app_log.info.max_packet_size, 776);
    CHECK_EQ(u2ps_fake_stream.mps, 776);
    CHECK_EQ(u2ps_fake_stream.fb_value, 0x00600000);
  }
  CHECK_EQ(app_log.info.format.audio_data_format, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT);
  CHECK_EQ(u2ps_fake_violations, 0);
  test_desc_load();
}

/* A rate change during a stream sets its nominal feedback; the app's own
 * value goes out as given */
static void test_feedback(void)
{
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_fb, 0x00060000);
  CHECK_EQ(set_rate(96000), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_fb, 0x000C0000);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_FB_SET, 0).from, U2PS_FAKE_FROM_DATA);
  u2ps_fake_clear_log();
  u2ps_dev_fb_set(0x000C0123);
  CHECK_EQ(u2ps_fake_fb, 0x000C0123);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_FB_SET, 0).from, U2PS_FAKE_FROM_THREAD);
  /* The next stream starts at the clock's rate again */
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  u2ps_fake_closed();
  CHECK_EQ(set_interface(1, 3), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_stream.fb_value, 0x000C0000);
  CHECK_EQ(u2ps_fake_fb, 0x000C0000);
  /* Without a stream, a rate change sends no feedback */
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  u2ps_fake_closed();
  u2ps_fake_clear_log();
  CHECK_EQ(set_rate(192000), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_FB_SET), 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Packets reach the app where the port put them; a zero-length packet too,
 * and the stream goes on */
static void test_packets(void)
{
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();

  u2ps_fake_packet(28, 0x0100);
  CHECK_EQ(app_log.packets, 1);
  CHECK(app_log.packet_data == u2ps_fake_last_data);
  CHECK(app_log.packet_data == app_mem);
  CHECK_EQ(app_log.packet_len, 28);
  CHECK_EQ(app_log.packet_uframe, 0x0100);
  CHECK_EQ(app_log.packet_first, u2ps_fake_last_data[0]);
  CHECK_EQ(u2ps_fake_held, 0); /* the app released it inside on_packet */

  u2ps_fake_packet(24, 0x0101);
  /* the next buffer: the stride is the app's, for the mps of the highest
   * rate, 388 */
  CHECK_EQ(app_log.cfg.stride, 388);
  CHECK(app_log.packet_data == app_mem + 388);
  /* A zero-length packet: length 0, no hole, no stop */
  u2ps_fake_packet(0, 0x0102);
  CHECK_EQ(app_log.packets, 3);
  CHECK_EQ(app_log.packet_len, 0);
  CHECK_EQ(app_log.packet_uframe, 0x0102);
  u2ps_fake_packet(0, 0x0103);
  u2ps_fake_packet(28, 0x0104);
  CHECK_EQ(app_log.packets, 5);
  CHECK_EQ(app_log.packet_len, 28);
  CHECK_EVENTS("PPPPP");
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_OPEN);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_CLOSE), 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* The app holds packets and releases them later, from thread. The port
 * keeps U2PS_DPORT_ISO_OUT_RESERVE buffers armed (1 in the fake), so the
 * app holds at most count - 1: a packet that comes while it does is a
 * reserve packet, a NO_BUFFER hole of its own microframe, reported before
 * the next packet */
static void test_release(void)
{
  CHECK_EQ(U2PS_DPORT_ISO_OUT_RESERVE, 1);
  start_at(48000);
  app_cfg.release_each = false;
  app_cfg.count = 3;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();

  u2ps_fake_packet(28, 10);
  u2ps_fake_packet(0, 11); /* a zero-length packet is held too */
  CHECK_EQ(u2ps_fake_held, 2);
  u2ps_fake_packet(28, 12); /* reserve packets: NO_BUFFER */
  u2ps_fake_packet(28, 13);
  CHECK_EQ(u2ps_fake_held, 2);
  CHECK_EQ(app_log.packets, 2);
  CHECK_EQ(app_log.nholes, 0); /* before the packet that ends the hole */
  u2ps_dev_release(app_log.info.stream, 2);
  CHECK_EQ(u2ps_fake_held, 0);
  /* from thread the stack masks the port's vectors around the release */
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_RELEASE, 0).from, U2PS_FAKE_FROM_MASKED);
  u2ps_fake_packet(28, 14);
  CHECK_EVENTS("PPHP");
  CHECK_EQ(app_log.nholes, 1);
  CHECK_EQ(app_log.holes[0].uframes, 2);
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_BUFFER);
  u2ps_dev_release(app_log.info.stream, 1);
  CHECK_EQ(u2ps_fake_held, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Holes come before the packet that ends them, one call per reason, also
 * across the wrap of the microframe number */
static void test_holes(void)
{
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();

  u2ps_fake_packet(28, 100);
  u2ps_fake_missed(3, U2PS_MISS_NO_PACKET);
  CHECK_EQ(app_log.nholes, 0); /* not before the packet that ends it */
  u2ps_fake_packet(28, 104);
  CHECK_EVENTS("PHP");
  CHECK_EQ(app_log.holes[0].uframes, 3);
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_PACKET);

  /* One hole, split into reports with different reasons */
  clear_events();
  u2ps_fake_missed(2, U2PS_MISS_LATE);
  u2ps_fake_missed(1, U2PS_MISS_NO_PACKET);
  u2ps_fake_missed(1, U2PS_MISS_CORRUPTED);
  u2ps_fake_packet(28, 109);
  CHECK_EVENTS("HHHP");
  CHECK_EQ(app_log.nholes, 4);
  CHECK_EQ(app_log.holes[1].uframes, 1);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_NO_PACKET);
  CHECK_EQ(app_log.holes[2].uframes, 2);
  CHECK_EQ(app_log.holes[2].why, U2PS_MISS_LATE);
  CHECK_EQ(app_log.holes[3].uframes, 1);
  CHECK_EQ(app_log.holes[3].why, U2PS_MISS_CORRUPTED);

  /* Across the wrap: 0x3FFC, 0x3FFD, then 0x3FFE 0x3FFF 0x0000 0x0001
   * missing, then 0x0002 */
  restart_stream();
  app_log.nholes = 0;
  u2ps_fake_packet(28, 0x3FFC);
  u2ps_fake_packet(28, 0x3FFD);
  u2ps_fake_missed(4, U2PS_MISS_NO_PACKET);
  u2ps_fake_packet(28, 0x0002);
  CHECK_EVENTS("PPHP");
  CHECK_EQ(app_log.holes[0].uframes, 4);
  CHECK_EQ(app_log.npauses, 0);
  /* Consecutive across the wrap: no hole */
  restart_stream();
  u2ps_fake_packet(28, 0x3FFE);
  u2ps_fake_packet(28, 0x3FFF);
  u2ps_fake_packet(28, 0x0000);
  u2ps_fake_packet(28, 0x0001);
  CHECK_EVENTS("PPPP");
  CHECK_EQ(app_log.npauses, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A gap longer than U2PS_GAP_PAUSE (400 microframes) is a pause, without
 * its holes; one of 400 is a hole */
static void test_pause(void)
{
  CHECK_EQ(U2PS_GAP_PAUSE, 400);
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();

  u2ps_fake_packet(28, 1000);
  u2ps_fake_missed(400, U2PS_MISS_NO_PACKET);
  u2ps_fake_packet(28, 1401);
  CHECK_EVENTS("PHP");
  CHECK_EQ(app_log.holes[0].uframes, 400);
  CHECK_EQ(app_log.npauses, 0);

  clear_events();
  u2ps_fake_missed(401, U2PS_MISS_NO_PACKET);
  u2ps_fake_packet(28, 1803);
  CHECK_EVENTS("ZP");
  CHECK_EQ(app_log.pauses[0], 401);
  CHECK_EQ(app_log.nholes, 1);

  /* Across the wrap: from 0x3F00 to 0x0100 is a gap of 0x1FF */
  restart_stream();
  u2ps_fake_packet(28, 0x3F00);
  u2ps_fake_missed(0x1FF, U2PS_MISS_NO_PACKET);
  u2ps_fake_packet(28, 0x0100);
  CHECK_EVENTS("PZP");
  CHECK_EQ(app_log.pauses[1], 0x1FF);
  /* ... and 400 across the wrap is still a hole */
  restart_stream();
  u2ps_fake_packet(28, 0x3FF0);
  u2ps_fake_missed(400, U2PS_MISS_LATE);
  u2ps_fake_packet(28, (0x3FF0 + 401) & 0x3FFF);
  CHECK_EVENTS("PHP");
  CHECK_EQ(app_log.holes[1].uframes, 400);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_LATE);

  /* A port that does not know the microframes: the reported holes decide */
  restart_stream();
  u2ps_fake_packet(28, U2PS_UFRAME_UNKNOWN);
  u2ps_fake_missed(300, U2PS_MISS_NO_PACKET);
  u2ps_fake_missed(100, U2PS_MISS_LATE);
  u2ps_fake_packet(28, U2PS_UFRAME_UNKNOWN);
  CHECK_EVENTS("PHHP");
  /* Only NO_PACKET microframes form a pause: 300 + 101 LATE are holes */
  u2ps_fake_missed(300, U2PS_MISS_NO_PACKET);
  u2ps_fake_missed(101, U2PS_MISS_LATE);
  u2ps_fake_packet(28, U2PS_UFRAME_UNKNOWN);
  CHECK_EVENTS("PHHPHHP");
  u2ps_fake_missed(401, U2PS_MISS_NO_PACKET);
  u2ps_fake_missed(5, U2PS_MISS_LATE);
  u2ps_fake_packet(28, U2PS_UFRAME_UNKNOWN);
  CHECK_EVENTS("PHHPHHPZHP");
  CHECK_EQ(app_log.pauses[2], 401);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Alternate setting 0 closes; the stream stops at on_closed */
static void test_close(void)
{
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  u2ps_fake_clear_log();
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_CLOSE), 1);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_STREAM_CLOSE, 0).from, U2PS_FAKE_FROM_SETUP);
  CHECK_EQ(app_log.stops, 0);
  /* Packets until on_closed still reach the app */
  u2ps_fake_packet(28, 50);
  CHECK_EQ(app_log.packets, 1);
  u2ps_fake_closed();
  CHECK_EVENTS("PX");
  /* alt 0 again: nothing to close */
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_CLOSE), 1);

  /* SET_CONFIGURATION closes too, also to the same configuration */
  CHECK_EQ(set_interface(1, 4), U2PS_FAKE_HOST_OK);
  clear_events();
  CHECK_EQ(set_configuration(1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_CLOSING);
  u2ps_fake_closed();
  CHECK_EVENTS("X");
  CHECK_EQ(set_interface(1, 4), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_configuration(0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_CLOSING);
  u2ps_fake_closed();

  /* A new alternate setting while open: close, then open at on_closed,
   * where SET_INTERFACE's status completes */
  dev_configure();
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_CLOSING);
  CHECK_EVENTS("");
  u2ps_fake_closed();
  CHECK_EVENTS("XS");
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.info.alternate_setting, 2);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_OPEN);
  /* The same alternate setting again: closed and opened again */
  clear_events();
  CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_NAK);
  u2ps_fake_closed();
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EVENTS("XS");
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* SET_INTERFACE during a close: the open waits for on_closed, and its
 * status stage for the open (alternate setting 0 at once); the bound
 * U2PS_CLOSE_MAX; a bus reset ending it */
static void test_set_interface_during_close(void)
{
  uint8_t alt;

  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  clear_events();
  u2ps_fake_clear_log();
  /* The status stage waits; no open while the close is pending */
  CHECK_EQ(set_interface(1, 3), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), 0);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_EP0_STATUS), 0);
  /* The host changes its mind twice; the last one opens */
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_interface(1, 4), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), 0);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_CLOSE), 0);
  u2ps_fake_closed();
  CHECK_EVENTS("XS");
  CHECK_EQ(app_log.info.alternate_setting, 4);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), 1);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_STREAM_OPEN, 0).from, U2PS_FAKE_FROM_STREAM);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_host_control(0x81, U2PS_GET_INTERFACE, 0, 1, 1, &alt, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(alt, 4);

  /* alt 0 while the close is pending: nothing opens at on_closed */
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  clear_events();
  u2ps_fake_closed();
  CHECK_EVENTS("X");
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_IDLE);

  /* The bound: counted from the first tick after the close began; past
   * U2PS_CLOSE_MAX (10 ms), u2ps_dev_tick() returns the fault once, no
   * app callback comes, and the open still waits */
  CHECK_EQ(U2PS_CLOSE_MAX, 10);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_dev_tick(1000), U2PS_FAULT_NONE); /* no close */
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  clear_events();
  u2ps_fake_clear_log();
  CHECK_EQ(u2ps_dev_tick(2000), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_dev_tick(2005), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_dev_tick(2010), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_dev_tick(2011), U2PS_FAULT_CLOSE);
  CHECK_EQ(u2ps_dev_tick(2050), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_dev_tick(3000), U2PS_FAULT_NONE);
  /* Each tick masks and unmasks the port's vectors, from thread */
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_IRQ_MASK), 12);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_CLOSING);
  CHECK_EQ(set_interface(1, 3), U2PS_FAKE_HOST_NAK); /* the open still waits */
  u2ps_fake_closed();
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EVENTS("XS");
  CHECK_EQ(app_log.info.alternate_setting, 3);
  /* The next close is measured afresh */
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_dev_tick(5000), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_dev_tick(5010), U2PS_FAULT_NONE);
  u2ps_fake_closed();
  CHECK_EQ(u2ps_dev_tick(5100), U2PS_FAULT_NONE);
  /* Without ticks there is no bound: no fault, however long the close */
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  u2ps_fake_closed();
  CHECK_EQ(app_log.faults[U2PS_FAULT_CLOSE], 0);

  /* A bus reset ends a pending close; no on_closed follows, nothing opens,
   * and after it a stream opens without waiting */
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_dev_tick(9000), U2PS_FAULT_NONE);
  clear_events();
  u2ps_fake_clear_log();
  unsigned resets = app_log.resets;
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EVENTS("XDB"); /* stop, unconfigured, reset */
  CHECK_EQ(app_log.resets, resets + 1);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), 0);
  CHECK_EQ(u2ps_dev_tick(9100), U2PS_FAULT_NONE);
  CHECK_EQ(u2ps_dev_tick(9200), U2PS_FAULT_NONE);
  CHECK_EQ(app_log.faults[U2PS_FAULT_CLOSE], 0);
  dev_configure();
  CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_OK);
  CHECK_EVENTS("XDBCS");
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_OPEN);
  /* A bus reset with a stream open stops it */
  clear_events();
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EVENTS("XDB");
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* on_stream_start comes before the port opens the endpoints, and the port
 * opens them only once it returned U2PS_STREAM_OPEN: inside it no
 * stream_open has been made for this stream and the port has no stream, so
 * no packet can come before the app is ready. Also for a stream that opens
 * from on_closed. */
static void test_start_before_open(void)
{
  start_at(48000);
  unsigned opens = u2ps_fake_count(U2PS_FAKE_STREAM_OPEN);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.starts, 1);
  CHECK_EQ(app_log.state_at_start, U2PS_FAKE_ST_IDLE);
  CHECK_EQ(app_log.opens_at_start, opens);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), opens + 1u);
  /* The next stream, opened by the close's end */
  CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(app_log.starts, 1);
  opens = u2ps_fake_count(U2PS_FAKE_STREAM_OPEN);
  u2ps_fake_closed();
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.starts, 2);
  CHECK_EQ(app_log.state_at_start, U2PS_FAKE_ST_IDLE);
  CHECK_EQ(app_log.opens_at_start, opens);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), opens + 1u);
  /* Refused: nothing opens, and SET_INTERFACE stalls */
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  u2ps_fake_closed();
  app_cfg.decline = true;
  opens = u2ps_fake_count(U2PS_FAKE_STREAM_OPEN);
  clear_events();
  CHECK_EQ(set_interface(1, 3), U2PS_FAKE_HOST_STALL);
  CHECK_EVENTS("S");
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), opens);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_IDLE);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* The app refuses a stream, or the port refuses its class memory:
 * SET_INTERFACE stalls (test_dev_start has the details) */
static void test_refused(void)
{
  start_at(48000);
  app_cfg.decline = true;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_STALL);
  CHECK_EVENTS("S");
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_OPEN), 0);
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_CLOSE), 0);

  app_cfg.decline = false;
  u2ps_fake_open_result = false;
  clear_events();
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_STALL);
  CHECK_EVENTS("SFX");
  CHECK_EQ(app_log.faults[U2PS_FAULT_OPEN], 1);
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_STREAM_CLOSE), 0);

  /* Too few buffers for the port: refused the same way */
  u2ps_fake_open_result = true;
  app_cfg.count = 1;
  clear_events();
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_STALL);
  CHECK_EVENTS("SFX");
  app_cfg.count = 4;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_stream_state, U2PS_FAKE_ST_OPEN);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* The app gives the first feedback value at the start, or keeps the
 * nominal one, rounded to the nearest: 176.4 kHz is 22.05 samples, 1445068.8
 * in 16.16, so 0x00160CCD (rounded down it would be 0x00160CCC) */
static void test_first_feedback(void)
{
  start_at(176400);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.fb_offered, 0x00160CCD);
  CHECK_EQ(u2ps_fake_stream.fb_value, 0x00160CCD);
  CHECK_EQ(u2ps_fake_fb, 0x00160CCD);

  start_at(768000);
  app_cfg.fb_value = 0x00600123;
  CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_OK);
  CHECK_EQ(app_log.fb_offered, 0x00600000);
  CHECK_EQ(u2ps_fake_stream.fb_value, 0x00600123);
  CHECK_EQ(u2ps_fake_fb, 0x00600123);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A deferred rate change during a stream sets the new nominal feedback
 * when the app replies */
static void test_deferred_rate_feedback(void)
{
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_fb, 0x00060000);
  app_cfg.rate_result = U2PS_REQ_DEFER;
  CHECK_EQ(set_rate(96000), U2PS_FAKE_HOST_NAK);
  CHECK_EQ(u2ps_fake_fb, 0x00060000);
  CHECK_EQ(u2ps_dev_reply(app_log.rate_id, NULL, 0), true);
  CHECK_EQ(u2ps_fake_host_resume(NULL, NULL), U2PS_FAKE_HOST_OK);
  CHECK_EQ(u2ps_fake_fb, 0x000C0000);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_FB_SET, 0).from, U2PS_FAKE_FROM_MASKED);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Only NO_PACKET microframes form a pause; LATE, NO_BUFFER and CORRUPTED
 * are holes however long, also next to a pause */
static void test_pause_reasons(void)
{
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  u2ps_fake_packet(28, 1000);
  u2ps_fake_missed(401, U2PS_MISS_NO_BUFFER);
  u2ps_fake_packet(28, 1402);
  u2ps_fake_missed(500, U2PS_MISS_LATE);
  u2ps_fake_packet(28, 1903);
  u2ps_fake_missed(1000, U2PS_MISS_CORRUPTED);
  u2ps_fake_packet(28, 2904);
  CHECK_EVENTS("PHPHPHP");
  CHECK_EQ(app_log.npauses, 0);
  CHECK_EQ(app_log.holes[0].uframes, 401);
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_BUFFER);
  CHECK_EQ(app_log.holes[1].uframes, 500);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_LATE);
  CHECK_EQ(app_log.holes[2].uframes, 1000);
  CHECK_EQ(app_log.holes[2].why, U2PS_MISS_CORRUPTED);
  /* A run of 450 reserve packets, one NO_BUFFER microframe each, while
   * the app holds the three of four buffers it may: a hole, no pause */
  clear_events();
  app_log.nholes = 0;
  app_cfg.release_each = false;
  restart_stream();
  unsigned packets = app_log.packets;
  for (unsigned u = 0; u < 3u + 450u; u++)
    u2ps_fake_packet(28, (u2ps_uframe_t)(3000u + u));
  CHECK_EQ(app_log.packets, packets + 3u);
  u2ps_dev_release(app_log.info.stream, 3);
  u2ps_fake_packet(28, 3453);
  CHECK_EQ(app_log.npauses, 0);
  CHECK_EQ(app_log.nholes, 1);
  CHECK_EQ(app_log.holes[0].uframes, 450);
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_BUFFER);
  /* NO_PACKET beyond the bound next to LATE: a pause of the NO_PACKET
   * part and a hole for LATE */
  u2ps_dev_release(app_log.info.stream, 1);
  clear_events();
  u2ps_fake_missed(10, U2PS_MISS_LATE);
  u2ps_fake_packet(28, (u2ps_uframe_t)(3453u + 412u));
  CHECK_EVENTS("ZHP");
  CHECK_EQ(app_log.pauses[0], 401);
  CHECK_EQ(app_log.holes[1].uframes, 10);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* The microframe gap and the port's reports disagree: the gap decides how
 * many microframes passed; those no other reason accounts for are
 * NO_PACKET. Across the wrap too, without any report. */
static void test_gap_and_reports(void)
{
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  app_log.nholes = 0;
  u2ps_fake_packet(28, 100);
  u2ps_fake_packet(28, 106); /* 5 missing, none reported */
  u2ps_fake_missed(3, U2PS_MISS_NO_PACKET);
  u2ps_fake_packet(28, 112); /* 5 missing, 3 reported */
  u2ps_fake_missed(2, U2PS_MISS_LATE);
  u2ps_fake_packet(28, 116); /* 3 missing, 2 of them LATE */
  u2ps_fake_missed(4, U2PS_MISS_LATE);
  u2ps_fake_packet(28, 119); /* 2 missing, 4 LATE reported */
  u2ps_fake_missed(7, U2PS_MISS_NO_PACKET);
  u2ps_fake_packet(28, 120); /* none missing, 7 NO_PACKET reported */
  CHECK_EVENTS("PHPHPHHPHPP");
  CHECK_EQ(app_log.nholes, 5);
  CHECK_EQ(app_log.holes[0].uframes, 5);
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_PACKET);
  CHECK_EQ(app_log.holes[1].uframes, 5);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_NO_PACKET);
  CHECK_EQ(app_log.holes[2].uframes, 1);
  CHECK_EQ(app_log.holes[2].why, U2PS_MISS_NO_PACKET);
  CHECK_EQ(app_log.holes[3].uframes, 2);
  CHECK_EQ(app_log.holes[3].why, U2PS_MISS_LATE);
  CHECK_EQ(app_log.holes[4].uframes, 4);
  CHECK_EQ(app_log.holes[4].why, U2PS_MISS_LATE);
  /* Across the wrap without a report: 0x3FFD, then 0x0002 */
  restart_stream();
  app_log.nholes = 0;
  u2ps_fake_packet(28, 0x3FFD);
  u2ps_fake_packet(28, 0x0002);
  CHECK_EVENTS("PHP");
  CHECK_EQ(app_log.holes[0].uframes, 4);
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_PACKET);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A step back, or the same microframe again, is no gap and is counted;
 * then the reports decide, as for a gap of 1.024 s or more, which the
 * signed distance cannot tell from a step back */
static void test_back_step(void)
{
  u2ps_dev_stats_t st;

  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  u2ps_fake_packet(28, 100);
  u2ps_fake_packet(28, 99);
  u2ps_fake_packet(28, 99);
  CHECK_EVENTS("PPP");
  u2ps_dev_stats(&st);
  CHECK_EQ(st.uframe_back, 2);
  /* 9000 microframes later: the distance is negative; the port's report of
   * 8999 NO_PACKET microframes makes it a pause */
  u2ps_fake_missed(8999, U2PS_MISS_NO_PACKET);
  u2ps_fake_packet(28, 9099);
  CHECK_EVENTS("PPPZP");
  CHECK_EQ(app_log.pauses[0], 8999);
  u2ps_dev_stats(&st);
  CHECK_EQ(st.uframe_back, 3);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Holes reported after the stream's last packet, during its close, reach
 * the app before on_stream_stop and are not carried into the next
 * stream */
static void test_close_hole_not_carried(void)
{
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  u2ps_fake_packet(28, 10);
  CHECK_EQ(set_interface(1, 2), U2PS_FAKE_HOST_NAK);
  u2ps_fake_missed(5, U2PS_MISS_NO_PACKET);
  u2ps_fake_missed(2, U2PS_MISS_LATE);
  clear_events();
  app_log.nholes = 0;
  u2ps_fake_closed();
  CHECK_EVENTS("HHXS");
  CHECK_EQ(app_log.holes[0].uframes, 5);
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_PACKET);
  CHECK_EQ(app_log.holes[1].uframes, 2);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_LATE);
  u2ps_fake_packet(28, 500);
  u2ps_fake_packet(28, 501);
  CHECK_EVENTS("HHXSPP");
  CHECK_EQ(app_log.nholes, 2);
  CHECK_EQ(app_log.npauses, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}


/* Streams are numbered; a release names its stream. A release for a stream
 * that is gone, of more packets than are held, or with no stream open is
 * ignored and counted, and never frees a newer stream's packets. */
static void test_release_streams(void)
{
  u2ps_dev_stats_t st;

  start_at(48000);
  app_cfg.release_each = false;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  /* Two of the releases below break the contract (more than held, and a
   * number no start gave); the releases from thread of a stream that has
   * stopped do not count, as the interrupt may have stopped it just
   * before */
  contract_expect(true);
  uint32_t first = app_log.info.stream;
  CHECK(first != 0u);
  u2ps_fake_packet(28, 10);
  u2ps_fake_packet(28, 11);
  CHECK_EQ(u2ps_fake_held, 2);
  /* More than held: ignored */
  u2ps_dev_release(first, 3);
  CHECK_EQ(u2ps_fake_held, 2);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_RELEASE), 0);
  /* A bus reset ends the stream; a new one holds 2 packets */
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  uint32_t second = app_log.info.stream;
  CHECK(second != first);
  u2ps_fake_packet(28, 20);
  u2ps_fake_packet(28, 21);
  CHECK_EQ(u2ps_fake_held, 2);
  /* The late release of the first stream's packets frees nothing */
  u2ps_dev_release(first, 2);
  CHECK_EQ(u2ps_fake_held, 2);
  u2ps_dev_release(second, 2);
  CHECK_EQ(u2ps_fake_held, 0);
  /* With no stream open: after the close ended */
  u2ps_fake_packet(28, 22);
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  u2ps_dev_release(second, 1); /* still open until on_closed: fine */
  u2ps_fake_packet(28, 23);
  u2ps_fake_closed();
  u2ps_dev_release(second, 1);
  u2ps_dev_release(0, 0);
  u2ps_dev_stats(&st);
  CHECK_EQ(st.release_errors, 4);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_RELEASE), 2);
  CHECK_EQ(u2ps_fake_violations, 0);
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_DEV_RELEASE_COUNT], CONTRACT_N(1));
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_DEV_RELEASE_STREAM], CONTRACT_N(1));
  CHECK_EQ(contract_log.total, CONTRACT_N(2));
  contract_expect(false);
}

/* The counters: the port's, as the fake reports them, and the stack's */
static void test_stats(void)
{
  u2ps_dev_stats_t st;

  start_at(48000);
  u2ps_fake_stats = (u2ps_dport_stats_t){7, 6, 5, 4, 3, 2, 1};
  u2ps_fake_clear_log();
  u2ps_dev_stats(&st);
  CHECK_EQ(st.late_completions, 7);
  CHECK_EQ(st.late, 6);
  CHECK_EQ(st.no_buffer, 5);
  CHECK_EQ(st.corrupted, 4);
  CHECK_EQ(st.fb_recoveries, 3);
  CHECK_EQ(st.errors, 2);
  CHECK_EQ(st.fallback_over, 1);
  CHECK_EQ(st.release_errors, 0);
  CHECK_EQ(st.uframe_back, 0);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_STATS, 0).from, U2PS_FAKE_FROM_MASKED);
  /* The library's build constants reach the tests */
  CHECK_EQ(U2PS_EP0_BUF, 512);
  CHECK_EQ(U2PS_ISO_OUT_MPS_MAX, 1024);
  CHECK_EQ(U2PS_ISO_OUT_COUNT_MAX, 16);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* One tick, with the fake's call log cleared so long runs fit it */
static u2ps_fault_t tick(uint32_t now_ms)
{
  u2ps_fake_clear_log();
  return u2ps_dev_tick(now_ms);
}

/* Gaps the microframe numbers cannot count: u2ps_dev_tick() sees no
 * packet for more than 50 ms, and the next packet ends a pause, never a
 * hole */
static void test_long_gap(void)
{
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();

  /* Packets flowing between ticks: no long gap */
  uint32_t t = 1000;
  u2ps_uframe_t u = 100;
  for (unsigned i = 0; i < 20; i++) {
    u2ps_fake_packet(28, u);
    u = (u2ps_uframe_t)(u + 8u);
    u2ps_fake_missed(7, U2PS_MISS_NO_PACKET);
    CHECK_EQ(tick(t), U2PS_FAULT_NONE);
    t += 1;
  }
  CHECK_EQ(app_log.npauses, 0);

  /* 3 s (24000 microframes) with ticks every 10 ms: the microframe gap,
   * 7616 modulo 2^14, is less than the 3000 ms the ticks saw, so the pause
   * has no known length */
  u2ps_fake_packet(28, u);
  clear_events();
  app_log.nholes = 0;
  for (unsigned ms = 0; ms <= 3000; ms += 10)
    tick(t + ms);
  t += 3010;
  u2ps_uframe_t later = (u2ps_uframe_t)((u + 24001u) & 0x3FFFu);
  u2ps_fake_missed(7616, U2PS_MISS_NO_PACKET);
  u2ps_fake_packet(28, later);
  CHECK_EVENTS("ZP");
  CHECK_EQ(app_log.pauses[0], U2PS_PAUSE_LONG);
  CHECK_EQ(app_log.nholes, 0);
  /* The pause ends with that packet: a short gap after it is a hole */
  u2ps_fake_missed(3, U2PS_MISS_NO_PACKET);
  u2ps_fake_packet(28, (u2ps_uframe_t)((later + 4u) & 0x3FFFu));
  CHECK_EVENTS("ZPHP");
  CHECK_EQ(app_log.holes[0].uframes, 3);

  /* 2^14 + 10 microframes (2.05 s): the numbers show a gap of 10; with
   * ticks it is a pause, not a hole of 10 */
  u = (u2ps_uframe_t)((later + 4u) & 0x3FFFu);
  tick(t);
  clear_events();
  for (unsigned ms = 0; ms <= 2050; ms += 25)
    tick(t + ms);
  t += 2100;
  u2ps_fake_missed(10, U2PS_MISS_NO_PACKET);
  u2ps_fake_packet(28, (u2ps_uframe_t)((u + 11u) & 0x3FFFu));
  CHECK_EVENTS("ZP");
  CHECK_EQ(app_log.pauses[1], U2PS_PAUSE_LONG);
  CHECK_EQ(app_log.nholes, 1);
  u = (u2ps_uframe_t)((u + 11u) & 0x3FFFu);

  /* 1000 microframes (125 ms) with ticks over 120 ms of it: the numbers
   * agree with the ticks, so the pause has its length; a LATE report in
   * the same gap stays a hole */
  tick(t);
  clear_events();
  for (unsigned ms = 0; ms <= 120; ms += 10)
    tick(t + ms);
  t += 130;
  u2ps_fake_missed(990, U2PS_MISS_NO_PACKET);
  u2ps_fake_missed(10, U2PS_MISS_LATE);
  u2ps_fake_packet(28, (u2ps_uframe_t)((u + 1001u) & 0x3FFFu));
  CHECK_EVENTS("ZHP");
  CHECK_EQ(app_log.pauses[2], 990);
  CHECK_EQ(app_log.holes[1].uframes, 10);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_LATE);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Without ticks, the documented behaviour: a gap of 2^14 + 10 microframes
 * looks like a hole of 10, and one of 3 s is decided by the port's report
 * (a pause of its 7616 microframes, modulo 2^14) */
static void test_long_gap_without_ticks(void)
{
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  u2ps_fake_packet(28, 200);
  u2ps_fake_missed(10, U2PS_MISS_NO_PACKET);
  u2ps_fake_packet(28, 211);
  CHECK_EVENTS("PHP");
  CHECK_EQ(app_log.holes[0].uframes, 10);
  CHECK_EQ(app_log.npauses, 0);
  u2ps_fake_missed(7616, U2PS_MISS_NO_PACKET);
  u2ps_fake_packet(28, (u2ps_uframe_t)((211u + 24001u) & 0x3FFFu));
  CHECK_EVENTS("PHPZP");
  CHECK_EQ(app_log.pauses[0], 7616);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* The stream's tail: the reserve packets after its last delivered packet,
 * and the microframes without a packet between them, reach the app before
 * on_stream_stop at the close, at a bus reset and at a detach */
static void tail_stream(void)
{
  start_at(48000);
  app_cfg.release_each = false;
  app_cfg.count = 3;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  app_log.nholes = 0;
  app_log.npauses = 0;
  u2ps_fake_packet(28, 100);
  u2ps_fake_packet(28, 101);
  u2ps_fake_packet(28, 102); /* reserve packets in 102 and 105 */
  u2ps_fake_packet(28, 105);
  CHECK_EVENTS("PP");
}

static void check_tail(const char *want)
{
  CHECK_EVENTS(want);
  CHECK_EQ(app_log.nholes, 2);
  CHECK_EQ(app_log.holes[0].uframes, 2); /* 103, 104 */
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_PACKET);
  CHECK_EQ(app_log.holes[1].uframes, 2);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_NO_BUFFER);
  CHECK_EQ(app_log.npauses, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

static void test_tail(void)
{
  /* Alternate setting 0, then on_closed */
  tail_stream();
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  CHECK_EVENTS("PP");
  u2ps_fake_closed();
  check_tail("PPHHX");
  /* A bus reset */
  tail_stream();
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  check_tail("PPHHXDB");
  /* A bus reset during the close */
  tail_stream();
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  check_tail("PPHHXDB");
  /* A detach */
  tail_stream();
  u2ps_dev_connect(false);
  check_tail("PPHHXD");
#if U2PS_VBUS == U2PS_VBUS_APP
  /* A VBUS loss */
  tail_stream();
  u2ps_fake_vbus_set(false);
  check_tail("PPHHXDv");
  u2ps_fake_vbus_set(true);
#endif

  /* A tail of more than U2PS_GAP_PAUSE microframes without a packet is a
   * pause; the delivered packets' own holes stay with them */
  tail_stream();
  u2ps_fake_packet(28, 105 + 402);
  u2ps_fake_missed(3, U2PS_MISS_LATE); /* the port's own report */
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  u2ps_fake_closed();
  CHECK_EVENTS("PPZHHX");
  CHECK_EQ(app_log.pauses[0], 2 + 401);
  CHECK_EQ(app_log.holes[0].uframes, 3);
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_LATE);
  CHECK_EQ(app_log.holes[1].uframes, 3);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_NO_BUFFER);

  /* No packet after the last delivered one: no tail */
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  u2ps_fake_packet(28, 100);
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  u2ps_fake_closed();
  CHECK_EVENTS("PX");
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* Holes longer than 65,535 microframes come in several calls, not
 * clamped: 70,000 reserve packets before a packet, and as the tail */
static void test_hole_split(void)
{
  start_at(48000);
  app_cfg.release_each = false;
  app_cfg.count = 2;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  app_log.nholes = 0;
  u2ps_fake_packet(28, 0);
  for (uint32_t u = 1; u <= 70000u; u++) {
    u2ps_fake_packet(28, (u2ps_uframe_t)(u & 0x3FFFu));
    if (u % 1000u == 0u)
      u2ps_fake_clear_log();
  }
  CHECK_EQ(app_log.packets, 1);
  u2ps_dev_release(app_log.info.stream, 1);
  u2ps_fake_packet(28, (u2ps_uframe_t)(70001u & 0x3FFFu));
  CHECK_EVENTS("PHHP");
  CHECK_EQ(app_log.nholes, 2);
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_BUFFER);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_NO_BUFFER);
  CHECK_EQ((uint32_t)app_log.holes[0].uframes + app_log.holes[1].uframes, 70000);
  CHECK_EQ(app_log.npauses, 0);
  /* The same as the tail */
  for (uint32_t u = 70002; u <= 140001u; u++) {
    u2ps_fake_packet(28, (u2ps_uframe_t)(u & 0x3FFFu));
    if (u % 1000u == 0u)
      u2ps_fake_clear_log();
  }
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  u2ps_fake_closed();
  CHECK_EVENTS("PHHPHHX");
  CHECK_EQ(app_log.nholes, 4);
  CHECK_EQ((uint32_t)app_log.holes[2].uframes + app_log.holes[3].uframes, 70000);
  CHECK_EQ(app_log.holes[2].why, U2PS_MISS_NO_BUFFER);
  CHECK_EQ(app_log.holes[3].why, U2PS_MISS_NO_BUFFER);
  CHECK_EQ(app_log.npauses, 0);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A stall of only NO_BUFFER microframes is no pause, however long the
 * ticks saw no packet (only NO_PACKET microframes form a pause); a host
 * pause inside a stall still is one. With ticks through the stall, its
 * holes come at each tick of the long gap (test_gap_holes_ticking). */
static void test_stall_no_pause(void)
{
  start_at(48000);
  app_cfg.release_each = false;
  app_cfg.count = 3;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  app_log.nholes = 0;
  app_log.npauses = 0;
  /* 100 ms of packets in every microframe, ticks every 10 ms: two
   * delivered, 798 reserve packets, three microframes without a packet;
   * the first long gap at the tick of 1560, then each tick */
  uint32_t t = 1000;
  for (uint32_t u = 0; u < 800u; u++) {
    if (u != 400u && u != 401u && u != 402u)
      u2ps_fake_packet(28, (u2ps_uframe_t)(1000u + u));
    if (u % 80u == 0u)
      tick(t + u / 8u);
  }
  t += 110;
  tick(t);
  u2ps_dev_release(app_log.info.stream, 2);
  u2ps_fake_packet(28, 1800);
  CHECK_EVENTS("PPHHHHHP");
  CHECK_EQ(app_log.npauses, 0);
  CHECK_EQ(app_log.holes[0].uframes, 3);
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_PACKET);
  CHECK_EQ(app_log.holes[1].uframes, 1560 - 1002 - 2);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_NO_BUFFER);
  CHECK_EQ(app_log.hole_total[U2PS_MISS_NO_PACKET], 3);
  CHECK_EQ(app_log.hole_total[U2PS_MISS_NO_BUFFER], 795);

  /* A host pause of 500 microframes inside a stall, with ticks: a pause of
   * its length, and the reserve packets a hole */
  clear_events();
  app_log.nholes = 0;
  u2ps_fake_packet(28, 1801); /* the app now holds 2 */
  for (uint32_t u = 1802; u < 1900u; u++)
    u2ps_fake_packet(28, (u2ps_uframe_t)u);
  for (uint32_t ms = 0; ms <= 70u; ms += 10u)
    tick(t + ms);
  t += 80;
  for (uint32_t u = 2400; u < 2450u; u++)
    u2ps_fake_packet(28, (u2ps_uframe_t)u);
  u2ps_dev_release(app_log.info.stream, 2);
  u2ps_fake_packet(28, 2450);
  CHECK_EVENTS("PHZHP");
  CHECK_EQ(app_log.npauses, 1);
  CHECK_EQ(app_log.pauses[0], 500);
  CHECK_EQ(app_log.holes[0].uframes, 98); /* at the long gap */
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_BUFFER);
  CHECK_EQ(app_log.holes[1].uframes, 50);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_NO_BUFFER);

  /* A host pause of 2^14 + 1 microframes: the numbers show the next
   * packet right after the last, but the ticks saw 2 s: a pause */
  u2ps_dev_release(app_log.info.stream, 1);
  clear_events();
  for (uint32_t ms = 0; ms <= 2050u; ms += 10u)
    tick(t + ms);
  t += 2060;
  u2ps_fake_packet(28, (u2ps_uframe_t)((2450u + 0x4001u) & 0x3FFFu));
  CHECK_EVENTS("ZP");
  CHECK_EQ(app_log.pauses[1], U2PS_PAUSE_LONG);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* ---- Holes at the long gap --------------------------------------------------- */

/* Packets the host sent in this stream */
static uint32_t sent;

static void send(u2ps_uframe_t uframe)
{
  u2ps_fake_packet(28, uframe);
  sent++;
}

/* A stream of 3 buffers whose app holds every packet it may (2): two
 * delivered in 1000 and 1001, then 'reserve' reserve packets from 1002, with
 * no tick meanwhile (a main-loop stall) */
static void gap_stall(uint32_t reserve)
{
  start_at(48000);
  app_cfg.release_each = false;
  app_cfg.count = 3;
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  sent = 0;
  for (uint32_t u = 0; u < 2u + reserve; u++)
    send((u2ps_uframe_t)(1000u + u));
  CHECK_EVENTS("PP");
}

/* The app releases what it holds inside its on_hole */
static void release_at_hole(char event)
{
  if (event == 'H')
    u2ps_dev_release(app_log.info.stream, 2);
}

/* Every packet the host sent is delivered or a NO_BUFFER microframe, once */
static void check_sum(void)
{
  CHECK_EQ(app_log.packets + app_log.hole_total[U2PS_MISS_NO_BUFFER], sent);
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A stall, then the host pauses (nothing sent, alternate setting
 * unchanged), and the main loop ticks again: the first tick that sees the
 * long gap (more than U2PS_GAP_PAUSE / 8 ms without a delivered packet)
 * asks the port for its pending holes and reports them, in thread with the
 * vectors masked, before any on_pause; the packet after the pause brings a
 * pause of only the pause's microframes */
static void test_gap_holes(void)
{
  gap_stall(600);
  uint32_t t = 5000;
  for (uint32_t ms = 0; ms <= 50u; ms += 10u)
    tick(t + ms);
  CHECK_EVENTS("PP"); /* 50 ms: not yet a long gap */
  tick(t + 60);
  CHECK_EVENTS("PPH");
  CHECK_EQ(app_log.nholes, 1);
  CHECK_EQ(app_log.holes[0].uframes, 600);
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_NO_BUFFER);
  CHECK(app_log.hole_masked);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_REPORT_HOLES), 1);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_REPORT_HOLES, 0).from, U2PS_FAKE_FROM_MASKED);
  /* Later ticks of the gap find nothing more */
  tick(t + 70);
  CHECK_EVENTS("PPH");
  /* The app releases; the host sends again 1000 microframes after its last
   * packet: a pause of the 999 without a packet, and no hole again */
  u2ps_dev_release(app_log.info.stream, 2);
  send(1601 + 1000);
  CHECK_EVENTS("PPHZP");
  CHECK_EQ(app_log.npauses, 1);
  CHECK_EQ(app_log.pauses[0], 999);
  CHECK_EQ(app_log.nholes, 1);
  check_sum();

  /* A host pause too short for a pause: a NO_PACKET hole of its 100
   * microframes, the stall's holes already reported. The app releases
   * inside that on_hole, in thread: the release masks nothing more */
  gap_stall(600);
  t += 1000;
  app_event_hook = release_at_hole;
  for (uint32_t ms = 0; ms <= 60u; ms += 10u)
    tick(t + ms);
  app_event_hook = NULL;
  CHECK_EVENTS("PPH");
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_RELEASE), 1);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_RELEASE, 0).from, U2PS_FAKE_FROM_MASKED);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_IRQ_MASK), 2);
  send(1601 + 101);
  CHECK_EVENTS("PPHHP");
  CHECK_EQ(app_log.holes[1].uframes, 100);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_NO_PACKET);
  CHECK_EQ(app_log.npauses, 0);
  check_sum();

  /* Without microframe numbers the port's reports decide: its 999
   * NO_PACKET microframes with the next packet are a pause, of no known
   * length */
  gap_stall(600);
  t += 1000;
  for (uint32_t ms = 0; ms <= 60u; ms += 10u)
    tick(t + ms);
  CHECK_EVENTS("PPH");
  u2ps_dev_release(app_log.info.stream, 2);
  u2ps_fake_missed(999, U2PS_MISS_NO_PACKET);
  send(U2PS_UFRAME_UNKNOWN);
  CHECK_EVENTS("PPHZP");
  CHECK_EQ(app_log.pauses[0], U2PS_PAUSE_LONG);
  check_sum();
}

/* The main loop ticks through the stall (the app holds its buffers on
 * purpose): from the first tick that sees the long gap on, each tick
 * reports what came since the last, so the stall's holes reach the app
 * within a tick; then a host pause of 100 microframes is a hole of its
 * own, and the stall's holes count once */
static void test_gap_holes_ticking(void)
{
  gap_stall(0);
  uint32_t t = 1000;
  unsigned at_gap = 0;
  for (uint32_t u = 2; u < 802u; u++) {
    send((u2ps_uframe_t)(1000u + u));
    if (u % 80u == 0u) {
      tick(t + u / 8u);
      if (at_gap == 0u && app_log.nholes != 0u)
        at_gap = u;
    }
  }
  /* The first long gap: the tick of 1560, 60 ms after the first tick
   * without a new packet (1080); the holes up to it, then those since
   * each tick at each later one */
  CHECK_EQ(at_gap, 560);
  CHECK_EQ(app_log.holes[0].uframes, 559);
  CHECK_EQ(app_log.hole_total[U2PS_MISS_NO_BUFFER], 800 - 1);
  tick(t + 110);
  CHECK_EQ(app_log.hole_total[U2PS_MISS_NO_BUFFER], 800);
  unsigned holes = app_log.nholes;
  /* The host pauses for 100 microframes, the app releases */
  u2ps_dev_release(app_log.info.stream, 2);
  send(1801 + 101);
  CHECK_EQ(app_log.nholes, holes + 1u);
  CHECK_EQ(app_log.holes[holes].uframes, 100);
  CHECK_EQ(app_log.holes[holes].why, U2PS_MISS_NO_PACKET);
  CHECK_EQ(app_log.npauses, 0);
  CHECK_EQ(app_log.hole_total[U2PS_MISS_NO_PACKET], 100);
  check_sum();
}

/* The stream's end after the holes of a long gap: the tail holds only
 * what came after them */
static void test_gap_holes_tail(void)
{
  uint32_t t = 5000;
  /* At the close */
  gap_stall(600);
  for (uint32_t ms = 0; ms <= 60u; ms += 10u)
    tick(t + ms);
  CHECK_EVENTS("PPH");
  for (uint32_t u = 0; u < 50u; u++) /* after 300 microframes without a packet */
    send((u2ps_uframe_t)(1601u + 301u + u));
  CHECK_EQ(set_interface(1, 0), U2PS_FAKE_HOST_OK);
  tick(t + 70); /* the close: no stream open to ask */
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_REPORT_HOLES), 0);
  CHECK_EVENTS("PPH");
  u2ps_fake_closed();
  CHECK_EVENTS("PPHHHX");
  CHECK_EQ(app_log.holes[1].uframes, 300);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_NO_PACKET);
  CHECK_EQ(app_log.holes[2].uframes, 50);
  CHECK_EQ(app_log.holes[2].why, U2PS_MISS_NO_BUFFER);
  check_sum();
  /* At a bus reset, with nothing after the long gap's holes: no tail */
  gap_stall(600);
  t += 1000;
  for (uint32_t ms = 0; ms <= 60u; ms += 10u)
    tick(t + ms);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  CHECK_EVENTS("PPHXDB");
  check_sum();
  /* At a detach */
  gap_stall(600);
  t += 1000;
  for (uint32_t ms = 0; ms <= 60u; ms += 10u)
    tick(t + ms);
  send(1601 + 2);
  u2ps_dev_connect(false);
  CHECK_EVENTS("PPHHHXD");
  CHECK_EQ(app_log.holes[1].uframes, 1);
  CHECK_EQ(app_log.holes[1].why, U2PS_MISS_NO_PACKET);
  CHECK_EQ(app_log.holes[2].uframes, 1);
  CHECK_EQ(app_log.holes[2].why, U2PS_MISS_NO_BUFFER);
  check_sum();
}

/* A long gap with nothing pending reports nothing: a host pause while the
 * app releases every packet; and the stack's own pending reports (a LATE
 * report after the last packet) come at the long gap, not again */
static void test_gap_holes_none(void)
{
  start_at(48000);
  CHECK_EQ(set_interface(1, 1), U2PS_FAKE_HOST_OK);
  clear_events();
  u2ps_fake_packet(28, 3000);
  u2ps_fake_packet(28, 3001);
  uint32_t t = 7000;
  for (uint32_t ms = 0; ms <= 70u; ms += 10u)
    tick(t + ms);
  CHECK_EVENTS("PP");
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_REPORT_HOLES), 1); /* the last tick's */
  u2ps_fake_packet(28, 3001 + 1001);
  CHECK_EVENTS("PPZP");
  CHECK_EQ(app_log.pauses[0], 1000);
  CHECK_EQ(app_log.nholes, 0);

  /* LATE after the last packet, as DWC2 reports a late arm's microframes */
  clear_events();
  u2ps_fake_missed(3, U2PS_MISS_LATE);
  t += 100;
  for (uint32_t ms = 0; ms <= 60u; ms += 10u)
    tick(t + ms);
  CHECK_EVENTS("H");
  CHECK_EQ(app_log.holes[0].uframes, 3);
  CHECK_EQ(app_log.holes[0].why, U2PS_MISS_LATE);
  u2ps_fake_packet(28, 4002 + 1001);
  CHECK_EVENTS("HZP");
  CHECK_EQ(app_log.pauses[1], 997);
  CHECK_EQ(app_log.nholes, 1);
  CHECK_EQ(u2ps_fake_violations, 0);
}

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
  test_open();
  test_open_float();
  test_feedback();
  test_packets();
  test_release();
  test_holes();
  test_pause();
  test_close();
  test_set_interface_during_close();
  test_start_before_open();
  test_refused();
  test_first_feedback();
  test_deferred_rate_feedback();
  test_pause_reasons();
  test_gap_and_reports();
  test_back_step();
  test_close_hole_not_carried();
  test_release_streams();
  test_stats();
  test_long_gap();
  test_long_gap_without_ticks();
  test_tail();
  test_hole_split();
  test_stall_no_pause();
  test_gap_holes();
  test_gap_holes_ticking();
  test_gap_holes_tail();
  test_gap_holes_none();
  return check_done();
}
