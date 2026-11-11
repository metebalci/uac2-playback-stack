/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dev_audio.c: the audio function. The clock's requests (UAC2
 * 5.2.5.1.1-2), the AudioStreaming interface's alternate settings, which
 * open and close the stream, and the stream itself: packets, holes,
 * pauses, release and feedback.
 *
 * References: UAC2 is the USB Device Class Definition for Audio Devices,
 * Release 2.0; USB 2.0 is the USB Specification, Revision 2.0.
 *
 * The close is asynchronous (u2ps_dport.h). SET_INTERFACE of a streaming
 * alternate setting holds its status stage until the stream is open (USB
 * 2.0 9.2.6: the device completes the request before its status stage):
 * the open follows the app's on_stream_start at once, or the last
 * stream's on_closed, or the app's u2ps_dev_stream_open() after a deferred
 * start. Alternate setting 0 is answered at once, whatever the close does.
 * u2ps_dev_tick() measures the close against U2PS_CLOSE_MAX and the held
 * status against U2PS_START_MAX, past which the status completes anyway
 * (9.2.6.4 allows 50 ms), and returns each fault to its caller in thread.
 * The stream's upcalls, the release and the feedback value are on the
 * packet path, in section .text.u2ps_fast (U2PS_FAST_TEXT).
 */
#include <stddef.h>

#include "u2ps_ch9.h"
#include "u2ps_dev_priv.h"
#include "u2ps_uac2.h"

#define STALL U2PS_REQ_STALL

/* Samples per microframe at 'rate', in 16.16, rounded to the nearest
 * (USB 2.0 5.12.4.2): 768 kHz gives 0x00600000 */
static uint32_t fb_value(uint32_t rate)
{
  return (uint32_t)((((uint64_t)rate << 16) + 4000u) / 8000u);
}

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* ---- Clock ------------------------------------------------------------------ */

static void rate_apply(uint32_t rate)
{
  u2ps_dev.rate = rate;
  if (u2ps_dev.stream == U2PS_DEV_STREAM_OPEN)
    u2ps_dport_fb_set(fb_value(rate));
}

/* The description's rate closest to 'rate'; on a tie the lower one. The
 * rates ascend. */
static uint32_t rate_closest(uint32_t rate)
{
  const u2ps_dev_description_t *d = u2ps_dev.desc;
  unsigned i = 0;
  while (i + 1u < d->sampling_frequency_count && d->sampling_frequencies[i + 1u] <= rate)
    i++;
  if (i + 1u < d->sampling_frequency_count && rate > d->sampling_frequencies[i] &&
      d->sampling_frequencies[i + 1u] - rate < rate - d->sampling_frequencies[i])
    i++;
  return d->sampling_frequencies[i];
}

uint32_t u2ps_dev_initial_rate(const u2ps_dev_description_t *d, uint32_t current)
{
  for (unsigned i = 0; i < d->sampling_frequency_count; i++)
    if (d->sampling_frequencies[i] == current)
      return current;
  if (d->initial_sampling_frequency != 0u)
    return d->initial_sampling_frequency;
  return d->sampling_frequencies[0];
}

/* SET CUR of the Sampling Frequency Control. The Control adjusts the value
 * to the closest valid one (UAC2 5.2.2, p. 91): the closest of the
 * description's rates, which the app accepts, refuses or defers; the app
 * also sees the value the host asked for. */
static int rate_set(uint32_t asked, uint32_t id)
{
  uint32_t rate = rate_closest(asked);
  int result = U2PS_DEV_REQUEST_RESULT(u2ps_app_on_sampling_frequency(asked, rate, id));
  if (result == U2PS_REQ_DEFER) {
    u2ps_dev.rate_pending = rate;
    u2ps_dev.rate_deferred = true;
  } else if (result >= 0) {
    rate_apply(rate);
  } else {
    result = STALL;
  }
  return result;
}

