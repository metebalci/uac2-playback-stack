/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dev.c: device stack start-up and re-init, attach and detach, VBUS,
 * bus events (reset, speed, suspend, resume) and the deferred reply.
 * References are to the USB 2.0 Specification, Revision 2.0. The stream
 * upcalls are in u2ps_dev_audio.c.
 */
#include <string.h>

#include "u2ps_ch9.h"
#include "u2ps_dev_priv.h"

/* The stack's state: hot on the packet path */
u2ps_dev_t u2ps_dev U2PS_FAST_BSS;

/* In DMA memory through its section, and on whole 32-byte cache lines, so
 * the port can clean and invalidate it alone (u2ps_dport.h) */
_Static_assert(U2PS_EP0_BUF >= 64u && U2PS_EP0_BUF % 32u == 0u && U2PS_EP0_BUF <= 0xFFE0u,
               "U2PS_EP0_BUF must be a multiple of 32, 64..65504 (an EP0 data stage's wLength "
               "is 16 bits): set -DU2PS_EP0_BUF (default 512)");
_Static_assert(U2PS_GAP_PAUSE >= 1u && U2PS_GAP_PAUSE <= 8191u,
               "U2PS_GAP_PAUSE must be 1..8191 microframes: a longer gap cannot be measured "
               "(u2ps_uframe_dist, 2^13 microframes): set -DU2PS_GAP_PAUSE (default 400)");
_Static_assert(U2PS_CLOSE_MAX >= 1u && U2PS_CLOSE_MAX <= 65535u,
               "U2PS_CLOSE_MAX must be 1..65535 ms: set -DU2PS_CLOSE_MAX (default 10)");
_Static_assert(U2PS_START_MAX >= 1u && U2PS_START_MAX <= 65535u,
               "U2PS_START_MAX must be 1..65535 ms: set -DU2PS_START_MAX (default 30)");
_Static_assert(U2PS_ISO_OUT_MPS_MAX >= 1u && U2PS_ISO_OUT_MPS_MAX <= 1024u,
               "U2PS_ISO_OUT_MPS_MAX must be 1..1024 (USB 2.0 5.6.3): set -DU2PS_ISO_OUT_MPS_MAX "
               "(default 1024)");
_Static_assert(U2PS_ISO_OUT_COUNT_MAX >= 1u && U2PS_ISO_OUT_COUNT_MAX <= 0xFFFFu,
               "U2PS_ISO_OUT_COUNT_MAX must be 1..65535, and at least the port's "
               "U2PS_DPORT_ISO_OUT_MIN: set -DU2PS_ISO_OUT_COUNT_MAX (default 16)");
_Alignas(32) uint8_t u2ps_dev_ep0_buf[U2PS_EP0_BUF]
  __attribute__((section(U2PS_DMA_SECTION)));

/* The port has started once: a re-init skips its hardware start-up */
static bool port_up;

#if U2PS_VBUS == U2PS_VBUS_APP
/* Outside u2ps_dev, which u2ps_dev_init() resets: a report before the
 * first init counts, and a re-init keeps the level */
bool u2ps_dev_vbus_on U2PS_FAST_BSS;
#endif

/* The rule the last init's description broke (u2ps_dev_description_error);
 * outside u2ps_dev, which a refused init leaves as it is */
static u2ps_dev_description_error_t description_error;

/* The device's session ends, for reason 'why': the port has stopped a
 * stream itself. The device is in the Default state at address 0,
 * unconfigured (9.1.1.3); until on_speed the stack treats the speed as
 * full speed. */
static void session_end(u2ps_config_end_t why)
{
  u2ps_dev_ctrl_reset();
  u2ps_dev_audio_reset();
  u2ps_dev_unconfigure(why);
  u2ps_dev.speed = U2PS_SPEED_FULL;
  u2ps_dev.speed_known = false;
  u2ps_dev.state = U2PS_DEV_DEFAULT;
  u2ps_dev.suspended = false;
  u2ps_dev.address = 0;
  u2ps_dev.config = 0;
  memset(u2ps_dev.alt, 0, sizeof u2ps_dev.alt);
}

