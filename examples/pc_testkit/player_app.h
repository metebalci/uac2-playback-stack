/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * player_app.h: a minimal player app on the u2ps host stack, written as
 * firmware: no PC code in it. It plays a ramp of 2-channel audio at
 * 48 kHz to the first playable alternate setting of the device it finds.
 * test_player.c tests it against the test kit's scripted device,
 * test_player_loopback.c against the u2ps device stack. On a board, main()
 * calls player_app_init() once and player_app_poll() every millisecond.
 */
#ifndef PLAYER_APP_H
#define PLAYER_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_types.h"

/* What the app has seen, for its own log (and for the test) */
typedef struct {
  bool init_ok;                 /* u2ps_host_init() returned U2PS_HOST_OK */
  bool device;                  /* a device is configured (on_device, not yet on_end) */
  uint8_t alternate_setting;    /* the one it plays; 0: none playable */
  bool streaming;               /* between on_stream_start and on_stream_stop */
  uint64_t bytes_queued;        /* audio bytes queued */
  uint64_t bytes_sent;          /* of them, reported sent */
  uint32_t holes[4];            /* hole microframes, per u2ps_miss_t */
  uint32_t stops;
  uint16_t test_device;         /* the test device's PID (on_test_device); 0: none */
  uint32_t contract_violations; /* u2ps_app_on_contract() calls */
} player_app_state_t;

extern player_app_state_t player_app_state;

/* Byte i of the audio the app plays */
uint8_t player_app_byte(uint64_t i);

/* Once, from main */
void player_app_init(void);
/* Every millisecond, from main's loop: starts the stream once a device is
 * there */
void player_app_poll(uint32_t now_ms);

#endif /* PLAYER_APP_H */
