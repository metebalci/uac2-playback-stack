/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_testkit_dev.c: the PC test kit's device side (u2ps_testkit_dev.h),
 * through its public API only, with a small DAC app of its own. Checked:
 * - the enumeration and the trace of its callbacks: on_vbus, on_reset,
 *   on_configured(true, NONE), with their arguments;
 * - SET and GET CUR of the sampling frequency: on_sampling_frequency;
 * - SET_INTERFACE: on_stream_start with the stream, alternate setting,
 *   sampling frequency and max_packet_size; the class memory the app lent;
 * - packets: on_packet with each length and microframe, the running
 *   counter's bytes and given bytes; the reserve: the app holds at most
 *   count - U2PS_DPORT_ISO_OUT_RESERVE, and the packets that come while
 *   it does are NO_BUFFER holes, before the next packet, or before
 *   on_stream_stop with the microframes without a packet between them;
 * - an injected hole of each reason reaches on_hole with that reason and
 *   length, before the packet that ends it; a microframe gap without a
 *   report is NO_PACKET; NO_PACKET beyond U2PS_GAP_PAUSE reaches on_pause;
 * - SET_INTERFACE 0: on_stream_stop; a deferred start: NAK, then the
 *   app's open and the resume;
 * - the time: the poll at each millisecond with the time;
 * - trace_match: exact, with *n, and a mismatch;
 * - a contract violation (a release of more than held): recorded with its
 *   code, passed to the app's hook, fails u2ps_testkit_result() until
 *   taken; a port violation (a packet without a stream) the same;
 * - a VBUS loss: on_configured(false, VBUS), on_vbus(false), detached.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "u2ps_testkit_dev.h"

/* ---- The app ------------------------------------------------------------------ */

static const u2ps_format_t formats[3] = {{2, 16, U2PS_AUDIO_DATA_FORMAT_PCM},
                                         {4, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
                                         {4, 32, U2PS_AUDIO_DATA_FORMAT_PCM}};
static const uint32_t rates[4] = {44100, 48000, 96000, 192000};
static const u2ps_dev_description_t desc = {
  .id_vendor = 0x1234,
  .id_product = 0x5678,
  .bcd_device = 0x0100,
  .manufacturer = "Maker",
  .product = "Kit DAC",
  .serial_number = "1",
  .max_power_ma = 100,
  .category = 0x08,
  .terminal_type = 0x0301,
  .nr_channels = 2,
  .channel_config = 0x3,
  .formats = formats,
  .format_count = 3,
  .sampling_frequencies = rates,
  .sampling_frequency_count = 4,
};

#define BUFFERS 4u
static _Alignas(U2PS_DPORT_ALIGN) uint8_t class_mem[BUFFERS * 1024];

static struct {
  bool hold;             /* release nothing in on_packet */
  bool defer;            /* on_stream_start defers */
  uint32_t stream;
  uint16_t mps;
  uint8_t first;         /* the last packet's first byte */
  unsigned contracts;    /* u2ps_app_on_contract() calls */
  u2ps_contract_t contract;
  unsigned polls;
  uint32_t poll_now;
} app;

static void app_init(void)
{
  CHECK(u2ps_dev_init(&desc) == U2PS_DEV_OK);
  u2ps_dev_connect(true);
}

static void app_poll(uint32_t now_ms)
{
  app.polls++;
  app.poll_now = now_ms;
  (void)u2ps_dev_tick(now_ms);
}

int u2ps_app_on_request(const u2ps_request_t *req)
{
  (void)req;
  return U2PS_REQ_STALL;
}

int u2ps_app_on_sampling_frequency(uint32_t requested, uint32_t chosen, uint32_t id)
{
  (void)requested;
  (void)chosen;
  (void)id;
  return 0;
}

void u2ps_app_on_configured(bool configured, u2ps_config_end_t why)
{
  (void)configured;
  (void)why;
}

static void lend(const u2ps_stream_info_t *info, u2ps_stream_cfg_t *cfg)
{
  cfg->mem = class_mem;
  cfg->stride = U2PS_DPORT_STRIDE(info->max_packet_size);
  cfg->count = BUFFERS;
}

static u2ps_stream_info_t deferred;

u2ps_stream_start_t u2ps_app_on_stream_start(const u2ps_stream_info_t *info,
                                             u2ps_stream_cfg_t *cfg)
{
  app.stream = info->stream;
  app.mps = info->max_packet_size;
  if (app.defer) {
    deferred = *info;
    return U2PS_STREAM_DEFER;
  }
  lend(info, cfg);
  return U2PS_STREAM_OPEN;
}

void u2ps_app_on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  (void)uframe;
  app.first = len > 0u ? data[0] : 0u;
  if (!app.hold)
    u2ps_dev_release(app.stream, 1);
}