void u2ps_dev_audio_replied(int result)
{
  if (!u2ps_dev.rate_deferred)
    return;
  u2ps_dev.rate_deferred = false;
  if (result >= 0)
    rate_apply(u2ps_dev.rate_pending);
}

/* The Clock Source's Controls (UAC2 5.2.5.1, request layout 5.2.2): the
 * Sampling Frequency Control with CUR (read and write) and RANGE (read),
 * Layout 3 (5.2.3.3); the Clock Validity Control with CUR (read only),
 * Layout 1 (5.2.3.1), always valid. Both are master Controls (CN = 0). */
static int clock_request(const u2ps_request_t *req)
{
  const u2ps_setup_t *s = &req->setup;
  const u2ps_dev_description_t *d = u2ps_dev.desc;
  uint8_t cs = (uint8_t)(s->wValue >> 8);
  bool get = (s->bmRequestType & U2PS_REQ_DIR_IN) != 0u;
  uint8_t *buf = req->buf;

  if ((s->wValue & 0xFFu) != 0u)
    return STALL;
  if (cs == U2PS_CS_SAM_FREQ_CONTROL) {
    if (get && s->bRequest == U2PS_REQ_CUR) {
      put32(buf, u2ps_dev.rate);
      return 4;
    }
    if (get && s->bRequest == U2PS_REQ_RANGE) {
      /* Each rate a subrange of its own: MIN = MAX, RES = 0 (5.2.1) */
      buf[0] = d->sampling_frequency_count;
      buf[1] = 0;
      for (unsigned i = 0; i < d->sampling_frequency_count; i++) {
        put32(buf + 2 + 12 * i, d->sampling_frequencies[i]);
        put32(buf + 6 + 12 * i, d->sampling_frequencies[i]);
        put32(buf + 10 + 12 * i, 0);
      }
      return 2 + 12 * d->sampling_frequency_count;
    }
    if (!get && s->bRequest == U2PS_REQ_CUR && req->len == 4u)
      return rate_set(buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 |
                          (uint32_t)buf[3] << 24,
                      req->id);
  } else if (cs == U2PS_CS_CLOCK_VALID_CONTROL && get && s->bRequest == U2PS_REQ_CUR) {
    buf[0] = 1;
    return 1;
  }
  return STALL;
}

/* Class requests to an interface (UAC2 5.2.2): the Clock Source of the
 * high-speed configuration is the only Entity with Controls. Every other
 * Entity, the interfaces themselves and the minimal FS configuration have
 * none, so their requests are request errors. */
int u2ps_dev_class_request(const u2ps_request_t *req)
{
  if (u2ps_dev.speed == U2PS_SPEED_HIGH &&
      req->setup.wIndex == (U2PS_ID_CLOCK << 8 | U2PS_ITF_AC))
    return clock_request(req);
  return STALL;
}

/* ---- Stream ------------------------------------------------------------------- */

/* The pending start's alternate setting: the one the app deferred, or the
 * one to start once the close has ended */
static uint8_t pending_alt(void)
{
  return u2ps_dev.stream == U2PS_DEV_STREAM_STARTING ? u2ps_dev.start_alt : u2ps_dev.open_alt;
}

/* The start ends: the port opens the stream with class memory *cfg, or
 * cfg is NULL and the start is refused. Returns 0 if the stream is open,
 * which takes the alternate setting, or U2PS_REQ_STALL if it is refused,
 * by the app or by the port (on_fault, on_stream_stop), and the interface
 * keeps alternate setting 0 (UAC2 5.2.6.1.2). If the host changed the
 * sampling frequency since the start, the feedback starts at the new one's
 * nominal value. */
