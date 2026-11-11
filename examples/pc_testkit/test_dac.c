/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dac.c: the DAC app of dac_app.c, tested end to end on the PC with
 * the test kit (u2ps_testkit_dev.h): attach, enumeration, a sampling
 * frequency, a stream of one second at 96 kHz, a hole of each reason, the
 * stop; the app's callbacks in order, and no contract violation. Copy it
 * for your own DAC app.
 */
#include <stdio.h>

#include "dac_app.h"
#include "u2ps_testkit_dev.h"

#define CHECK U2PS_TESTKIT_CHECK

int main(void)
{
  /* Attach and enumerate */
  u2ps_testkit_dev_reset();
  dac_app_init();
  CHECK(dac_app_state.init_ok);
  u2ps_testkit_dev_vbus(true); /* the board's VBUS poll */
  CHECK(u2ps_testkit_dev_attached());
  u2ps_testkit_set_poll(dac_app_poll);
  CHECK(u2ps_testkit_dev_enumerate(U2PS_SPEED_HIGH) == U2PS_TESTKIT_CONTROL_OK);
  CHECK(dac_app_state.configured);

  /* 96 kHz, alternate setting 2 (24 bits in 4 bytes, 2 channels) */
  u2ps_testkit_trace_clear();
  CHECK(u2ps_testkit_dev_set_sampling_frequency(96000) == U2PS_TESTKIT_CONTROL_OK);
  CHECK(dac_app_state.sampling_frequency == 96000u);
  CHECK(u2ps_testkit_dev_set_interface(2) == U2PS_TESTKIT_CONTROL_OK);
  CHECK(dac_app_state.streaming);
  CHECK(u2ps_testkit_trace_match("on_sampling_frequency on_stream_start"));

  /* One second: 12 frames of 8 bytes in each microframe, a millisecond
   * every 8 microframes */
  u2ps_uframe_t m = 0;
  for (unsigned i = 0; i < 8000u; i++) {
    u2ps_testkit_dev_packet(96, m);
    m = u2ps_uframe_add(m, 1);
    if (i % 8u == 7u)
      u2ps_testkit_dev_run_ms(1);
  }
  CHECK(dac_app_state.frames == 96000u);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_ON_PACKET) == 8000u);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_ON_HOLE) == 0u);

  /* A hole of each reason: reported before the packet that ends it */
  const u2ps_miss_t why[4] = {U2PS_MISS_NO_PACKET, U2PS_MISS_LATE, U2PS_MISS_NO_BUFFER,
                              U2PS_MISS_CORRUPTED};
  for (unsigned i = 0; i < 4u; i++) {
    u2ps_testkit_trace_clear();
    u2ps_testkit_dev_hole(2, why[i]);
    m = u2ps_uframe_add(m, 2);
    u2ps_testkit_dev_packet(96, m);
    m = u2ps_uframe_add(m, 1);
    CHECK(u2ps_testkit_trace_match("on_hole on_packet"));
    const u2ps_testkit_call_t *c = u2ps_testkit_trace_get(0);
    CHECK(c != NULL && c->arg[0] == 2u && c->arg[1] == (uint32_t)why[i]);
    CHECK(dac_app_state.holes[why[i]] == 2u);
  }

  /* The host stops the stream */
  u2ps_testkit_trace_clear();
  CHECK(u2ps_testkit_dev_set_interface(0) == U2PS_TESTKIT_CONTROL_OK);
  CHECK(u2ps_testkit_trace_match("on_stream_stop"));
  CHECK(!dac_app_state.streaming);

  CHECK(dac_app_state.contract_violations == 0u);
  CHECK(dac_app_state.faults == 0u);
  return u2ps_testkit_result();
}
