/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * dac_app.c: a minimal DAC app on the u2ps device stack: a 2-channel UAC2
 * sink of 16 and 24 bits at 44.1 to 192 kHz. It lends 8 buffers of class
 * memory, takes each packet's audio frames (a real DAC would hand them to
 * its I2S output here) and releases the packet at once. See dac_app.h.
 */
#include "dac_app.h"

#include "u2ps_dev.h"
#include "u2ps_dport_cfg.h"

dac_app_state_t dac_app_state;

static const u2ps_format_t formats[2] = {
  {.subslot_size = 2, .bit_resolution = 16}, /* alternate setting 1 */
  {.subslot_size = 4, .bit_resolution = 24}, /* alternate setting 2 */
};
static const uint32_t sampling_frequencies[6] = {44100, 48000, 88200, 96000, 176400, 192000};
static const u2ps_dev_description_t description = {
  .id_vendor = 0x1234,
  .id_product = 0x5678,
  .bcd_device = 0x0100,
  .manufacturer = "Example",
  .product = "Example DAC",
  .serial_number = "0001",
  .self_powered = false,
  .max_power_ma = 100,
  .category = 0x08,       /* I/O box */
  .terminal_type = 0x0301, /* speaker */
  .nr_channels = 2,
  .channel_config = 0x3,  /* front left, front right */
  .formats = formats,
  .format_count = 2,
  .sampling_frequencies = sampling_frequencies,
  .sampling_frequency_count = 6,
  .initial_sampling_frequency = 48000,
};

/* Class memory: 8 buffers of the largest packet; on a board it lies in
 * DMA memory */
#define BUFFERS 8u
#define STRIDE  U2PS_DPORT_STRIDE(U2PS_ISO_OUT_MPS_MAX)
static _Alignas(U2PS_DPORT_ALIGN) uint8_t class_mem[BUFFERS * STRIDE];
static uint32_t stream;

void dac_app_init(void)
{
  dac_app_state = (dac_app_state_t){0};
  dac_app_state.init_ok = u2ps_dev_init(&description) == U2PS_DEV_OK;
  if (dac_app_state.init_ok)
    u2ps_dev_connect(true);
}

void dac_app_poll(uint32_t now_ms)
{
  if (u2ps_dev_tick(now_ms) != U2PS_FAULT_NONE)
    dac_app_state.faults++;
}

int u2ps_app_on_request(const u2ps_request_t *req)
{
  (void)req;
  return U2PS_REQ_STALL; /* no request of its own */
}

int u2ps_app_on_sampling_frequency(uint32_t requested, uint32_t chosen, uint32_t id)
{
  (void)requested;
  (void)id;
  dac_app_state.sampling_frequency = chosen; /* a real DAC sets its clock here */
  return 0;
}

void u2ps_app_on_configured(bool configured, u2ps_config_end_t why)
{
  (void)why;
  dac_app_state.configured = configured;
}

u2ps_stream_start_t u2ps_app_on_stream_start(const u2ps_stream_info_t *info,
                                             u2ps_stream_cfg_t *cfg)
{
  stream = info->stream;
  dac_app_state.streaming = true;
  dac_app_state.alternate_setting = info->alternate_setting;
  dac_app_state.frame_bytes = (uint16_t)(description.nr_channels * info->format.subslot_size);
  cfg->mem = class_mem;
  cfg->stride = STRIDE;
  cfg->count = BUFFERS;
  return U2PS_STREAM_OPEN; /* cfg->fb_value: the stack's nominal value */
}

void u2ps_app_on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  (void)data;
  (void)uframe;
  dac_app_state.frames += len / dac_app_state.frame_bytes;
  u2ps_dev_release(stream, 1);
}

void u2ps_app_on_hole(uint16_t uframes, u2ps_miss_t why)
{
  if ((unsigned)why < 4u)
    dac_app_state.holes[why] += uframes;
}

void u2ps_app_on_pause(uint16_t uframes)
{
  (void)uframes;
  dac_app_state.pauses++;
}

void u2ps_app_on_stream_stop(void)
{
  dac_app_state.streaming = false;
}

void u2ps_app_on_fault(u2ps_fault_t fault)
{
  (void)fault;
  dac_app_state.faults++;
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

#if U2PS_CONTRACT_CHECKS
void u2ps_app_on_contract(u2ps_contract_t violation)
{
  (void)violation;
  dac_app_state.contract_violations++;
}
#endif