static int start_end(const u2ps_stream_cfg_t *cfg)
{
  const u2ps_dev_description_t *d = u2ps_dev.desc;
  uint8_t alt = u2ps_dev.start_alt;

  u2ps_dev.stream = U2PS_DEV_STREAM_IDLE;
  u2ps_dev.alt[U2PS_ITF_AS] = 0;
  if (cfg == NULL)
    return STALL;
  u2ps_stream_t s = {
    .ep_out = U2PS_EP_DATA & 0x0Fu,
    .ep_fb = U2PS_EP_FB & 0x0Fu,
    .fb_interval = U2PS_EP_FB_INTERVAL,
    .fb_size = U2PS_FB_SIZE,
    .fb_value = u2ps_dev.rate == u2ps_dev.start_rate ? cfg->fb_value : fb_value(u2ps_dev.rate),
    .mps = u2ps_dev_mps(d, &d->formats[alt - 1u]),
    .stride = cfg->stride,
    .count = cfg->count,
    .mem = cfg->mem,
  };
  /* The port's buffer queue holds at most U2PS_ISO_OUT_COUNT_MAX; at
   * 65535 the uint16_t count cannot exceed it, and the test would be
   * always false (-Wtype-limits) */
#if U2PS_ISO_OUT_COUNT_MAX < 0xFFFFu
  bool too_many = cfg->count > U2PS_ISO_OUT_COUNT_MAX;
#else
  bool too_many = false;
#endif
  if (too_many || !u2ps_dport_stream_open(&s)) {
    u2ps_app_on_fault(U2PS_FAULT_OPEN);
    u2ps_app_on_stream_stop();
    return STALL;
  }
  u2ps_dev.stream = U2PS_DEV_STREAM_OPEN;
  u2ps_dev.alt[U2PS_ITF_AS] = alt;
  u2ps_dev.held = 0;
  u2ps_dev.packets = 0;
  u2ps_dev.tick_packets = 0;
  u2ps_dev.quiet_timed = false;
  u2ps_dev.long_gap = false;
  u2ps_dev.last_uframe = U2PS_UFRAME_UNKNOWN;
  for (unsigned i = 0; i <= U2PS_MISS_CORRUPTED; i++)
    u2ps_dev.missed[i] = 0;
  return 0;
}

/* A stream starts on alternate setting 'alt': the app first, the port's
 * endpoints only once it has lent the memory, so no packet of the stream
 * can come before the app is ready. Returns as start_end(), or
 * U2PS_REQ_HOLD if the app deferred the start. */
static int stream_start(uint8_t alt)
{
  const u2ps_dev_description_t *d = u2ps_dev.desc;
  const u2ps_format_t *f = &d->formats[alt - 1u];
  u2ps_stream_info_t info = {
    .stream = u2ps_dev.stream_no + 1u != 0u ? u2ps_dev.stream_no + 1u : 1u,
    .alternate_setting = alt,
    .format = *f,
    .sampling_frequency = u2ps_dev.rate,
    .max_packet_size = u2ps_dev_mps(d, f),
  };
  u2ps_stream_cfg_t cfg = {NULL, 0, 0, fb_value(u2ps_dev.rate)};

  u2ps_dev.open_alt = 0;
  u2ps_dev.stream_no = info.stream;
  u2ps_dev.start_alt = alt;
  u2ps_dev.start_rate = u2ps_dev.rate;
  switch (U2PS_DEV_START_RESULT(u2ps_app_on_stream_start(&info, &cfg))) {
  case U2PS_STREAM_OPEN:
    return start_end(&cfg);
  case U2PS_STREAM_DEFER:
    u2ps_dev.stream = U2PS_DEV_STREAM_STARTING;
    return U2PS_REQ_HOLD;
  default:
    return start_end(NULL);
  }
}

/* A new alternate setting of the AudioStreaming interface. A pending start
 * is cancelled (on_stream_stop), an open stream closes, also for the same
 * alternate setting again; a streaming setting starts now, or once the
 * close has ended. Returns SET_INTERFACE's result: 0 for alternate
 * setting 0 or a stream open now, U2PS_REQ_STALL for a refused start, or
 * U2PS_REQ_HOLD while the open waits for the close or the app. The
 * interface has alternate setting 0 until the stream opens. */