void u2ps_app_on_hole(uint16_t uframes, u2ps_miss_t why)
{
  (void)uframes;
  (void)why;
}

void u2ps_app_on_pause(uint16_t uframes)
{
  (void)uframes;
}

void u2ps_app_on_stream_stop(void)
{
}

void u2ps_app_on_fault(u2ps_fault_t fault)
{
  (void)fault;
}

void u2ps_app_on_reset(void)
{
}

void u2ps_app_on_suspend(void)
{
}

void u2ps_app_on_resume(void)
{
}

#if U2PS_VBUS != U2PS_VBUS_BUS
void u2ps_app_on_vbus(bool present)
{
  (void)present;
}
#endif

#if U2PS_CONTROL_HOOK
void u2ps_app_on_control(const u2ps_setup_t *setup, int result)
{
  (void)setup;
  (void)result;
}
#endif

void u2ps_app_on_contract(u2ps_contract_t violation)
{
  app.contracts++;
  app.contract = violation;
}

/* ---- Helpers ------------------------------------------------------------------- */

/* The ith call of the trace is 'callback' with arguments a0, a1 */
static bool call_is(unsigned i, u2ps_testkit_callback_t callback, uint32_t a0, uint32_t a1)
{
  const u2ps_testkit_call_t *c = u2ps_testkit_trace_get(i);
  if (c == NULL || c->callback != callback || c->arg[0] != a0 || c->arg[1] != a1) {
    printf("  trace %u: want %s %u %u\n", i, u2ps_testkit_callback_name(callback), (unsigned)a0,
           (unsigned)a1);
    u2ps_testkit_trace_print();
    return false;
  }
  return true;
}

/* A scenario: reset, the app's init, enumerated at high speed, the trace
 * cleared */
static void start(void)
{
  memset(&app, 0, sizeof app);
  u2ps_testkit_dev_reset();
  u2ps_testkit_set_poll(NULL);
  app_init();
  u2ps_testkit_dev_vbus(true);
  CHECK(u2ps_testkit_dev_attached());
  CHECK(u2ps_testkit_dev_enumerate(U2PS_SPEED_HIGH) == U2PS_TESTKIT_CONTROL_OK);
  u2ps_testkit_trace_clear();
}

/* ---- Tests ------------------------------------------------------------------------ */

static void test_enumerate(void)
{
  printf("enumerate\n");
  memset(&app, 0, sizeof app);
  u2ps_testkit_dev_reset();
  app_init();
  u2ps_testkit_dev_vbus(true);
  CHECK(u2ps_testkit_dev_enumerate(U2PS_SPEED_HIGH) == U2PS_TESTKIT_CONTROL_OK);
#if U2PS_VBUS == U2PS_VBUS_APP
  CHECK(u2ps_testkit_trace_match("on_vbus on_reset on_configured"));
  CHECK(call_is(0, U2PS_TESTKIT_ON_VBUS, 1, 0));
  CHECK(call_is(2, U2PS_TESTKIT_ON_CONFIGURED, 1, U2PS_CONFIG_END_NONE));
#else
  /* Mode bus: no VBUS reports */
  CHECK(u2ps_testkit_trace_match("on_reset on_configured"));
  CHECK(call_is(1, U2PS_TESTKIT_ON_CONFIGURED, 1, U2PS_CONFIG_END_NONE));
#endif
  uint16_t len = 0;
  const uint8_t *cfg = u2ps_testkit_dev_configuration(&len);
  CHECK(cfg != NULL && len > 9u && cfg[1] == 0x02 && (cfg[2] | cfg[3] << 8) == len);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_ON_CONFIGURED) == 1u);
}

