/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_player_loopback.c: the player app of player_app.c, tested on the PC
 * against the u2ps device stack through the test kit's loopback
 * (u2ps_testkit_loopback.h): the device stack enumerates, the app starts
 * its stream, every byte it sent arrives at the device stack's DAC app
 * intact, in the microframe of its on_sent; the callbacks of both sides,
 * and no contract violation. Copy it for your own player app.
 */
#include <stdio.h>

#include "player_app.h"
#include "u2ps_testkit_loopback.h"

#define CHECK U2PS_TESTKIT_CHECK

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
  CHECK(u2ps_testkit_loopback_reset(NULL));
  u2ps_testkit_host_on_packet(device_packet);
  player_app_init();
  CHECK(player_app_state.init_ok);
  u2ps_testkit_set_poll(player_app_poll);

  u2ps_testkit_host_connect();
  CHECK(u2ps_testkit_host_run_until(streaming, 2000));
  /* The DAC side started too */
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_ON_STREAM_START) == 1u);

  u2ps_testkit_trace_clear();
  u2ps_testkit_host_run_ms(100);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_HOST_ON_SENT) == 800u);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_ON_PACKET) == 800u);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_ON_HOLE) == 0u);
  CHECK(received_bytes == player_app_state.bytes_sent && bad_bytes == 0u);

  CHECK(player_app_state.contract_violations == 0u);
  return u2ps_testkit_result();
}
