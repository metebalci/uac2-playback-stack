/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * app_stall.h: the stall mode of the example app, a bench test built only
 * with the CMake option U2PS_EXAMPLE_STALL. In the last APP_STALL_HOLD_MS
 * of every APP_STALL_PERIOD_MS of a stream (counted from on_stream_start)
 * the app holds every packet it may hold, count -
 * U2PS_DPORT_ISO_OUT_RESERVE (u2ps_stream_cfg_t), and then releases them
 * all at once; each packet the host sends meanwhile is a NO_BUFFER hole.
 * It also keeps the holes since the last packet by reason: at
 * on_stream_stop they are the stream's tail.
 *
 * No hardware access, so the PC tests run it (tests/test_example_stall.c).
 * The app calls app_stall_packet() and app_stall_hole() from its
 * callbacks, and app_stall_begin() and app_stall_end() from its thread
 * with the USB vector masked.
 */
#ifndef APP_STALL_H
#define APP_STALL_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_dport_cfg.h"
#include "u2ps_types.h"

#define APP_STALL_PERIOD_MS 3000u
#define APP_STALL_HOLD_MS 1000u

typedef struct {
  uint32_t limit;           /* the packets the app may hold: count - RESERVE */
  uint32_t held;            /* the packets it holds now */
  uint32_t held_max;        /* the most it held at once */
  uint32_t stalls;          /* stalls begun */
  uint32_t over;            /* packets delivered while it held 'limit' (released at once) */
  uint32_t tail[4];         /* holes since the last packet, by u2ps_miss_t */
  uint32_t tail_uframes[4]; /* their microframes */
  bool holding;             /* a stall is in progress */
} app_stall_t;

/* Whether a stall is due 'elapsed_ms' after the stream's start */
static inline bool app_stall_due(uint32_t elapsed_ms)
{
  return elapsed_ms % APP_STALL_PERIOD_MS >= APP_STALL_PERIOD_MS - APP_STALL_HOLD_MS;
}

/* A stream starts with 'count' buffers */
static inline void app_stall_start(app_stall_t *s, uint16_t count)
{
  *s = (app_stall_t){0};
  s->limit = count > U2PS_DPORT_ISO_OUT_RESERVE ? count - U2PS_DPORT_ISO_OUT_RESERVE : 0u;
}

/* A packet came (on_packet): true if the app holds it, false if it
 * releases it at once */
static inline bool app_stall_packet(app_stall_t *s)
{
  for (unsigned i = 0; i < 4u; i++)
    s->tail[i] = s->tail_uframes[i] = 0;
  if (!s->holding)
    return false;
  if (s->held >= s->limit) {
    s->over++;
    return false;
  }
  s->held++;
  if (s->held > s->held_max)
    s->held_max = s->held;
  return true;
}

/* A hole came (on_hole) */
static inline void app_stall_hole(app_stall_t *s, uint16_t uframes, u2ps_miss_t why)
{
  if ((unsigned)why < 4u) {
    s->tail[why]++;
    s->tail_uframes[why] += uframes;
  }
}

/* A stall begins, unless one is in progress */
static inline void app_stall_begin(app_stall_t *s)
{
  if (!s->holding) {
    s->holding = true;
    s->stalls++;
  }
}

/* The stall ends: returns the packets the app must release */
static inline uint32_t app_stall_end(app_stall_t *s)
{
  uint32_t n = s->held;
  s->holding = false;
  s->held = 0;
  return n;
}

#endif /* APP_STALL_H */