static void test_stream(void)
{
  printf("stream\n");
  start();
  CHECK(u2ps_testkit_dev_set_sampling_frequency(96000) == U2PS_TESTKIT_CONTROL_OK);
  uint32_t hz = 0;
  CHECK(u2ps_testkit_dev_get_sampling_frequency(&hz) == U2PS_TESTKIT_CONTROL_OK);
  CHECK(hz == 96000u);
  CHECK(call_is(0, U2PS_TESTKIT_ON_SAMPLING_FREQUENCY, 96000, 96000));

  u2ps_testkit_trace_clear();
  CHECK(u2ps_testkit_dev_set_interface(2) == U2PS_TESTKIT_CONTROL_OK);
  CHECK(u2ps_testkit_dev_streaming());
  const u2ps_testkit_call_t *c = u2ps_testkit_trace_get(0);
  CHECK(c != NULL && c->callback == U2PS_TESTKIT_ON_STREAM_START && c->arg[0] == app.stream &&
        c->arg[1] == 2u && c->arg[2] == 96000u && c->arg[3] == app.mps &&
        c->result == U2PS_STREAM_OPEN);

  /* Packets: each length and microframe; the counter's bytes, then given
   * bytes */
  u2ps_testkit_dev_packet(96, 100);
  u2ps_testkit_dev_packet(104, 101);
  CHECK(call_is(1, U2PS_TESTKIT_ON_PACKET, 96, 100));
  CHECK(call_is(2, U2PS_TESTKIT_ON_PACKET, 104, 101));
  const uint8_t bytes[8] = {0xA5, 1, 2, 3, 4, 5, 6, 7};
  u2ps_testkit_dev_packet_bytes(bytes, sizeof bytes, 102);
  CHECK(app.first == 0xA5u);
  CHECK(call_is(3, U2PS_TESTKIT_ON_PACKET, 8, 102));
  CHECK(u2ps_testkit_dev_held() == 0u);

  /* A hole of each reason, with its length: reported before the packet
   * that ends it, whose microframe follows the hole */
  const u2ps_miss_t why[4] = {U2PS_MISS_NO_PACKET, U2PS_MISS_LATE, U2PS_MISS_NO_BUFFER,
                              U2PS_MISS_CORRUPTED};
  u2ps_uframe_t m = 102;
  for (unsigned i = 0; i < 4u; i++) {
    u2ps_testkit_trace_clear();
    u2ps_testkit_dev_hole((uint16_t)(i + 2u), why[i]);
    m = (u2ps_uframe_t)(m + i + 3u);
    u2ps_testkit_dev_packet(96, m);
    CHECK(u2ps_testkit_trace_match("on_hole on_packet"));
    CHECK(call_is(0, U2PS_TESTKIT_ON_HOLE, i + 2u, why[i]));
    CHECK(call_is(1, U2PS_TESTKIT_ON_PACKET, 96, m));
  }
  /* A gap in the microframes without a report is NO_PACKET */
  u2ps_testkit_trace_clear();
  m = (u2ps_uframe_t)(m + 6u);
  u2ps_testkit_dev_packet(96, m);
  CHECK(u2ps_testkit_trace_match("on_hole on_packet"));
  CHECK(call_is(0, U2PS_TESTKIT_ON_HOLE, 5, U2PS_MISS_NO_PACKET));
  /* NO_PACKET beyond U2PS_GAP_PAUSE is a pause */
  u2ps_testkit_trace_clear();
  u2ps_testkit_dev_hole(U2PS_GAP_PAUSE + 1u, U2PS_MISS_NO_PACKET);
  m = (u2ps_uframe_t)(m + U2PS_GAP_PAUSE + 2u);
  u2ps_testkit_dev_packet(96, m);
  CHECK(u2ps_testkit_trace_match("on_pause on_packet"));
  CHECK(call_is(0, U2PS_TESTKIT_ON_PAUSE, U2PS_GAP_PAUSE + 1u, 0));

  /* The reserve: the app holds at most BUFFERS - 1 (the kit's port keeps
   * U2PS_DPORT_ISO_OUT_RESERVE, 1, armed); the next two packets are
   * reserve packets, a NO_BUFFER hole of 2 before the packet after them */
  CHECK(U2PS_DPORT_ISO_OUT_RESERVE == 1u);
  u2ps_testkit_trace_clear();
  app.hold = true;
  for (unsigned i = 0; i < BUFFERS + 1u; i++)
    u2ps_testkit_dev_packet(96, (u2ps_uframe_t)(m + 1u + i));
  CHECK(u2ps_testkit_trace_match("on_packet*3"));
  CHECK(u2ps_testkit_dev_held() == BUFFERS - 1u);
  u2ps_dev_release(app.stream, BUFFERS - 1u);
  CHECK(u2ps_testkit_dev_held() == 0u);
  app.hold = false;
  u2ps_testkit_dev_packet(96, (u2ps_uframe_t)(m + BUFFERS + 2u));
  CHECK(u2ps_testkit_trace_match("on_packet*3 on_hole on_packet"));
  CHECK(call_is(3, U2PS_TESTKIT_ON_HOLE, 2, U2PS_MISS_NO_BUFFER));
  m = (u2ps_uframe_t)(m + BUFFERS + 2u);

  /* Reserve packets after the stream's last packet: the tail, before
   * on_stream_stop, with the microframes without a packet between them */
  u2ps_testkit_trace_clear();
  app.hold = true;
  for (unsigned i = 0; i < BUFFERS; i++)
    u2ps_testkit_dev_packet(96, (u2ps_uframe_t)(m + 1u + i));
  u2ps_testkit_dev_packet(96, (u2ps_uframe_t)(m + BUFFERS + 3u));
  CHECK(u2ps_testkit_trace_match("on_packet*3"));

  /* Alternate setting 0 stops it */
  u2ps_testkit_trace_clear();
  CHECK(u2ps_testkit_dev_set_interface(0) == U2PS_TESTKIT_CONTROL_OK);
  CHECK(u2ps_testkit_trace_match("on_hole on_hole on_stream_stop"));
  CHECK(call_is(0, U2PS_TESTKIT_ON_HOLE, 2, U2PS_MISS_NO_PACKET));
  CHECK(call_is(1, U2PS_TESTKIT_ON_HOLE, 2, U2PS_MISS_NO_BUFFER));
  CHECK(!u2ps_testkit_dev_streaming());
  app.hold = false;
}

