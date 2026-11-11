/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dev_std.c: the standard requests (USB 2.0 9.4), the descriptors of
 * both speeds (9.2.6.6, 9.6) and test mode (9.4.9, 7.1.20). References are
 * to the USB 2.0 Specification, Revision 2.0.
 *
 * Where 9.4 leaves the device's behaviour unspecified (the Default state for
 * most requests, SET_ADDRESS when configured, malformed fields), the device
 * answers with a request error.
 */
#include <string.h>

#include "u2ps_ch9.h"
#include "u2ps_dev_priv.h"

#define STALL U2PS_REQ_STALL

/* Whether interface 'itf' of the current configuration has alternate
 * setting 'alt': the AudioControl interface only 0, the AudioStreaming
 * interface 0 and one per format */
static bool has_alt(uint8_t itf, uint8_t alt)
{
  if (itf == U2PS_ITF_AS)
    return alt <= u2ps_dev.desc->format_count;
  return alt == 0u;
}

/* An interface of the current configuration (9.3.4 Figure 9-3, 9.6.5):
 * interfaces exist only in the Configured state */
static bool itf_exists(uint16_t windex)
{
  return u2ps_dev.state == U2PS_DEV_CONFIGURED && (windex >> 8) == 0u &&
         (windex & 0xFFu) < u2ps_dev_itf_count();
}

/* An endpoint (9.3.4 Figure 9-2): EP0 in either direction in any state;
 * otherwise, in the Configured state, an endpoint of the selected alternate
 * setting of its interface (9.4.5: one that does not exist is a request
 * error). Only the streaming alternate settings have endpoints, the data
 * endpoint and its feedback endpoint. bEndpointAddress carries the
 * direction, so 0x01 and 0x81 differ. */
static bool ep_exists(uint16_t windex)
{
  if ((windex & 0xFF7Fu) == 0u)
    return true;
  return u2ps_dev.state == U2PS_DEV_CONFIGURED && u2ps_dev.alt[U2PS_ITF_AS] != 0u &&
         (windex == U2PS_EP_DATA || windex == U2PS_EP_FB);
}

/* A LANGID listed in string descriptor 0 (9.6.7) */
static bool langid_ok(uint16_t langid)
{
  return langid == U2PS_LANGID;
}

/* 9.4.3. The descriptor builder writes the descriptor into the control
 * buffer. The other-speed descriptors exist only if the device enumerates
 * at both speeds; with U2PS_REFUSE_FS, nothing is served at full speed. */
static int get_descriptor(const u2ps_setup_t *s)
{
  uint8_t *buf = u2ps_dev_ep0_buf;
  uint8_t type = (uint8_t)(s->wValue >> 8);
  uint8_t index = (uint8_t)s->wValue;
  bool high = u2ps_dev.speed == U2PS_SPEED_HIGH;
  uint16_t len = 0;

  if (s->bmRequestType != (U2PS_REQ_DIR_IN | U2PS_REQ_RCPT_DEVICE))
    return STALL;
  if (U2PS_REFUSE_FS && !high)
    return STALL;

  switch (type) {
  case U2PS_DESC_DEVICE:
    if (index == 0u)
      len = u2ps_dev_build_device(buf);
    break;
  case U2PS_DESC_CONFIGURATION:
    if (index == 0u)
      len = u2ps_dev_build_config(buf, high, type);
    break;
  case U2PS_DESC_OTHER_SPEED:
    /* other_speed_configuration has a configuration's structure (9.6.4) */
    if (index == 0u)
      len = u2ps_dev_build_config(buf, !high, type);
    break;
  case U2PS_DESC_STRING:
    if (index == 0u || langid_ok(s->wIndex))
      len = u2ps_dev_build_string(buf, index);
    break;
  case U2PS_DESC_DEVICE_QUALIFIER:
    if (index == 0u)
      len = u2ps_dev_build_qualifier(buf);
    break;
  default:
    break;
  }
  if (len == 0u)
    return STALL;

  /* Only the initial bytes if wLength is shorter (9.4.3) */
  if (len > s->wLength)
    len = s->wLength;
  return len;
}