/* The pull-up goes off, called with the vectors masked: the port stops a
 * stream and EP0, then removes it; the app's callbacks of the session's
 * end run here, in thread, never alongside the interrupt's */
static void pull_up_off(u2ps_config_end_t why)
{
  u2ps_dport_connect(false);
  session_end(why);
  u2ps_dev.attached = false;
  u2ps_dev.reset_seen = false;
}

static void detach(u2ps_config_end_t why)
{
  U2PS_DEV_MASK(true);
  u2ps_dev.in_upcall = true;
  pull_up_off(why);
  u2ps_dev.in_upcall = false;
  U2PS_DEV_MASK(false);
}

/* The pull-up goes on: the attach is allowed and VBUS is present. No
 * reset can come before it, and the idle bus after it, until the host's
 * first reset, is no suspend (u2ps_dev_on_suspend). */
static void pull_up_on(void)
{
  if (!u2ps_dev.attached) {
    u2ps_dev.attached = true;
    u2ps_dev.reset_seen = false;
  }
  u2ps_dport_connect(true);
}

u2ps_dev_err_t u2ps_dev_init(const u2ps_dev_description_t *desc)
{
  U2PS_DEV_CHECK_THREAD();
  description_error = u2ps_dev_description_check(desc);
  if (description_error != U2PS_DESCRIPTION_OK)
    return U2PS_DEV_ERR_DESCRIPTION;
  /* DMA memory the port's DMA does not reach: refused before anything
   * changes, at every init (an app may have switched a RAM's clock off) */
  if (!u2ps_dport_dma_check(u2ps_dev_ep0_buf, sizeof u2ps_dev_ep0_buf))
    return U2PS_DEV_ERR_DMA_MEMORY;
#if U2PS_CONTRACT_CHECKS
  u2ps_dev_check_init(desc);
#endif

  /* A re-init: detached first. The numbers that name streams and requests
   * go on, so that no stream or request the app still names matches one
   * of the new session; the stack's counters restart. */
  if (port_up && u2ps_dev.attached)
    detach(U2PS_CONFIG_END_REINIT);
  /* Detached, every EP0 stage aborted: the control buffer is the stack's
   * alone, and its section needs no zeroing at startup */
  memset(u2ps_dev_ep0_buf, 0, sizeof u2ps_dev_ep0_buf);
  if (port_up)
    U2PS_DEV_MASK(true);
  uint32_t stream_no = u2ps_dev.stream_no, id = u2ps_dev.id, rate = u2ps_dev.rate;
  memset(&u2ps_dev, 0, sizeof u2ps_dev);
  u2ps_dev.desc = desc;
  u2ps_dev.speed = U2PS_SPEED_FULL;
  u2ps_dev.state = U2PS_DEV_DEFAULT;
  u2ps_dev.rate = u2ps_dev_initial_rate(desc, rate);
  u2ps_dev.stream = U2PS_DEV_STREAM_IDLE;
  u2ps_dev.stream_no = stream_no;
  u2ps_dev.id = id;
  if (port_up) {
    u2ps_dport_reinit();
    U2PS_DEV_MASK(false);
    return U2PS_DEV_OK;
  }
  u2ps_dev_err_t e = u2ps_dport_init();
  port_up = e == U2PS_DEV_OK;
  return e;
}

u2ps_dev_description_error_t u2ps_dev_description_error(void)
{
  return description_error;
}

void u2ps_dev_connect(bool on)
{
  U2PS_DEV_CHECK_THREAD();
  if (!port_up) {
#if U2PS_CONTRACT_CHECKS
    if (on)
      U2PS_DEV_CONTRACT(U2PS_CONTRACT_DEV_CONNECT_BEFORE_INIT);
#endif
    return;
  }
  /* While detached, a detach only ends the permission */
  if (!on) {
    u2ps_dev.allowed = false;
    if (u2ps_dev.attached)
      detach(U2PS_CONFIG_END_DETACH);
    return;
  }
  /* No reset can come before the pull-up, so no mask is needed here */
  u2ps_dev.allowed = true;
  if (U2PS_DEV_VBUS_ON())
    pull_up_on();
}