/* Holes at the long gap: the app holds every buffer it may while the
 * host streams, then the host pauses. The kit's time (the tick) reports
 * the stall's holes once no packet has come for more than U2PS_GAP_PAUSE
 * microframes' worth, before the host sends again; the packet after the
 * pause brings only the pause */
static void test_gap(void)
{
  printf("holes at the long gap\n");
  start();
  CHECK(u2ps_testkit_dev_set_interface(1) == U2PS_TESTKIT_CONTROL_OK);
  app.hold = true;
  for (unsigned i = 0; i < BUFFERS - 1u + 5u; i++)
    u2ps_testkit_dev_packet(96, (u2ps_uframe_t)(100u + i));
  u2ps_testkit_trace_clear();
  u2ps_testkit_dev_run_ms(U2PS_GAP_PAUSE / 8u);
  CHECK(u2ps_testkit_trace_match(""));
  u2ps_testkit_dev_run_ms(2);
  CHECK(u2ps_testkit_trace_match("on_hole"));
  CHECK(call_is(0, U2PS_TESTKIT_ON_HOLE, 5, U2PS_MISS_NO_BUFFER));
  u2ps_testkit_dev_run_ms(10);
  CHECK(u2ps_testkit_trace_match("on_hole"));
  u2ps_dev_release(app.stream, BUFFERS - 1u);
  app.hold = false;
  u2ps_testkit_dev_packet(96, (u2ps_uframe_t)(100u + BUFFERS - 1u + 5u + 1000u));
  CHECK(u2ps_testkit_trace_match("on_hole on_pause on_packet"));
  /* Its length where the microframes agree with the time the ticks saw,
   * U2PS_GAP_PAUSE / 8 + 11 ms (u2ps_app_on_pause) */
  uint32_t seen = 8u * (U2PS_GAP_PAUSE / 8u + 11u);
  CHECK(call_is(1, U2PS_TESTKIT_ON_PAUSE, seen <= 1000u + 5u ? 1000u : U2PS_PAUSE_LONG, 0));
  CHECK(u2ps_testkit_dev_set_interface(0) == U2PS_TESTKIT_CONTROL_OK);
  CHECK(u2ps_testkit_trace_match("on_hole on_pause on_packet on_stream_stop"));
}

