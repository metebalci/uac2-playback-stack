/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_testkit_dev.c: the PC test kit's device side: the scripted host on
 * top of the fake device port (fake_dport.h), and the functions the kit's
 * copy of the device stack calls instead of the app's callbacks, which
 * record each in the trace and then call it (testkit/CMakeLists.txt). See
 * u2ps_testkit_dev.h.
 */
#include <stdio.h>
#include <string.h>

#include "fake_dport.h"
#include "u2ps_ch9.h"
#include "u2ps_testkit_dev.h"
#include "u2ps_testkit_priv.h"
#include "u2ps_uac2.h"

/* ---- The record of the fake port's violations -------------------------------- */

static unsigned port_count(void)
{
  return u2ps_fake_violations;
}

static void port_clear(void)
{
  u2ps_fake_violations = 0;
}

static const u2ps_testkit_port_t port = {port_count, port_clear};

/* ---- The scripted host ---------------------------------------------------------- */

/* What the last enumeration read, and what the host found in it */
static uint8_t device_desc[18];
static uint8_t config[U2PS_EP0_BUF];
static uint16_t config_len;
static uint16_t clock_windex; /* Clock Source ID << 8 | AudioControl interface */
static int as_interface;      /* the AudioStreaming interface, or -1 */
static bool clock_found;

void u2ps_testkit_dev_reset(void)
{
  u2ps_testkit_side_reset(0, &port);
  u2ps_fake_reset_all();
  config_len = 0;
  clock_found = false;
  as_interface = -1;
}

/* The fake port ends a close at the kit's next step. Its log of port
 * calls, which the kit does not read, starts again at each step, so that
 * it never fills. */
static void end_close(void)
{
  u2ps_fake_clear_log();
  if (u2ps_fake_stream_state == U2PS_FAKE_ST_CLOSING)
    u2ps_fake_closed();
}

void u2ps_testkit_dev_vbus(bool present)
{
  u2ps_fake_vbus_set(present);
}

bool u2ps_testkit_dev_attached(void)
{
  return u2ps_fake_attached;
}

void u2ps_testkit_dev_bus_reset(u2ps_speed_t speed)
{
  u2ps_fake_clear_log();
  u2ps_fake_bus_reset(speed);
}

void u2ps_testkit_dev_suspend(void)
{
  end_close();
  u2ps_fake_suspend();
}

void u2ps_testkit_dev_resume(void)
{
  end_close();
  u2ps_fake_resume();
}

static u2ps_testkit_control_t result_of(u2ps_fake_host_result_t r)
{
  switch (r) {
  case U2PS_FAKE_HOST_OK:
    return U2PS_TESTKIT_CONTROL_OK;
  case U2PS_FAKE_HOST_STALL:
    return U2PS_TESTKIT_CONTROL_STALL;
  case U2PS_FAKE_HOST_NAK:
    return U2PS_TESTKIT_CONTROL_NAK;
  default:
    return U2PS_TESTKIT_CONTROL_ERROR;
  }
}

/* A transfer that waits for a close ends when the port's close does */
static u2ps_fake_host_result_t finish(u2ps_fake_host_result_t r, void *data, uint16_t *actual)
{
  if (r == U2PS_FAKE_HOST_NAK && u2ps_fake_stream_state == U2PS_FAKE_ST_CLOSING) {
    u2ps_fake_closed();
    r = u2ps_fake_host_resume(data, actual);
  }
  end_close();
  return r;
}

u2ps_testkit_control_t u2ps_testkit_dev_control(const u2ps_setup_t *setup, void *data,
                                                uint16_t *actual)
{
  end_close();
  u2ps_fake_host_result_t r = u2ps_fake_host_control(setup->bmRequestType, setup->bRequest,
                                                     setup->wValue, setup->wIndex, setup->wLength,
                                                     data, actual);
  return result_of(finish(r, data, actual));
}

u2ps_testkit_control_t u2ps_testkit_dev_control_resume(void *data, uint16_t *actual)
{
  end_close();
  return result_of(finish(u2ps_fake_host_resume(data, actual), data, actual));
}

/* One transfer of the enumeration; prints which, if it fails */
static u2ps_testkit_control_t step(const char *what, uint8_t bmRequestType, uint8_t bRequest,
                                   uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                   void *data, uint16_t *actual)
{
  u2ps_setup_t s = {bmRequestType, bRequest, wValue, wIndex, wLength};
  u2ps_testkit_control_t r = u2ps_testkit_dev_control(&s, data, actual);
  if (r != U2PS_TESTKIT_CONTROL_OK)
    printf("u2ps testkit: enumerate: %s ended %s\n", what,
           r == U2PS_TESTKIT_CONTROL_STALL ? "STALL"
           : r == U2PS_TESTKIT_CONTROL_NAK ? "NAK"
                                           : "ERROR");
  return r;
}

