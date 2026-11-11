/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * hmodel.c: see hmodel.h.
 */
#include "hmodel.h"

#include <stdio.h>
#include <string.h>

#include "u2ps_ch9.h"
#include "u2ps_uac2.h"

u2ps_hmodel_t u2ps_hmodel;

/* The default configuration: the sink of hmodel.h, wTotalLength 293
 * bytes, one descriptor per line (the 17-byte one on two) */
static const uint8_t default_config[] = {
  0x09, 0x02, 0x25, 0x01, 0x02, 0x01, 0x00, 0x80, 0x32,
  0x08, 0x0b, 0x00, 0x02, 0x01, 0x00, 0x20, 0x00,
  0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x20, 0x00,
  0x09, 0x24, 0x01, 0x00, 0x02, 0x08, 0x2e, 0x00, 0x00,
  0x08, 0x24, 0x0a, 0x04, 0x03, 0x07, 0x00, 0x00,
  0x11, 0x24, 0x02, 0x01, 0x01, 0x01, 0x00, 0x04, 0x02,
  0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x0c, 0x24, 0x03, 0x03, 0x01, 0x03, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00,
  0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x20, 0x00,
  0x09, 0x04, 0x01, 0x01, 0x02, 0x01, 0x02, 0x20, 0x00,
  0x10, 0x24, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00,
  0x06, 0x24, 0x02, 0x01, 0x02, 0x10,
  0x07, 0x05, 0x01, 0x05, 0x84, 0x01, 0x01,
  0x08, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x07, 0x05, 0x81, 0x11, 0x04, 0x00, 0x04,
  0x09, 0x04, 0x01, 0x02, 0x02, 0x01, 0x02, 0x20, 0x00,
  0x10, 0x24, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00,
  0x06, 0x24, 0x02, 0x01, 0x04, 0x18,
  0x07, 0x05, 0x01, 0x05, 0x08, 0x03, 0x01,
  0x08, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x07, 0x05, 0x81, 0x11, 0x04, 0x00, 0x04,
  0x09, 0x04, 0x01, 0x03, 0x02, 0x01, 0x02, 0x20, 0x00,
  0x10, 0x24, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00,
  0x06, 0x24, 0x02, 0x01, 0x04, 0x20,
  0x07, 0x05, 0x01, 0x05, 0x08, 0x03, 0x01,
  0x08, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x07, 0x05, 0x81, 0x11, 0x04, 0x00, 0x04,
  0x09, 0x04, 0x01, 0x04, 0x02, 0x01, 0x02, 0x20, 0x00,
  0x10, 0x24, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00,
  0x06, 0x24, 0x02, 0x01, 0x03, 0x18,
  0x07, 0x05, 0x01, 0x05, 0x46, 0x02, 0x01,
  0x08, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x07, 0x05, 0x81, 0x11, 0x04, 0x00, 0x04,
};

static void put16(uint8_t *p, unsigned v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
  put16(p, v & 0xFFFFu);
  put16(p + 2, v >> 16);
}

void u2ps_hmodel_set_config(uint8_t index, const uint8_t *desc, uint16_t len)
{
  /* len, a uint16_t, never exceeds U2PS_HMODEL_CONFIG_MAX */
  _Static_assert(U2PS_HMODEL_CONFIG_MAX == 0xFFFFu, "len is checked by its type");
  if (index >= 2u) {
    printf("u2ps_hmodel_set_config: index %u or length %u too large\n", index, len);
    return;
  }
  memcpy(u2ps_hmodel.config[index], desc, len);
  u2ps_hmodel.config_len[index] = len;
}

void u2ps_hmodel_set_qualifier(uint8_t n)
{
  uint8_t *q = u2ps_hmodel.qualifier;
  q[0] = U2PS_QUALIFIER_LEN;
  q[1] = U2PS_DESC_DEVICE_QUALIFIER;
  memcpy(q + 2, u2ps_hmodel.device + 2, 5); /* bcdUSB, class, subclass, protocol */
  q[7] = 64;
  q[8] = n;
  q[9] = 0;
  u2ps_hmodel.qualifier_len = U2PS_QUALIFIER_LEN;
}

void u2ps_hmodel_set_other(uint8_t index, const uint8_t *desc, uint16_t len)
{
  /* len, a uint16_t, never exceeds U2PS_HMODEL_CONFIG_MAX */
  if (index >= 2u || len < 2u) {
    printf("u2ps_hmodel_set_other: index %u or length %u out of range\n", index, len);
    return;
  }
  memcpy(u2ps_hmodel.other[index], desc, len);
  u2ps_hmodel.other[index][1] = U2PS_DESC_OTHER_SPEED;
  u2ps_hmodel.other_len[index] = len;
}