static void test_deferred(void)
{
  printf("deferred start\n");
  start();
  app.defer = true;
  CHECK(u2ps_testkit_dev_set_interface(1) == U2PS_TESTKIT_CONTROL_NAK);
  CHECK(!u2ps_testkit_dev_streaming());
  u2ps_stream_cfg_t cfg = {0};
  lend(&deferred, &cfg);
  cfg.fb_value = u2ps_testkit_dev_feedback();
  CHECK(u2ps_dev_stream_open(deferred.stream, &cfg) == U2PS_OPEN_DONE);
  CHECK(u2ps_testkit_dev_control_resume(NULL, NULL) == U2PS_TESTKIT_CONTROL_OK);
  CHECK(u2ps_testkit_dev_streaming());
  CHECK(u2ps_testkit_trace_match("on_stream_start"));
}

static void test_time(void)
{
  printf("time\n");
  start();
  u2ps_testkit_set_poll(app_poll);
  u2ps_testkit_dev_run_ms(25);
  CHECK(app.polls == 25u);
  CHECK(app.poll_now == 25u);
  CHECK(u2ps_testkit_now_ms() == 25u);
  u2ps_testkit_set_poll(NULL);
  u2ps_testkit_dev_run_ms(5);
  CHECK(app.polls == 25u);
  CHECK(u2ps_testkit_now_ms() == 30u);
  /* A reset starts the time again */
  u2ps_testkit_dev_reset();
  CHECK(u2ps_testkit_now_ms() == 0u);
}

static void test_match(void)
{
  printf("trace_match\n");
  start();
  CHECK(u2ps_testkit_trace_match(""));
  CHECK(u2ps_testkit_dev_set_interface(1) == U2PS_TESTKIT_CONTROL_OK);
  for (unsigned i = 0; i < 3u; i++)
    u2ps_testkit_dev_packet(16, (u2ps_uframe_t)i);
  u2ps_testkit_dev_hole(1, U2PS_MISS_LATE);
  u2ps_testkit_dev_packet(16, 4);
  CHECK(u2ps_testkit_trace_match("on_stream_start on_packet*3 on_hole on_packet"));
  CHECK(u2ps_testkit_trace_match(
    "on_stream_start on_packet on_packet on_packet on_hole on_packet"));
  CHECK(!u2ps_testkit_trace_match("on_stream_start on_packet*2 on_hole on_packet"));
  CHECK(!u2ps_testkit_trace_match("on_stream_start on_packet*3 on_hole"));
  CHECK(!u2ps_testkit_trace_match("on_stream_start on_packet*3 on_hole on_packet*2"));
  CHECK(!u2ps_testkit_trace_match("on_stream_start on_packet*3 on_pause on_packet"));
  CHECK(!u2ps_testkit_trace_match("on_stream_start on_packets*3 on_hole on_packet"));
  CHECK(!u2ps_testkit_trace_match("on_stream_start on_packet*3 on_hole on_packet on_bogus"));
  CHECK(u2ps_testkit_trace_count() == 6u);
  CHECK(u2ps_testkit_calls(U2PS_TESTKIT_ON_PACKET) == 4u);
  CHECK(strcmp(u2ps_testkit_callback_name(U2PS_TESTKIT_HOST_ON_SENT), "host_on_sent") == 0);
}