int u2ps_dev_audio_set_alt(uint8_t alt)
{
  u2ps_dev.alt[U2PS_ITF_AS] = 0;
  if (u2ps_dev.stream == U2PS_DEV_STREAM_STARTING) {
    u2ps_dev.stream = U2PS_DEV_STREAM_IDLE;
    u2ps_app_on_stream_stop();
  }
  if (u2ps_dev.stream == U2PS_DEV_STREAM_OPEN) {
    u2ps_dport_stream_close();
    u2ps_dev.stream = U2PS_DEV_STREAM_CLOSING;
    u2ps_dev.close_timed = false;
    u2ps_dev.close_fault = false;
  }
  u2ps_dev.open_alt = alt;
  if (alt == 0u)
    return 0;
  if (u2ps_dev.stream == U2PS_DEV_STREAM_CLOSING)
    return U2PS_REQ_HOLD;
  return stream_start(alt);
}

void u2ps_dev_on_closed(void)
{
  if (!U2PS_DEV_VBUS_ON())
    return;
  u2ps_dev.in_upcall = true;
  if (u2ps_dev.stream == U2PS_DEV_STREAM_CLOSING) {
    u2ps_dev_audio_tail();
    u2ps_dev.stream = U2PS_DEV_STREAM_IDLE;
    u2ps_app_on_stream_stop();
    if (u2ps_dev.open_alt != 0u)
      u2ps_dev_ctrl_held_answer(stream_start(u2ps_dev.open_alt));
  }
  u2ps_dev.in_upcall = false;
}

u2ps_open_result_t u2ps_dev_stream_open(uint32_t stream, const u2ps_stream_cfg_t *cfg)
{
  u2ps_open_result_t r = U2PS_OPEN_NOT_PENDING;

  U2PS_DEV_CHECK_THREAD();
  U2PS_DEV_MASK(true);
  u2ps_dev.in_upcall = true;
  if (stream == u2ps_dev.stream_no && u2ps_dev.stream == U2PS_DEV_STREAM_STARTING) {
    int result = start_end(cfg);
    r = result == 0 ? U2PS_OPEN_DONE : cfg == NULL ? U2PS_OPEN_REFUSED : U2PS_OPEN_FAULT;
    u2ps_dev_ctrl_held_answer(result);
  }
  u2ps_dev.in_upcall = false;
  U2PS_DEV_MASK(false);
  return r;
}

/* Bus reset or detach: a pending close ends here too, and no on_closed
 * follows; a pending start is cancelled. The port has reported the
 * stream's tail already (u2ps_dport.h). */
void u2ps_dev_audio_reset(void)
{
  u2ps_dev.open_alt = 0;
  if (u2ps_dev.stream != U2PS_DEV_STREAM_IDLE) {
    if (u2ps_dev.stream != U2PS_DEV_STREAM_STARTING)
      u2ps_dev_audio_tail();
    u2ps_dev.stream = U2PS_DEV_STREAM_IDLE;
    u2ps_app_on_stream_stop();
  }
}

/* A tick in a long gap: the holes the port holds back
 * (u2ps_dport_report_holes) and those the stack has summed since the last
 * packet reach the app now, as at the stream's end, in thread with the
 * vectors masked; the next packet's gap counts them as reported. Each tick
 * of the gap reports what came since the last. */
static void gap_holes(void)
{
  const uint32_t *missed = u2ps_dev.missed;

  u2ps_dev.in_upcall = true;
  u2ps_dport_report_holes();
  for (unsigned i = 0; i <= U2PS_MISS_CORRUPTED; i++)
    u2ps_dev.reported += missed[i];
  u2ps_dev_audio_tail();
  u2ps_dev.in_upcall = false;
}