/* 9.4.5 */
static int get_status(const u2ps_setup_t *s)
{
  uint8_t *buf = u2ps_dev_ep0_buf;
  uint8_t status = 0;

  switch (s->bmRequestType & U2PS_REQ_RCPT_MASK) {
  case U2PS_REQ_RCPT_DEVICE:
    /* Self Powered as the description declares it, not measured. Remote
     * Wakeup is never enabled: the stack does not support it. */
    if (u2ps_dev.desc->self_powered)
      status |= U2PS_STATUS_SELF_POWERED;
    break;
  case U2PS_REQ_RCPT_ITF:
    if (!itf_exists(s->wIndex))
      return STALL;
    break;
  case U2PS_REQ_RCPT_EP:
    /* No endpoint is ever halted */
    if (!ep_exists(s->wIndex))
      return STALL;
    break;
  default:
    return STALL;
  }
  buf[0] = status;
  buf[1] = 0;
  return 2;
}

/* SET_FEATURE (9.4.9) and CLEAR_FEATURE (9.4.1) */
static int feature(const u2ps_setup_t *s, bool set)
{
  switch (s->bmRequestType & U2PS_REQ_RCPT_MASK) {
  case U2PS_REQ_RCPT_DEVICE:
    /* DEVICE_REMOTE_WAKEUP is not supported: no configuration declares it
     * (u2ps_dev_desc.c), so it falls to the request error below */
    if (s->wValue == U2PS_FEATURE_TEST_MODE && set) {
      /* Device recipient, low byte of wIndex zero, a selector of Table 9-7
       * that applies to an upstream facing port, and at high speed only
       * (7.1.20). Test mode cannot be cleared (9.4.1). */
      uint8_t selector = (uint8_t)(s->wIndex >> 8);
      if (u2ps_dev.speed != U2PS_SPEED_HIGH || (s->wIndex & 0xFFu) != 0u ||
          selector < U2PS_TEST_J || selector > U2PS_TEST_PACKET)
        return STALL;
      u2ps_dev.after = U2PS_AFTER_TEST_MODE;
      u2ps_dev.after_arg = selector;
      return 0;
    }
    return STALL;
  case U2PS_REQ_RCPT_EP:
    /* ENDPOINT_HALT cannot be set: EP0 need not have it and the isochronous
     * endpoints do not (9.4.5, p. 256: required for interrupt and bulk
     * endpoints only). Clearing it on an existing endpoint succeeds and
     * changes nothing. Strictly, clearing a feature that does not exist is a
     * request error (9.4.1); success is the tolerant choice for hosts that
     * clear halts on every endpoint. */
    if (s->wValue == U2PS_FEATURE_ENDPOINT_HALT && !set && ep_exists(s->wIndex))
      return 0;
    return STALL;
  default:
    /* USB 2.0 defines no interface feature (Table 9-6) */
    return STALL;
  }
}

/* 9.4.6. The state changes at the SETUP: the port may report the status
 * completion only after the next SETUP, which cancels whatever was left to
 * do (a ChipIdea interrupt serves a SETUP before completions, IMXRT1060RM
 * Rev. 3 42.5.6.7.1, Table 42-75). The port applies the bus address after
 * the status stage. Address 0 returns the device to the Default state. */
static int set_address(const u2ps_setup_t *s)
{
  if (s->wValue > U2PS_ADDRESS_MAX || s->wIndex != 0u ||
      u2ps_dev.state == U2PS_DEV_CONFIGURED)
    return STALL;
  u2ps_dev.address = (uint8_t)s->wValue;
  u2ps_dev.state = u2ps_dev.address ? U2PS_DEV_ADDRESS : U2PS_DEV_DEFAULT;
  u2ps_dev.after = U2PS_AFTER_ADDRESS;
  u2ps_dev.after_arg = u2ps_dev.address;
  return 0;
}

/* 9.4.7. Configuring resets every interface to alternate setting 0
 * (9.1.1.5), which closes a stream. The app hears of entering and leaving
 * the Configured state, not of a configuration selected again. */