#if U2PS_VBUS == U2PS_VBUS_APP
/* VBUS goes (7.2.1: the pull-up off within 10 s) or comes. A device
 * without VBUS is not powered (9.1.1.2), so its session ends as at a
 * detach; the port's events are ignored until VBUS returns (the upcalls'
 * U2PS_DEV_VBUS_ON checks). Before the port has started, no vector runs,
 * so nothing is masked. */
void u2ps_dev_vbus(bool present)
{
  U2PS_DEV_CHECK_THREAD();
  if (present == u2ps_dev_vbus_on)
    return;
  if (port_up)
    U2PS_DEV_MASK(true);
  u2ps_dev.in_upcall = true;
  u2ps_dev_vbus_on = present;
  if (!present && u2ps_dev.attached)
    pull_up_off(U2PS_CONFIG_END_VBUS);
  else if (present && u2ps_dev.allowed)
    pull_up_on();
  u2ps_app_on_vbus(present);
  u2ps_dev.in_upcall = false;
  if (port_up)
    U2PS_DEV_MASK(false);
}
#endif

bool u2ps_dev_vbus_present(void)
{
  return U2PS_DEV_VBUS_ON();
}

u2ps_speed_t u2ps_dev_speed(void)
{
  return u2ps_dev.speed_known ? u2ps_dev.speed : U2PS_SPEED_NONE;
}

void u2ps_dev_unconfigure(u2ps_config_end_t why)
{
  if (u2ps_dev.state != U2PS_DEV_CONFIGURED)
    return;
  u2ps_dev.state = U2PS_DEV_ADDRESS;
  u2ps_app_on_configured(false, why);
}

void u2ps_dev_on_reset(void)
{
  if (!U2PS_DEV_VBUS_ON())
    return;
  u2ps_dev.in_upcall = true;
  session_end(U2PS_CONFIG_END_BUS_RESET);
  u2ps_dev.reset_seen = true;
  u2ps_app_on_reset();
  u2ps_dev.in_upcall = false;
}

void u2ps_dev_on_speed(u2ps_speed_t speed)
{
  if (!U2PS_DEV_VBUS_ON())
    return;
  u2ps_dev.speed = speed;
  u2ps_dev.speed_known = true;
}

/* A suspended device keeps its address and configuration (9.1.1.6). The
 * bus is idle after the pull-up until the host's first reset, and a port
 * reports that as a suspend; the app hears of suspend only after a reset
 * since the attach, and of a resume only after a suspend it heard of. */
void u2ps_dev_on_suspend(void)
{
  if (!U2PS_DEV_VBUS_ON() || !u2ps_dev.reset_seen || u2ps_dev.suspended)
    return;
  u2ps_dev.in_upcall = true;
  u2ps_dev.suspended = true;
  u2ps_app_on_suspend();
  u2ps_dev.in_upcall = false;
}

void u2ps_dev_on_resume(void)
{
  if (!U2PS_DEV_VBUS_ON() || !u2ps_dev.suspended)
    return;
  u2ps_dev.in_upcall = true;
  u2ps_dev.suspended = false;
  u2ps_app_on_resume();
  u2ps_dev.in_upcall = false;
}

/* Bytes of a deferred reply copied into the control buffer per masked
 * region: one packet's worth */
#define REPLY_PIECE U2PS_EP0_MPS

static bool reply_pending(uint32_t id)
{
  return u2ps_dev.stage == U2PS_CTRL_PENDING && u2ps_dev.id == id;
}

/* The masked regions stay short whatever the reply's size: the first
 * checks the request and takes the first piece of the data, each further
 * one checks again and takes the next piece, and the last checks once more
 * and arms the stage, copying nothing. While the request is pending, EP0
 * NAKs and nothing else uses the control buffer; a SETUP that cancels it
 * between two regions is seen by the next check, and the copy stops there,
 * so the new request's use of the buffer is never overwritten. */