u2ps_fault_t u2ps_dev_tick(uint32_t now_ms)
{
  u2ps_fault_t fault = U2PS_FAULT_NONE;

  U2PS_DEV_CHECK_THREAD();
#if U2PS_CONTRACT_CHECKS
  u2ps_dev_check_description();
#endif
  U2PS_DEV_MASK(true);
#if U2PS_CONTRACT_CHECKS
  /* A close or a held status this tick measures from an earlier one */
  bool timed = (u2ps_dev.stream == U2PS_DEV_STREAM_CLOSING && u2ps_dev.close_timed &&
                !u2ps_dev.close_fault) ||
               (u2ps_dev.stage == U2PS_CTRL_HELD && u2ps_dev.held_timed);
#endif
  if (u2ps_dev.stream == U2PS_DEV_STREAM_CLOSING && !u2ps_dev.close_fault) {
    /* The close began after the last tick: count from this one */
    if (!u2ps_dev.close_timed) {
      u2ps_dev.close_timed = true;
      u2ps_dev.close_start = now_ms;
    } else if (now_ms - u2ps_dev.close_start > U2PS_CLOSE_MAX) {
      u2ps_dev.close_fault = true;
      fault = U2PS_FAULT_CLOSE;
    }
  }
  /* SET_INTERFACE's held status: counted from the first tick after its
   * SETUP; past U2PS_START_MAX it completes, and the start stays pending.
   * A close fault due in the same call goes first; this one comes next. */
  if (u2ps_dev.stage == U2PS_CTRL_HELD) {
    if (!u2ps_dev.held_timed) {
      u2ps_dev.held_timed = true;
      u2ps_dev.held_start = now_ms;
    } else if (fault == U2PS_FAULT_NONE && now_ms - u2ps_dev.held_start > U2PS_START_MAX) {
      fault = U2PS_FAULT_START;
      u2ps_dev.alt[U2PS_ITF_AS] = pending_alt();
      u2ps_dev.in_upcall = true;
      u2ps_dev_ctrl_answer(0);
      u2ps_dev.in_upcall = false;
    }
  }
  /* A long gap: no packet for more than U2PS_GAP_PAUSE microframes'
   * worth of milliseconds since the stream's last packet. Microframe
   * numbers cannot tell it from a short one past 2^13 microframes, so the
   * next packet ends a pause. Counted from the first tick that saw no new
   * packet. Each tick of a long gap reports the holes held back. */
  if (u2ps_dev.stream == U2PS_DEV_STREAM_OPEN && u2ps_dev.packets != 0u) {
    if (u2ps_dev.packets != u2ps_dev.tick_packets || !u2ps_dev.quiet_timed) {
      u2ps_dev.tick_packets = u2ps_dev.packets;
      u2ps_dev.quiet_timed = true;
      u2ps_dev.quiet_start = now_ms;
      u2ps_dev.quiet_ms = 0;
    } else {
      u2ps_dev.quiet_ms = now_ms - u2ps_dev.quiet_start;
      if (u2ps_dev.quiet_ms > U2PS_GAP_PAUSE / 8u)
        u2ps_dev.long_gap = true;
    }
    if (u2ps_dev.long_gap)
      gap_holes();
  }
  U2PS_DEV_MASK(false);
#if U2PS_CONTRACT_CHECKS
  u2ps_dev_check_tick(now_ms, timed);
#endif
  return fault;
}

/* Each hole is reported once, possibly split into consecutive reports with
 * different reasons (u2ps_dport.h). They are summed per reason until the
 * next packet, or until the stream's end. Taken also without VBUS: at a
 * VBUS loss the port reports the stream's tail inside connect(false),
 * after the stack has noted the loss. */
U2PS_FAST_TEXT void u2ps_dev_on_missed(uint16_t uframes, u2ps_miss_t why)
{
  if (u2ps_dev.stream == U2PS_DEV_STREAM_IDLE || (unsigned)why > U2PS_MISS_CORRUPTED)
    return;
  u2ps_dev.missed[why] += uframes;
}