void u2ps_hmodel_set_rates(const uint32_t *rates, unsigned n)
{
  put16(u2ps_hmodel.range, n);
  for (unsigned i = 0; i < n && 2u + 12u * (i + 1u) <= sizeof u2ps_hmodel.range; i++) {
    put32(u2ps_hmodel.range + 2 + 12 * i, rates[i]);
    put32(u2ps_hmodel.range + 6 + 12 * i, rates[i]);
    put32(u2ps_hmodel.range + 10 + 12 * i, 0);
  }
  u2ps_hmodel.range_len = (uint16_t)(2u + 12u * n);
}

void u2ps_hmodel_set_string(uint8_t index, const char *text)
{
  if (text == NULL) {
    u2ps_hmodel.string_len[index] = 0;
    return;
  }
  unsigned n = (unsigned)strlen(text);
  uint8_t *s = u2ps_hmodel.string[index];
  s[0] = (uint8_t)(2u + 2u * n);
  s[1] = U2PS_DESC_STRING;
  for (unsigned i = 0; i < n; i++)
    put16(s + 2 + 2 * i, (uint8_t)text[i]);
  u2ps_hmodel.string_len[index] = s[0];
}

/* The ten rates of the default device, 44.1 to 768 kHz */
static const uint32_t default_rates[10] = {44100,  48000,  88200,  96000,  176400,
                                           192000, 352800, 384000, 705600, 768000};

void u2ps_hmodel_default(void)
{
  static const uint8_t device[18] = {18,   U2PS_DESC_DEVICE, 0x00, 0x02, 0xEF, 0x02,
                                     0x01, 64,   0x34, 0x12, 0x78, 0x56,
                                     0x00, 0x01, 1,    2,    3,    1};
  memset(&u2ps_hmodel, 0, sizeof u2ps_hmodel);
  memcpy(u2ps_hmodel.device, device, sizeof device);
  u2ps_hmodel_set_config(0, default_config, sizeof default_config);
  static const uint8_t langid[4] = {4, U2PS_DESC_STRING, 0x09, 0x04};
  memcpy(u2ps_hmodel.string[0], langid, sizeof langid);
  u2ps_hmodel.string_len[0] = sizeof langid;
  u2ps_hmodel_set_string(1, "Maker");
  u2ps_hmodel_set_string(2, "Player Test");
  u2ps_hmodel_set_string(3, "SN-0001");
  u2ps_hmodel_set_rates(default_rates, 10);
  u2ps_hmodel.cur = 48000;
  u2ps_hmodel.valid = 1;
  u2ps_hmodel.clock_windex = 0x0400;
  u2ps_hmodel.itf_as = 1;
  u2ps_hmodel.hang_at = u2ps_hmodel.stall_at = u2ps_hmodel.error_at = -1;
  u2ps_hmodel.stall_alt = -1;
}

static u2ps_hfake_answer_t unexpected(const u2ps_hfake_setup_t *s)
{
  u2ps_hmodel.unexpected++;
  printf("device model: unexpected request %02x %02x %04x %04x %04x\n", s->bmRequestType,
         s->bRequest, s->wValue, s->wIndex, s->wLength);
  return U2PS_HFAKE_ANSWER_STALL;
}

/* Copy a reply of n bytes, at most wLength */
static u2ps_hfake_answer_t reply(const u2ps_hfake_setup_t *s, uint8_t *data, uint16_t *len,
                                 const uint8_t *src, unsigned n)
{
  if (n > s->wLength)
    n = s->wLength;
  memcpy(data, src, n);
  *len = (uint16_t)n;
  return U2PS_HFAKE_ANSWER_OK;
}

