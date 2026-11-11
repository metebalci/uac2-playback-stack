/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_player.c: the player app of player_app.c, tested on the PC with the
 * test kit's scripted device (u2ps_testkit_host.h): the device is found,
 * the app starts its stream from its poll, every byte it queued and the
 * stack sent arrives intact, a LATE and a CORRUPTED hole reach the app,
 * the device leaves; the callbacks in order, and no contract violation.
 * Copy it for your own player app.
 */
#include <stdio.h>

#include "player_app.h"
#include "u2ps_testkit_host.h"

#define CHECK U2PS_TESTKIT_CHECK

/* The device checks every byte it receives */
static uint64_t received_bytes;
static unsigned bad_bytes;

static void device_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  (void)uframe;
  for (unsigned i = 0; i < len; i++)
    bad_bytes += data[i] != player_app_byte(received_bytes + i);
  received_bytes += len;
}

static bool streaming(void)
{
  return player_app_state.streaming;
}

int main(void)
{
  u2ps_testkit_host_reset();
  u2ps_testkit_host_on_packet(device_packet);
  player_app_init();
  CHECK(player_app_state.init_ok);
  u2ps_testkit_set_poll(player_app_poll);

  /* The device is plugged in: enumeration, then the app's start */
  u2ps_testkit_host_connect();
  CHECK(u2ps_testkit_host_run_until(streaming, 2000));
  CHECK(u2ps_testkit_trace_match("host_on_device host_on_stream_start"));
  CHECK(player_app_state.alternate_setting == 1u);
  CHECK(u2ps_testkit_host_sampling_frequency() == 48000u);

  /* 100 ms of audio: everything sent arrives, in order */
  u2ps_testkit_trace_clear();
  u2ps_testkit_host_run_ms(100);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_HOST_ON_SENT) == 800u);
  CHECK(u2ps_testkit_host_received()->packets == 800u);
  CHECK(received_bytes == player_app_state.bytes_sent && bad_bytes == 0u);
  CHECK(received_bytes == 800u * 6u * 4u); /* 6 slots of 2 x 16 bits per microframe */

  /* Holes on the bus */
  u2ps_testkit_host_inject_late(2);
  u2ps_testkit_host_run_ms(1);
  CHECK(player_app_state.holes[U2PS_MISS_LATE] == 2u);
  u2ps_testkit_host_inject_corrupted();
  u2ps_testkit_host_run_ms(1);
  CHECK(player_app_state.holes[U2PS_MISS_CORRUPTED] == 1u);
  CHECK(u2ps_testkit_host_received()->corrupted == 1u);

  /* The device leaves */
  u2ps_testkit_trace_clear();
  u2ps_testkit_host_disconnect();
  u2ps_testkit_host_run_ms(1);
  CHECK(u2ps_testkit_trace_match("host_on_stream_stop host_on_end"));
  CHECK(!player_app_state.device && !player_app_state.streaming);

  CHECK(player_app_state.contract_violations == 0u);
  return u2ps_testkit_result();
}