/* The clock and the AudioStreaming interface of the configuration */
static void find_function(void)
{
  int ac_interface = -1;
  int itf = -1;
  for (unsigned off = 0; off + 2u <= config_len && config[off] >= 2u; off += config[off]) {
    const uint8_t *d = &config[off];
    if (d[1] == U2PS_DESC_INTERFACE && d[0] >= 9u && d[5] == U2PS_AUDIO) {
      itf = d[2];
      if (d[6] == U2PS_SUBCLASS_AUDIOCONTROL && ac_interface < 0)
        ac_interface = itf;
      if (d[6] == U2PS_SUBCLASS_AUDIOSTREAMING && as_interface < 0)
        as_interface = itf;
    }
    if (d[1] == U2PS_CS_INTERFACE && d[0] >= 4u && d[2] == U2PS_AC_CLOCK_SOURCE && !clock_found &&
        itf >= 0 && itf == ac_interface) {
      clock_windex = (uint16_t)(d[3] << 8 | ac_interface);
      clock_found = true;
    }
  }
}

u2ps_testkit_control_t u2ps_testkit_dev_enumerate(u2ps_speed_t speed)
{
  config_len = 0;
  clock_found = false;
  as_interface = -1;
  if (!u2ps_fake_attached) {
    printf("u2ps testkit: enumerate: the device is not attached (u2ps_dev_connect(true), and "
           "VBUS in mode app: u2ps_testkit_dev_vbus(true))\n");
    return U2PS_TESTKIT_CONTROL_ERROR;
  }
  end_close();
  u2ps_fake_bus_reset(speed);
  uint16_t n = 0;
  uint8_t first[64];
  u2ps_testkit_control_t r =
    step("GET_DESCRIPTOR device (64)", 0x80, U2PS_GET_DESCRIPTOR, U2PS_DESC_DEVICE << 8, 0,
         sizeof first, first, &n);
  if (r == U2PS_TESTKIT_CONTROL_OK)
    r = step("SET_ADDRESS 1", 0x00, U2PS_SET_ADDRESS, 1, 0, 0, NULL, NULL);
  if (r == U2PS_TESTKIT_CONTROL_OK)
    r = step("GET_DESCRIPTOR device", 0x80, U2PS_GET_DESCRIPTOR, U2PS_DESC_DEVICE << 8, 0,
             sizeof device_desc, device_desc, &n);
  if (r == U2PS_TESTKIT_CONTROL_OK)
    r = step("GET_DESCRIPTOR configuration (9)", 0x80, U2PS_GET_DESCRIPTOR,
             U2PS_DESC_CONFIGURATION << 8, 0, 9, config, &n);
  uint16_t total = (uint16_t)(config[2] | config[3] << 8);
  if (r == U2PS_TESTKIT_CONTROL_OK && (n < 9u || total > sizeof config)) {
    printf("u2ps testkit: enumerate: a configuration descriptor of %u bytes, wTotalLength %u\n",
           n, total);
    r = U2PS_TESTKIT_CONTROL_ERROR;
  }
  if (r == U2PS_TESTKIT_CONTROL_OK)
    r = step("GET_DESCRIPTOR configuration", 0x80, U2PS_GET_DESCRIPTOR,
             U2PS_DESC_CONFIGURATION << 8, 0, total, config, &n);
  if (r != U2PS_TESTKIT_CONTROL_OK)
    return r;
  config_len = n;
  /* The strings: LANGID, then each index the device descriptor names */
  if (device_desc[14] != 0u || device_desc[15] != 0u || device_desc[16] != 0u) {
    uint8_t s[255];
    r = step("GET_DESCRIPTOR string 0", 0x80, U2PS_GET_DESCRIPTOR, U2PS_DESC_STRING << 8, 0,
             sizeof s, s, &n);
    uint16_t langid = n >= 4u ? (uint16_t)(s[2] | s[3] << 8) : 0u;
    for (unsigned i = 14; i <= 16u && r == U2PS_TESTKIT_CONTROL_OK; i++)
      if (device_desc[i] != 0u)
        r = step("GET_DESCRIPTOR string", 0x80, U2PS_GET_DESCRIPTOR,
                 (uint16_t)(U2PS_DESC_STRING << 8 | device_desc[i]), langid, sizeof s, s, &n);
    if (r != U2PS_TESTKIT_CONTROL_OK)
      return r;
  }
  r = step("SET_CONFIGURATION 1", 0x00, U2PS_SET_CONFIGURATION, 1, 0, 0, NULL, NULL);
  if (r == U2PS_TESTKIT_CONTROL_OK)
    find_function();
  return r;
}