static int set_configuration(const u2ps_setup_t *s)
{
  uint8_t value = (uint8_t)s->wValue;

  if (u2ps_dev.state == U2PS_DEV_DEFAULT || (s->wValue >> 8) != 0u || s->wIndex != 0u)
    return STALL;
  if (value != 0u && (value != U2PS_CONFIG_VALUE || u2ps_dev_itf_count() == 0u))
    return STALL;
  u2ps_dev.config = value;
  memset(u2ps_dev.alt, 0, sizeof u2ps_dev.alt);
  (void)u2ps_dev_audio_set_alt(0);
  if (value == 0u) {
    u2ps_dev_unconfigure(U2PS_CONFIG_END_SET_CONFIGURATION);
  } else if (u2ps_dev.state != U2PS_DEV_CONFIGURED) {
    u2ps_dev.state = U2PS_DEV_CONFIGURED;
    u2ps_app_on_configured(true, U2PS_CONFIG_END_NONE);
  }
  return 0;
}

/* 9.4.10. On the AudioStreaming interface it starts or closes the stream:
 * the status stage of a streaming alternate setting waits for the
 * stream's open, a refused start stalls, and the interface takes the
 * alternate setting only when the stream opens (u2ps_dev_audio.c). */
static int set_interface(const u2ps_setup_t *s)
{
  if (!itf_exists(s->wIndex) || (s->wValue >> 8) != 0u)
    return STALL;
  uint8_t itf = (uint8_t)s->wIndex;
  uint8_t alt = (uint8_t)s->wValue;
  if (!has_alt(itf, alt))
    return STALL;
  if (itf == U2PS_ITF_AS)
    return u2ps_dev_audio_set_alt(alt);
  u2ps_dev.alt[itf] = alt;
  return 0;
}

int u2ps_dev_std_request(const u2ps_setup_t *s)
{
  uint8_t *buf = u2ps_dev_ep0_buf;
  bool in = (s->bmRequestType & U2PS_REQ_DIR_IN) != 0u;

  /* The direction each request has in Table 9-3 */
  switch (s->bRequest) {
  case U2PS_GET_STATUS:
  case U2PS_GET_DESCRIPTOR:
  case U2PS_GET_CONFIGURATION:
  case U2PS_GET_INTERFACE:
  case U2PS_SYNCH_FRAME:
    if (!in)
      return STALL;
    break;
  default:
    if (in)
      return STALL;
    break;
  }

  switch (s->bRequest) {
  case U2PS_GET_STATUS:
    return get_status(s);
  case U2PS_CLEAR_FEATURE:
    return feature(s, false);
  case U2PS_SET_FEATURE:
    return feature(s, true);
  case U2PS_SET_ADDRESS:
    if (s->bmRequestType != U2PS_REQ_RCPT_DEVICE)
      return STALL;
    return set_address(s);
  case U2PS_GET_DESCRIPTOR:
    return get_descriptor(s);
  case U2PS_GET_CONFIGURATION:
    if (s->bmRequestType != (U2PS_REQ_DIR_IN | U2PS_REQ_RCPT_DEVICE))
      return STALL;
    buf[0] = u2ps_dev.config;
    return 1;
  case U2PS_SET_CONFIGURATION:
    if (s->bmRequestType != U2PS_REQ_RCPT_DEVICE)
      return STALL;
    return set_configuration(s);
  case U2PS_GET_INTERFACE:
    if (s->bmRequestType != (U2PS_REQ_DIR_IN | U2PS_REQ_RCPT_ITF) || !itf_exists(s->wIndex))
      return STALL;
    buf[0] = u2ps_dev.alt[s->wIndex];
    return 1;
  case U2PS_SET_INTERFACE:
    if (s->bmRequestType != U2PS_REQ_RCPT_ITF)
      return STALL;
    return set_interface(s);
  default:
    /* SET_DESCRIPTOR is optional (9.4.8). SYNCH_FRAME is for implicit
     * pattern synchronization, which no endpoint here uses (9.4.11). */
    return STALL;
  }
}