bool u2ps_dev_reply(uint32_t id, const void *data, int result)
{
  const uint8_t *src = data;
  uint16_t n = 0; /* bytes to send */

  U2PS_DEV_CHECK_THREAD();
#if U2PS_CONTRACT_CHECKS
  if (result < 0 && result != U2PS_REQ_STALL)
    U2PS_DEV_CONTRACT(U2PS_CONTRACT_DEV_REPLY_RESULT);
#endif
  U2PS_DEV_MASK(true);
  bool pending = reply_pending(id);
  if (pending) {
    const u2ps_setup_t *s = &u2ps_dev.setup;
    if (result < 0) {
      result = U2PS_REQ_STALL;
    } else if ((s->bmRequestType & U2PS_REQ_DIR_IN) && s->wLength > 0u) {
      int k = result;
      if (k > s->wLength)
        k = s->wLength;
      if (k > (int)U2PS_EP0_BUF)
        k = (int)U2PS_EP0_BUF;
      if (k > 0 && src == NULL) {
#if U2PS_CONTRACT_CHECKS
        U2PS_DEV_CONTRACT(U2PS_CONTRACT_DEV_REPLY_DATA);
#endif
        result = U2PS_REQ_STALL;
      } else {
#if U2PS_CONTRACT_CHECKS
        /* The request's buf, the control buffer, is the stack's again */
        if (k > 0 && (uintptr_t)src - (uintptr_t)u2ps_dev_ep0_buf < U2PS_EP0_BUF)
          U2PS_DEV_CONTRACT(U2PS_CONTRACT_DEV_REPLY_DATA);
#endif
        n = (uint16_t)k;
        result = k;
      }
    }
  }
  for (uint16_t off = 0; pending && off < n;) {
    uint16_t k = (uint16_t)(n - off) < REPLY_PIECE ? (uint16_t)(n - off) : REPLY_PIECE;
    memcpy(u2ps_dev_ep0_buf + off, src + off, k);
    off = (uint16_t)(off + k);
    U2PS_DEV_MASK(false);
    U2PS_DEV_MASK(true);
    pending = reply_pending(id);
  }
  if (pending) {
    u2ps_dev_audio_replied(result);
#if U2PS_CONTROL_HOOK
    /* The answer calls the hook, a callback in thread: inside it a release
     * masks nothing more (u2ps_dev_release) */
    u2ps_dev.in_upcall = true;
#endif
    u2ps_dev_ctrl_answer(result);
#if U2PS_CONTROL_HOOK
    u2ps_dev.in_upcall = false;
#endif
  }
  U2PS_DEV_MASK(false);
  return pending;
}

/* ---- Contract checks (u2ps_types.h, u2ps_contract_t) -------------------------- */

#if U2PS_CONTRACT_CHECKS
bool u2ps_dev_masked;

/* The stack's one tick period, in ms (u2ps_dev.h) */
#define TICK_PERIOD 10u

/* The description as the last successful init's check took it: its
 * checksum, and the counts and string lengths the sum reads, so that a
 * count or a string changed later never makes it read beyond what the
 * init checked. And the last tick's time. */
static struct {
  const u2ps_dev_description_t *desc;
  uint32_t sum;
  uint8_t format_count, sampling_frequency_count;
  uint8_t string_len[3];
  bool tick_known;
  uint32_t tick_ms;
} chk;

/* FNV-1a, 32 bits, over one value */
static uint32_t sum_add(uint32_t h, uint32_t v)
{
  for (unsigned i = 0; i < 4u; i++) {
    h ^= (uint8_t)(v >> (8u * i));
    h *= 16777619u;
  }
  return h;
}

static const char *string_of(const u2ps_dev_description_t *d, unsigned i)
{
  return i == 0u ? d->manufacturer : i == 1u ? d->product : d->serial_number;
}

/* Every field, and what the pointers point to as far as the init saw it:
 * a string up to its terminator, at most one character beyond its length
 * then */