const uint8_t *u2ps_testkit_dev_configuration(uint16_t *len)
{
  *len = config_len;
  return config_len > 0u ? config : NULL;
}

/* The clock's Sampling Frequency Control, CUR, channel 0 (UAC2 5.2.5.1.1) */
static u2ps_testkit_control_t sampling_frequency(uint8_t bmRequestType, uint32_t *hz)
{
  if (!clock_found) {
    printf("u2ps testkit: no clock: enumerate the device first (u2ps_testkit_dev_enumerate)\n");
    return U2PS_TESTKIT_CONTROL_ERROR;
  }
  uint8_t b[4] = {(uint8_t)*hz, (uint8_t)(*hz >> 8), (uint8_t)(*hz >> 16), (uint8_t)(*hz >> 24)};
  uint16_t n = 0;
  u2ps_setup_t s = {bmRequestType, U2PS_REQ_CUR, U2PS_CS_SAM_FREQ_CONTROL << 8, clock_windex, 4};
  u2ps_testkit_control_t r = u2ps_testkit_dev_control(&s, b, &n);
  if ((bmRequestType & 0x80u) != 0u)
    *hz = r == U2PS_TESTKIT_CONTROL_OK && n == 4u
            ? (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24
            : 0u;
  return r;
}

u2ps_testkit_control_t u2ps_testkit_dev_set_sampling_frequency(uint32_t hz)
{
  return sampling_frequency(0x21, &hz);
}

u2ps_testkit_control_t u2ps_testkit_dev_get_sampling_frequency(uint32_t *hz)
{
  *hz = 0;
  return sampling_frequency(0xA1, hz);
}

u2ps_testkit_control_t u2ps_testkit_dev_set_interface(uint8_t alternate_setting)
{
  if (as_interface < 0) {
    printf("u2ps testkit: no AudioStreaming interface: enumerate the device first "
           "(u2ps_testkit_dev_enumerate)\n");
    return U2PS_TESTKIT_CONTROL_ERROR;
  }
  u2ps_setup_t s = {0x01, U2PS_SET_INTERFACE, alternate_setting, (uint16_t)as_interface, 0};
  return u2ps_testkit_dev_control(&s, NULL, NULL);
}

void u2ps_testkit_dev_packet(uint16_t len, u2ps_uframe_t uframe)
{
  end_close();
  u2ps_fake_packet(len, uframe);
}

void u2ps_testkit_dev_packet_bytes(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  end_close();
  u2ps_fake_packet_bytes(data, len, uframe);
}

void u2ps_testkit_dev_hole(uint16_t uframes, u2ps_miss_t why)
{
  end_close();
  u2ps_fake_missed(uframes, why);
}

void u2ps_testkit_dev_run_ms(uint32_t ms)
{
  for (uint32_t i = 0; i < ms; i++) {
    uint32_t now = u2ps_testkit_now_ms() + 1u;
    u2ps_testkit_set_now(now);
    end_close();
    if (!u2ps_testkit_poll_run(now))
      (void)u2ps_dev_tick(now);
  }
}

uint32_t u2ps_testkit_dev_feedback(void)
{
  return u2ps_fake_fb;
}

unsigned u2ps_testkit_dev_held(void)
{
  return u2ps_fake_held;
}

bool u2ps_testkit_dev_streaming(void)
{
  return u2ps_fake_stream_state == U2PS_FAKE_ST_OPEN;
}

/* ---- The callbacks, traced ------------------------------------------------------
 * The kit's copy of the device stack calls these instead of the app's
 * callbacks (testkit/CMakeLists.txt) */

int u2ps_testkit_traced_on_request(const u2ps_request_t *req);
u2ps_stream_start_t u2ps_testkit_traced_on_stream_start(const u2ps_stream_info_t *info,
                                                        u2ps_stream_cfg_t *cfg);
int u2ps_testkit_traced_on_sampling_frequency(uint32_t requested, uint32_t chosen, uint32_t id);
void u2ps_testkit_traced_on_configured(bool configured, u2ps_config_end_t why);
void u2ps_testkit_traced_on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe);
void u2ps_testkit_traced_on_hole(uint16_t uframes, u2ps_miss_t why);
void u2ps_testkit_traced_on_pause(uint16_t uframes);
void u2ps_testkit_traced_on_stream_stop(void);
void u2ps_testkit_traced_on_fault(u2ps_fault_t fault);
void u2ps_testkit_traced_on_reset(void);
void u2ps_testkit_traced_on_suspend(void);
void u2ps_testkit_traced_on_resume(void);
void u2ps_testkit_traced_on_vbus(bool present);
void u2ps_testkit_traced_on_control(const u2ps_setup_t *setup, int result);
void u2ps_testkit_traced_dev_on_contract(u2ps_contract_t violation);