/* A pause's length: U2PS_PAUSE_LONG from 65,535 microframes on */
U2PS_FAST_TEXT static uint16_t clamp16(uint32_t v)
{
  return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
}

/* A hole of n microframes for 'why', in calls of at most 65,535 each */
U2PS_FAST_TEXT static void hole(uint32_t n, u2ps_miss_t why)
{
  for (; n > 0xFFFFu; n -= 0xFFFFu)
    u2ps_app_on_hole(0xFFFFu, why);
  if (n != 0u)
    u2ps_app_on_hole((uint16_t)n, why);
}

/* The holes of the other reasons than NO_PACKET, in order, and then none
 * is left */
U2PS_FAST_TEXT static void holes_other(uint32_t *missed)
{
  for (unsigned i = U2PS_MISS_LATE; i <= U2PS_MISS_CORRUPTED; i++)
    if (missed[i] != 0u)
      hole(missed[i], (u2ps_miss_t)i);
  for (unsigned i = 0; i <= U2PS_MISS_CORRUPTED; i++)
    missed[i] = 0;
}

/* The stream's end: the holes the port reported after its last packet, up
 * to the last packet the port received, before on_stream_stop; or those
 * reported so far, at a tick in a long gap. Only the port's counts: no
 * microframe number ends them. NO_PACKET beyond U2PS_GAP_PAUSE is a
 * pause. */
void u2ps_dev_audio_tail(void)
{
  uint32_t *missed = u2ps_dev.missed;
  uint32_t no_packet = missed[U2PS_MISS_NO_PACKET];

  if (no_packet > U2PS_GAP_PAUSE)
    u2ps_app_on_pause(clamp16(no_packet));
  else
    hole(no_packet, U2PS_MISS_NO_PACKET);
  holes_other(missed);
}

/* The gap before a packet. Only microframes in which the host sent nothing
 * (NO_PACKET) can form a pause; LATE, NO_BUFFER and CORRUPTED microframes
 * are holes however long. With microframe numbers for this packet and the
 * last one, a forward distance d gives d - 1 microframes without a packet,
 * and those that no other reason accounts for, and that no tick in a long
 * gap reported already (gap_holes), are NO_PACKET. A distance of
 * 0 or less (modulo 2^14, u2ps_uframe_dist) is no gap, and is counted;
 * then, as without microframe numbers, the port's reports decide. If
 * u2ps_dev_tick() saw a long gap, the NO_PACKET part is a pause if it
 * exceeds U2PS_GAP_PAUSE, or if the time the ticks saw exceeds the other
 * reasons' microframes and those the ticks reported by more than
 * U2PS_GAP_PAUSE (so the host sent
 * nothing in more than that many, whatever the numbers wrapped to): of the
 * NO_PACKET count if the microframe gap is at least the time the ticks
 * saw, otherwise of U2PS_PAUSE_LONG. A gap of other reasons alone, such
 * as a hold of every buffer the app may hold while the host sends, is no
 * pause. */