u2ps_hfake_answer_t u2ps_hmodel_answer(uint8_t addr, const u2ps_hfake_setup_t *s, uint8_t *data,
                                       uint16_t *len)
{
  int nth = (int)u2ps_hmodel.requests++;
  if (nth == u2ps_hmodel.hang_at)
    return U2PS_HFAKE_ANSWER_NONE;
  if (nth == u2ps_hmodel.stall_at)
    return U2PS_HFAKE_ANSWER_STALL;
  if (nth == u2ps_hmodel.error_at)
    return U2PS_HFAKE_ANSWER_ERROR;
  /* Nothing answers at another address: the port's retries end in errors */
  if (addr != u2ps_hmodel.address)
    return U2PS_HFAKE_ANSWER_ERROR;

  uint8_t type = (uint8_t)(s->wValue >> 8), index = (uint8_t)s->wValue;
  if (s->bmRequestType == 0x80 && s->bRequest == U2PS_GET_DESCRIPTOR) {
    if (type == U2PS_DESC_DEVICE && index == 0u)
      return reply(s, data, len, u2ps_hmodel.device, sizeof u2ps_hmodel.device);
    if (type == U2PS_DESC_CONFIGURATION && index < 2u && u2ps_hmodel.config_len[index] > 0u)
      return reply(s, data, len, u2ps_hmodel.config[index], u2ps_hmodel.config_len[index]);
    /* A full-speed-only device answers with a request error (USB 2.0
     * 9.6.2) */
    if (type == U2PS_DESC_DEVICE_QUALIFIER && index == 0u) {
      if (u2ps_hmodel.qualifier_len == 0u)
        return U2PS_HFAKE_ANSWER_STALL;
      u2ps_hmodel.qualified = true;
      return reply(s, data, len, u2ps_hmodel.qualifier, u2ps_hmodel.qualifier_len);
    }
    if (type == U2PS_DESC_OTHER_SPEED && index < 2u && u2ps_hmodel.other_len[index] > 0u &&
        u2ps_hmodel.qualified)
      return reply(s, data, len, u2ps_hmodel.other[index], u2ps_hmodel.other_len[index]);
    if (type == U2PS_DESC_STRING && index < 4u) {
      if (u2ps_hmodel.string_len[index] == 0u)
        return U2PS_HFAKE_ANSWER_STALL;
      return reply(s, data, len, u2ps_hmodel.string[index], u2ps_hmodel.string_len[index]);
    }
    return unexpected(s);
  }
  /* GET_STATUS(device) (USB 2.0 9.4.5): bus-powered, no remote wakeup */
  if (s->bmRequestType == 0x80 && s->bRequest == U2PS_GET_STATUS && s->wValue == 0u &&
      s->wIndex == 0u && s->wLength == 2u) {
    static const uint8_t status[2] = {0, 0};
    u2ps_hmodel.get_status++;
    return reply(s, data, len, status, sizeof status);
  }
  if (s->bmRequestType == 0x00 && s->bRequest == U2PS_SET_ADDRESS && s->wLength == 0u &&
      s->wValue >= 1u && s->wValue <= 127u) {
    u2ps_hmodel.address = (uint8_t)s->wValue;
    return U2PS_HFAKE_ANSWER_OK;
  }
  if (s->bmRequestType == 0x00 && s->bRequest == U2PS_SET_CONFIGURATION && s->wLength == 0u) {
    u2ps_hmodel.configuration = (uint8_t)s->wValue;
    return U2PS_HFAKE_ANSWER_OK;
  }
  if (s->bmRequestType == 0x01 && s->bRequest == U2PS_SET_INTERFACE && s->wLength == 0u &&
      s->wIndex == u2ps_hmodel.itf_as && u2ps_hmodel.configuration != 0u) {
    u2ps_hmodel.set_interfaces++;
    if ((int)s->wValue == u2ps_hmodel.stall_alt)
      return U2PS_HFAKE_ANSWER_STALL;
    u2ps_hmodel.alt = (uint8_t)s->wValue;
    return U2PS_HFAKE_ANSWER_OK;
  }
  /* SET CUR of the clock's Sampling Frequency Control (UAC2 5.2.2) */
  if (s->bmRequestType == 0x21 && s->bRequest == U2PS_REQ_CUR &&
      s->wValue == (U2PS_CS_SAM_FREQ_CONTROL << 8) && s->wIndex == u2ps_hmodel.clock_windex &&
      s->wLength == 4u && u2ps_hmodel.configuration != 0u) {
    if (u2ps_hmodel.stall_set_cur)
      return U2PS_HFAKE_ANSWER_STALL;
    if (!u2ps_hmodel.fixed_cur)
      u2ps_hmodel.cur = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
                        (uint32_t)data[3] << 24;
    return U2PS_HFAKE_ANSWER_OK;
  }
  /* The clock's requests (UAC2 5.2.2): its Sampling Frequency and Clock
   * Validity Controls, channel 0 */
  if (s->bmRequestType == 0xA1 && s->wIndex == u2ps_hmodel.clock_windex &&
      u2ps_hmodel.configuration != 0u) {
    uint8_t buf[4];
    if (s->wValue == (U2PS_CS_SAM_FREQ_CONTROL << 8) && s->bRequest == U2PS_REQ_RANGE) {
      if (u2ps_hmodel.stall_range)
        return U2PS_HFAKE_ANSWER_STALL;
      return reply(s, data, len, u2ps_hmodel.range, u2ps_hmodel.range_len);
    }
    if (s->wValue == (U2PS_CS_SAM_FREQ_CONTROL << 8) && s->bRequest == U2PS_REQ_CUR) {
      if (u2ps_hmodel.stall_cur)
        return U2PS_HFAKE_ANSWER_STALL;
      put32(buf, u2ps_hmodel.cur);
      return reply(s, data, len, buf, u2ps_hmodel.short_cur ? 3u : 4u);
    }
    if (s->wValue == (U2PS_CS_CLOCK_VALID_CONTROL << 8) && s->bRequest == U2PS_REQ_CUR) {
      if (u2ps_hmodel.stall_valid)
        return U2PS_HFAKE_ANSWER_STALL;
      return reply(s, data, len, &u2ps_hmodel.valid, 1);
    }
  }
  return unexpected(s);
}