static void test_violations(void)
{
  printf("violations\n");
  start();
  CHECK(u2ps_testkit_result() == 0);
  CHECK(u2ps_testkit_dev_set_interface(1) == U2PS_TESTKIT_CONTROL_OK);
  /* A release of more than held: the stack reports it, the kit records
   * it, passes it on, and fails the result until it is taken */
  u2ps_dev_release(app.stream, 1);
  CHECK(u2ps_testkit_contract_count() == 1u);
  CHECK(app.contracts == 1u && app.contract == U2PS_CONTRACT_DEV_RELEASE_COUNT);
  CHECK(u2ps_testkit_trace_match("on_stream_start on_contract"));
  CHECK(call_is(1, U2PS_TESTKIT_ON_CONTRACT, U2PS_CONTRACT_DEV_RELEASE_COUNT, 0));
  CHECK(strcmp(u2ps_testkit_contract_name(U2PS_CONTRACT_DEV_RELEASE_COUNT),
               "U2PS_CONTRACT_DEV_RELEASE_COUNT") == 0);
  CHECK(u2ps_testkit_result() == 1);
  CHECK(u2ps_testkit_contract_take(U2PS_CONTRACT_DEV_CALL_NESTED) == 0u);
  CHECK(u2ps_testkit_contract_take(U2PS_CONTRACT_DEV_RELEASE_COUNT) == 1u);
  CHECK(u2ps_testkit_contract_count() == 0u);
  CHECK(u2ps_testkit_result() == 0);
  CHECK(u2ps_testkit_dev_set_interface(0) == U2PS_TESTKIT_CONTROL_OK);
  /* A packet without a stream: the script's mistake, a port violation */
  u2ps_testkit_dev_packet(16, 1);
  CHECK(u2ps_testkit_port_violations() == 1u);
  CHECK(u2ps_testkit_result() == 1);
  CHECK(u2ps_testkit_port_violations_take() == 1u);
  CHECK(u2ps_testkit_result() == 0);
  /* Violations survive a reset until taken */
  u2ps_testkit_dev_hole(1, U2PS_MISS_LATE);
  u2ps_testkit_dev_reset();
  CHECK(u2ps_testkit_port_violations() == 1u);
  CHECK(u2ps_testkit_port_violations_take() == 1u);
}

static void test_vbus(void)
{
  printf("vbus\n");
  start();
#if U2PS_VBUS == U2PS_VBUS_APP
  u2ps_testkit_dev_vbus(false);
  CHECK(!u2ps_testkit_dev_attached());
  CHECK(u2ps_testkit_trace_match("on_configured on_vbus"));
  CHECK(call_is(0, U2PS_TESTKIT_ON_CONFIGURED, 0, U2PS_CONFIG_END_VBUS));
  CHECK(call_is(1, U2PS_TESTKIT_ON_VBUS, 0, 0));
  u2ps_testkit_dev_vbus(true);
  CHECK(u2ps_testkit_dev_attached());
#endif
}

int main(void)
{
  test_enumerate();
  test_stream();
  test_gap();
  test_deferred();
  test_time();
  test_match();
  test_violations();
  test_vbus();
  /* Nothing left in the record: the kit's own verdict passes */
  CHECK_EQ(u2ps_testkit_result(), 0);
  /* A failed check of the kit fails its verdict */
  CHECK(!U2PS_TESTKIT_CHECK(1 + 1 == 3));
  CHECK_EQ(u2ps_testkit_result(), 1);
  return check_done();
}
