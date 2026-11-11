/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * player_app.c: a minimal player app on the u2ps host stack. It keeps 4
 * packets queued: one at the start, one more at each on_sent, each of the
 * size u2ps_host_slots_next() asks for. See player_app.h.
 */
#include "player_app.h"

#include "u2ps_host.h"
#include "u2ps_hport_cfg.h"

player_app_state_t player_app_state;

#define DEPTH   4u
#define BUFFERS (DEPTH + 1u)
/* Packet buffers; on a board they lie in DMA memory */
static _Alignas(U2PS_HPORT_ALIGN) uint8_t packets[BUFFERS][1024];
static unsigned next_buffer, in_flight;
static uint16_t packet_bytes[BUFFERS]; /* each queued packet's bytes, by buffer */
static unsigned oldest;                /* the buffer of the oldest packet in flight */
static uint32_t stream;
static uint16_t slot_bytes;
static bool start_due; /* on_device came: the thread starts the stream */

uint8_t player_app_byte(uint64_t i)
{
  return (uint8_t)(i * 3u + (i >> 10));
}

/* Queue packets until DEPTH are in flight: thread or callback */
static void fill(void)
{
  while (in_flight < DEPTH) {
    uint16_t slots = u2ps_host_slots_next(stream);
    uint16_t n = (uint16_t)(slots * slot_bytes);
    uint8_t *p = packets[next_buffer];
    if (n > sizeof packets[0])
      return;
    for (unsigned i = 0; i < n; i++)
      p[i] = player_app_byte(player_app_state.bytes_queued + i);
    if (!u2ps_host_queue(stream, p))
      return;
    packet_bytes[next_buffer] = n;
    player_app_state.bytes_queued += n;
    next_buffer = (next_buffer + 1u) % BUFFERS;
    in_flight++;
  }
}

void player_app_init(void)
{
  player_app_state = (player_app_state_t){0};
  next_buffer = in_flight = oldest = 0;
  start_due = false;
  player_app_state.init_ok = u2ps_host_init() == U2PS_HOST_OK;
  u2ps_host_vbus(true); /* a board reports its VBUS switch here */
  u2ps_host_connect(true);
}

void player_app_poll(uint32_t now_ms)
{
  u2ps_host_tick(now_ms);
  if (start_due) {
    start_due = false;
    (void)u2ps_host_stream_start(player_app_state.alternate_setting, 48000);
  }
}

void u2ps_app_host_on_device(const u2ps_host_device_t *dev)
{
  player_app_state.device = true;
  for (unsigned i = 0; i < dev->alternate_setting_count; i++) {
    const u2ps_host_alternate_setting_t *a = &dev->alternate_settings[i];
    if (a->skip == U2PS_HOST_SKIP_NONE && a->nr_channels == 2u) {
      player_app_state.alternate_setting = a->alternate_setting;
      start_due = true; /* a thread call: not from this callback */
      return;
    }
  }
}

void u2ps_app_host_on_end(u2ps_host_end_t why, u2ps_host_refusal_t refusal)
{
  (void)why;
  (void)refusal;
  player_app_state.device = false;
  start_due = false;
}

void u2ps_app_host_on_stream_start(const u2ps_host_stream_info_t *info)
{
  stream = info->stream;
  slot_bytes = (uint16_t)(info->nr_channels * info->format.subslot_size);
  player_app_state.streaming = true;
  fill();
}

void u2ps_app_host_on_sent(u2ps_uframe_t uframe)
{
  (void)uframe;
  player_app_state.bytes_sent += packet_bytes[oldest];
  oldest = (oldest + 1u) % BUFFERS;
  in_flight--;
  fill();
}

void u2ps_app_host_on_hole(uint16_t uframes, u2ps_miss_t why)
{
  if ((unsigned)why < 4u)
    player_app_state.holes[why] += uframes;
}

void u2ps_app_host_on_stream_stop(u2ps_host_stop_t why, u2ps_host_stop_request_t request)
{
  (void)why;
  (void)request;
  player_app_state.streaming = false;
  player_app_state.stops++;
  in_flight = 0; /* the packets still queued are the app's again */
  oldest = next_buffer;
}

/* The USB-IF test device (VID 0x1A0A): the stack runs its test; the app
 * only notes it */
void u2ps_app_host_on_test_device(uint16_t id_product, uint16_t bcd_device)
{
  (void)bcd_device;
  player_app_state.test_device = id_product;
}

/* The app never suspends, but a device's remote wakeup is reported */
void u2ps_app_host_on_resumed(bool remote_wakeup)
{
  (void)remote_wakeup;
}

#if U2PS_CONTRACT_CHECKS
void u2ps_app_on_contract(u2ps_contract_t violation)
{
  (void)violation;
  player_app_state.contract_violations++;
}
#endif