int u2ps_testkit_traced_on_request(const u2ps_request_t *req)
{
  unsigned i = u2ps_testkit_record(U2PS_TESTKIT_ON_REQUEST, req->setup.bmRequestType,
                                   req->setup.bRequest, req->setup.wValue, req->setup.wIndex);
  int r = u2ps_app_on_request(req);
  u2ps_testkit_record_result(i, r);
  return r;
}

int u2ps_testkit_traced_on_sampling_frequency(uint32_t requested, uint32_t chosen, uint32_t id)
{
  unsigned i =
    u2ps_testkit_record(U2PS_TESTKIT_ON_SAMPLING_FREQUENCY, requested, chosen, id, 0);
  int r = u2ps_app_on_sampling_frequency(requested, chosen, id);
  u2ps_testkit_record_result(i, r);
  return r;
}

void u2ps_testkit_traced_on_configured(bool configured, u2ps_config_end_t why)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_ON_CONFIGURED, configured, (uint32_t)why, 0, 0);
  u2ps_app_on_configured(configured, why);
}

u2ps_stream_start_t u2ps_testkit_traced_on_stream_start(const u2ps_stream_info_t *info,
                                                        u2ps_stream_cfg_t *cfg)
{
  unsigned i = u2ps_testkit_record(U2PS_TESTKIT_ON_STREAM_START, info->stream,
                                   info->alternate_setting, info->sampling_frequency,
                                   info->max_packet_size);
  u2ps_stream_start_t r = u2ps_app_on_stream_start(info, cfg);
  u2ps_testkit_record_result(i, (int32_t)r);
  return r;
}

void u2ps_testkit_traced_on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_ON_PACKET, len, uframe, 0, 0);
  u2ps_app_on_packet(data, len, uframe);
}

void u2ps_testkit_traced_on_hole(uint16_t uframes, u2ps_miss_t why)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_ON_HOLE, uframes, (uint32_t)why, 0, 0);
  u2ps_app_on_hole(uframes, why);
}

void u2ps_testkit_traced_on_pause(uint16_t uframes)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_ON_PAUSE, uframes, 0, 0, 0);
  u2ps_app_on_pause(uframes);
}

void u2ps_testkit_traced_on_stream_stop(void)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_ON_STREAM_STOP, 0, 0, 0, 0);
  u2ps_app_on_stream_stop();
}

void u2ps_testkit_traced_on_fault(u2ps_fault_t fault)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_ON_FAULT, (uint32_t)fault, 0, 0, 0);
  u2ps_app_on_fault(fault);
}

void u2ps_testkit_traced_on_reset(void)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_ON_RESET, 0, 0, 0, 0);
  u2ps_app_on_reset();
}

void u2ps_testkit_traced_on_suspend(void)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_ON_SUSPEND, 0, 0, 0, 0);
  u2ps_app_on_suspend();
}

void u2ps_testkit_traced_on_resume(void)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_ON_RESUME, 0, 0, 0, 0);
  u2ps_app_on_resume();
}

#if U2PS_VBUS != U2PS_VBUS_BUS
void u2ps_testkit_traced_on_vbus(bool present)
{
  (void)u2ps_testkit_record(U2PS_TESTKIT_ON_VBUS, present, 0, 0, 0);
  u2ps_app_on_vbus(present);
}
#endif

#if U2PS_CONTROL_HOOK
void u2ps_testkit_traced_on_control(const u2ps_setup_t *setup, int result)
{
  unsigned i = u2ps_testkit_record(U2PS_TESTKIT_ON_CONTROL, setup->bmRequestType,
                                   setup->bRequest, setup->wValue, setup->wIndex);
  u2ps_testkit_record_result(i, result);
  u2ps_app_on_control(setup, result);
}
#endif

#if U2PS_CONTRACT_CHECKS
void u2ps_testkit_traced_dev_on_contract(u2ps_contract_t violation)
{
  u2ps_testkit_record_contract(violation, 0);
  u2ps_app_on_contract(violation);
}
#endif