U2PS_FAST_TEXT void u2ps_dev_on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  uint32_t *missed = u2ps_dev.missed;

  if (!U2PS_DEV_VBUS_ON() || u2ps_dev.stream == U2PS_DEV_STREAM_IDLE)
    return;
  u2ps_dev.in_upcall = true;
  uint32_t no_packet = missed[U2PS_MISS_NO_PACKET];
  uint32_t other = missed[U2PS_MISS_LATE] + missed[U2PS_MISS_NO_BUFFER] +
                   missed[U2PS_MISS_CORRUPTED];
  uint32_t known = other + u2ps_dev.reported; /* reported, or to report, for this gap */
  uint32_t gap = 0;
  bool counted = false; /* gap holds the microframes since the last packet */
  if (uframe != U2PS_UFRAME_UNKNOWN && u2ps_dev.last_uframe != U2PS_UFRAME_UNKNOWN) {
    int16_t dist = u2ps_uframe_dist(u2ps_dev.last_uframe, uframe);
    if (dist > 0) {
      gap = (uint32_t)dist - 1u;
      counted = true;
      no_packet = gap > known ? gap - known : 0u;
    } else {
      u2ps_dev.uframe_back++;
    }
  }
  if (u2ps_dev.long_gap && (no_packet > U2PS_GAP_PAUSE ||
                            known + U2PS_GAP_PAUSE < u2ps_dev.quiet_ms * 8u)) {
    /* The ticks saw a long gap in which the host sent nothing: a pause,
     * whose length is the NO_PACKET count only if the microframes agree
     * with the time the ticks saw */
    bool agree = counted && no_packet > U2PS_GAP_PAUSE && u2ps_dev.quiet_ms * 8u <= gap;
    u2ps_app_on_pause(agree ? clamp16(no_packet) : (uint16_t)U2PS_PAUSE_LONG);
  } else if (no_packet > U2PS_GAP_PAUSE)
    u2ps_app_on_pause(clamp16(no_packet));
  else
    hole(no_packet, U2PS_MISS_NO_PACKET);
  missed[U2PS_MISS_NO_PACKET] = 0;
  if (other != 0u)
    holes_other(missed);
  u2ps_dev.last_uframe = uframe;
  u2ps_dev.long_gap = false;
  u2ps_dev.reported = 0;
  u2ps_dev.packets++;
  u2ps_dev.held++;
  u2ps_app_on_packet(data, len, uframe);
  u2ps_dev.in_upcall = false;
}

/* Checked with the vectors masked when called from thread: the stream must
 * still be the one the app means, and hold that many packets */
U2PS_FAST_TEXT void u2ps_dev_release(uint32_t stream, uint16_t n)
{
  bool thread = !u2ps_dev.in_upcall;

#if U2PS_CONTRACT_CHECKS
  if (thread && u2ps_dev_masked)
    U2PS_DEV_CONTRACT(U2PS_CONTRACT_DEV_CALL_NESTED);
#endif
  if (thread)
    U2PS_DEV_MASK(true);
  if (stream == u2ps_dev.stream_no && u2ps_dev.stream != U2PS_DEV_STREAM_IDLE &&
      n <= u2ps_dev.held) {
    u2ps_dev.held -= n;
    if (n != 0u)
      u2ps_dport_release(n);
  } else {
    u2ps_dev.release_errors++;
#if U2PS_CONTRACT_CHECKS
    /* A stream that stopped is reported only inside a callback: from
     * thread, the interrupt may have stopped it just before */
    if (stream == u2ps_dev.stream_no && u2ps_dev.stream != U2PS_DEV_STREAM_IDLE)
      U2PS_DEV_CONTRACT(U2PS_CONTRACT_DEV_RELEASE_COUNT);
    else if (stream == 0u || stream > u2ps_dev.stream_no || !thread)
      U2PS_DEV_CONTRACT(U2PS_CONTRACT_DEV_RELEASE_STREAM);
#endif
  }
  if (thread)
    U2PS_DEV_MASK(false);
}

U2PS_FAST_TEXT void u2ps_dev_fb_set(uint32_t value)
{
  u2ps_dport_fb_set(value);
}

void u2ps_dev_stats(u2ps_dev_stats_t *stats)
{
  U2PS_DEV_CHECK_THREAD();
  U2PS_DEV_MASK(true);
  const u2ps_dport_stats_t *p = u2ps_dport_stats();
  *stats = (u2ps_dev_stats_t){
    .late_completions = p->late_completions,
    .late = p->late,
    .no_buffer = p->no_buffer,
    .corrupted = p->corrupted,
    .fb_recoveries = p->fb_recoveries,
    .errors = p->errors,
    .fallback_over = p->fallback_over,
    .release_errors = u2ps_dev.release_errors,
    .uframe_back = u2ps_dev.uframe_back,
  };
  U2PS_DEV_MASK(false);
}
