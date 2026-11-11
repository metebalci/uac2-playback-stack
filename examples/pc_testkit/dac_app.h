/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * dac_app.h: a minimal DAC app on the u2ps device stack, written as
 * firmware: no PC code in it. test_dac.c tests it on the PC with the test
 * kit. On a board, main() calls dac_app_init() once and dac_app_poll()
 * every millisecond, and the board's VBUS poll calls u2ps_dev_vbus()
 * (VBUS mode app).
 */
#ifndef DAC_APP_H
#define DAC_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_types.h"

/* What the app has seen, for its own log (and for the test) */
typedef struct {
  bool init_ok;                 /* u2ps_dev_init() returned U2PS_DEV_OK */
  bool configured;
  uint32_t sampling_frequency;  /* the clock's, as the host set it */
  bool streaming;               /* between on_stream_start and on_stream_stop */
  uint8_t alternate_setting;    /* of the last stream */
  uint16_t frame_bytes;         /* nr_channels * subslot_size of the stream */
  uint64_t frames;              /* audio frames received */
  uint32_t holes[4];            /* hole microframes, per u2ps_miss_t */
  uint32_t pauses;
  uint32_t faults;
  uint32_t contract_violations; /* u2ps_app_on_contract() calls */
} dac_app_state_t;

extern dac_app_state_t dac_app_state;

/* Once, from main: describes the device and lets it attach */
void dac_app_init(void);
/* Every millisecond, from main's loop */
void dac_app_poll(uint32_t now_ms);

#endif /* DAC_APP_H */