static uint32_t description_sum(void)
{
  const u2ps_dev_description_t *d = chk.desc;
  uint32_t h = 2166136261u;
  uint32_t fields[] = {d->id_vendor,     d->id_product,   d->bcd_device,
                       d->self_powered,  d->max_power_ma, d->category,
                       d->terminal_type, d->nr_channels,  d->channel_config,
                       d->format_count,  d->sampling_frequency_count,
                       d->initial_sampling_frequency};
  for (unsigned i = 0; i < sizeof fields / sizeof fields[0]; i++)
    h = sum_add(h, fields[i]);
  for (unsigned i = 0; d->formats != NULL && i < chk.format_count; i++) {
    const u2ps_format_t *f = &d->formats[i];
    h = sum_add(h, (uint32_t)f->subslot_size << 16 | (uint32_t)f->bit_resolution << 8 |
                     (uint32_t)f->audio_data_format);
  }
  for (unsigned i = 0; d->sampling_frequencies != NULL && i < chk.sampling_frequency_count;
       i++)
    h = sum_add(h, d->sampling_frequencies[i]);
  for (unsigned i = 0; i < 3u; i++) {
    const char *s = string_of(d, i);
    unsigned k = 0;
    while (s != NULL && k <= chk.string_len[i] && s[k] != '\0')
      h = sum_add(h, (uint8_t)s[k++]);
    h = sum_add(h, s != NULL ? k : 0xFFFFFFFFu);
  }
  return h;
}

void u2ps_dev_check_init(const u2ps_dev_description_t *d)
{
  chk.desc = d;
  chk.format_count = d->format_count;
  chk.sampling_frequency_count = d->sampling_frequency_count;
  for (unsigned i = 0; i < 3u; i++) {
    const char *s = string_of(d, i);
    uint8_t n = 0;
    while (s != NULL && s[n] != '\0' && n < 255u)
      n++;
    chk.string_len[i] = n;
  }
  chk.sum = description_sum();
}

/* Once per change: the new sum is the one to compare with from now on */
void u2ps_dev_check_description(void)
{
  if (chk.desc == NULL)
    return;
  uint32_t sum = description_sum();
  if (sum != chk.sum) {
    chk.sum = sum;
    U2PS_DEV_CONTRACT(U2PS_CONTRACT_DEV_DESCRIPTION_CHANGED);
  }
}

void u2ps_dev_check_tick(uint32_t now_ms, bool timed)
{
  if (chk.tick_known && timed && now_ms - chk.tick_ms > TICK_PERIOD)
    U2PS_DEV_CONTRACT(U2PS_CONTRACT_DEV_TICK_LATE);
  chk.tick_known = true;
  chk.tick_ms = now_ms;
}

void u2ps_dev_mask(bool masked)
{
  if (masked) {
    u2ps_dev_masked = true;
    u2ps_dport_irq_mask(true);
  } else {
    u2ps_dport_irq_mask(false);
    u2ps_dev_masked = false;
  }
}

/* in_upcall: inside an upcall (an app callback, as the stack's own code
 * makes no thread call), or inside a masked region of a thread call that
 * calls the app; otherwise the mask held means another thread call runs */
void u2ps_dev_check_thread(void)
{
  if (u2ps_dev.in_upcall)
    U2PS_DEV_CONTRACT(U2PS_CONTRACT_DEV_CALL_IN_CALLBACK);
  else if (u2ps_dev_masked)
    U2PS_DEV_CONTRACT(U2PS_CONTRACT_DEV_CALL_NESTED);
}

int u2ps_dev_check_request(int result)
{
  if (result < 0 && result != U2PS_REQ_STALL && result != U2PS_REQ_DEFER)
    U2PS_DEV_CONTRACT(U2PS_CONTRACT_DEV_CALLBACK_RESULT);
  return result;
}

u2ps_stream_start_t u2ps_dev_check_start(u2ps_stream_start_t decision)
{
  if (decision != U2PS_STREAM_REFUSE && decision != U2PS_STREAM_OPEN &&
      decision != U2PS_STREAM_DEFER)
    U2PS_DEV_CONTRACT(U2PS_CONTRACT_DEV_CALLBACK_RESULT);
  return decision;
}
#endif
