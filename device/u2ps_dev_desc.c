/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dev_desc.c: the descriptor builder. It checks the app's description
 * once, at init, and builds each descriptor from it into the control buffer
 * when the host asks for it: the device and device_qualifier descriptors,
 * the high-speed configuration with the audio function, the minimal FS
 * configuration, either of them as other_speed_configuration, and the
 * strings.
 *
 * References: USB 2.0 Specification, Revision 2.0 (Chapter 9); UAC2 is the
 * USB Device Class Definition for Audio Devices, Release 2.0; Formats is
 * its Audio Data Formats, Release 2.0; IAD is the Interface Association
 * Descriptor ECN.
 *
 * The high-speed configuration (9.6.3), in this order:
 *   configuration, IAD (UAC2 4.6), standard AC interface (UAC2 4.7.1),
 *   class-specific AC header (4.7.2) followed by the Clock Source (4.7.2.1),
 *   the Input Terminal (4.7.2.4: the USB OUT endpoint) and the Output
 *   Terminal (4.7.2.5); AS interface alternate setting 0 without endpoints
 *   (4.9.1); then per format, alternate setting n: AS interface, AS general
 *   (4.9.2), Type I format (Formats 2.3.1.6), data endpoint (4.10.1.1),
 *   class-specific data endpoint (4.10.1.2), feedback endpoint (4.10.2.1).
 * The minimal FS configuration holds the audio function with its
 * AudioControl interface only: no AudioStreaming interface (UAC2 3.16.2 allows
 * zero), and so no clock or terminals, since a USB streaming terminal is
 * one that a streaming interface links to. Its AC header's wTotalLength
 * counts the header alone.
 */
#include <stddef.h>

#include "u2ps_ch9.h"
#include "u2ps_dev_priv.h"
#include "u2ps_uac2.h"

/* Lengths: up to and including AS alternate setting 0, and per format */
#define AC_BODY_LEN \
  (U2PS_CLOCK_SOURCE_LEN + U2PS_INPUT_TERM_LEN + U2PS_OUTPUT_TERM_LEN)
#define HEAD_LEN \
  (U2PS_CONFIG_LEN + U2PS_IAD_LEN + U2PS_ITF_LEN + U2PS_AC_HEADER_LEN)
#define HS_HEAD_LEN (HEAD_LEN + AC_BODY_LEN + U2PS_ITF_LEN)
#define ALT_LEN                                                                     \
  (U2PS_ITF_LEN + U2PS_AS_GENERAL_LEN + U2PS_FORMAT_I_LEN + U2PS_EP_LEN + \
   U2PS_CS_EP_LEN + U2PS_EP_LEN)

/* A string descriptor holds at most 126 UTF-16 code units: bLength is one
 * byte (9.6.7) */
#define STRING_CHARS_MAX 126u

static uint8_t *put8(uint8_t *p, unsigned v)
{
  *p = (uint8_t)v;
  return p + 1;
}

static uint8_t *put16(uint8_t *p, unsigned v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  return p + 2;
}

static uint8_t *put32(uint8_t *p, uint32_t v)
{
  p = put16(p, v & 0xFFFFu);
  return put16(p, v >> 16);
}

/* Number of characters of a string the description may hold, or -1 if it
 * is not printable ASCII or too long (U2PS_DESCRIPTION_MANUFACTURER) */
static int string_chars(const char *s)
{
  unsigned n = 0;
  if (s == NULL)
    return 0;
  for (; s[n] != '\0'; n++)
    if (n >= STRING_CHARS_MAX || s[n] < 0x20 || s[n] > 0x7E)
      return -1;
  return 2u + 2u * n <= U2PS_EP0_BUF ? (int)n : -1;
}

static const char *string_of(const u2ps_dev_description_t *d, uint8_t index)
{
  switch (index) {
  case 1:
    return d->manufacturer;
  case 2:
    return d->product;
  case 3:
    return d->serial_number;
  default:
    return NULL;
  }
}

static bool has_strings(const u2ps_dev_description_t *d)
{
  return d->manufacturer != NULL || d->product != NULL || d->serial_number != NULL;
}

/* The highest number of samples a microframe can carry: the highest rate's
 * samples per microframe, rounded up, and one more for the rate the
 * feedback asks the host for */
static uint32_t samples_max(const u2ps_dev_description_t *d)
{
  uint32_t top = d->sampling_frequencies[d->sampling_frequency_count - 1u];
  return top / 8000u + (top % 8000u != 0u) + 1u;
}

uint16_t u2ps_dev_mps(const u2ps_dev_description_t *d, const u2ps_format_t *f)
{
  return (uint16_t)(samples_max(d) * d->nr_channels * f->subslot_size);
}

/* A type an Output Terminal may have: not a USB terminal type (Table
 * 2-1), and none that the tables mark input-only ("I"): every Input
 * Terminal type (Table 2-2, p. 8) and the embedded types CD player 0x0703
 * and 0x0708 to 0x0710, Phonograph to Radio Receiver (Table 2-7,
 * p. 11-12). USB Device Class Definition for Terminal Types, Release 2.0,
 * May 31, 2006. */
static bool output_type(uint16_t t)
{
  if ((t >> 8) == 0x01u || (t >> 8) == 0x02u)
    return false;
  return t != 0x0703u && (t < 0x0708u || t > 0x0710u);
}

u2ps_dev_description_error_t u2ps_dev_description_check(const u2ps_dev_description_t *d)
{
  if (d == NULL)
    return U2PS_DESCRIPTION_NULL;
  if (d->formats == NULL)
    return U2PS_DESCRIPTION_FORMATS;
  if (d->format_count == 0u)
    return U2PS_DESCRIPTION_FORMAT_COUNT;
  if (d->sampling_frequencies == NULL)
    return U2PS_DESCRIPTION_SAMPLING_FREQUENCIES;
  /* RANGE: discrete rates as subranges [rate, rate, 0] in one Layout 3
   * block (UAC2 5.2.3.3) */
  if (d->sampling_frequency_count == 0u || 2u + 12u * d->sampling_frequency_count > U2PS_EP0_BUF)
    return U2PS_DESCRIPTION_SAMPLING_FREQUENCY_COUNT;
  if (d->nr_channels == 0u)
    return U2PS_DESCRIPTION_NR_CHANNELS;
  if (d->max_power_ma > 500u)
    return U2PS_DESCRIPTION_MAX_POWER_MA;
  /* VBUS mode bus is for a bus-powered device only (u2ps_dev.h) */
  if (U2PS_VBUS == U2PS_VBUS_BUS && d->self_powered)
    return U2PS_DESCRIPTION_SELF_POWERED;
  if (string_chars(d->manufacturer) < 0)
    return U2PS_DESCRIPTION_MANUFACTURER;
  if (string_chars(d->product) < 0)
    return U2PS_DESCRIPTION_PRODUCT;
  if (string_chars(d->serial_number) < 0)
    return U2PS_DESCRIPTION_SERIAL_NUMBER;
  /* The cluster (UAC2 4.1, p. 43): no more spatial locations than channels;
   * D27..D30 reserved; D31 (raw data) only with 0 channels, which a PCM
   * or IEEE_FLOAT stream cannot have */
  unsigned locations = 0;
  for (uint32_t m = d->channel_config; m != 0u; m &= m - 1u)
    locations++;
  if (locations > d->nr_channels || (d->channel_config & 0xF8000000u) != 0u)
    return U2PS_DESCRIPTION_CHANNEL_CONFIG;
  if (!output_type(d->terminal_type))
    return U2PS_DESCRIPTION_TERMINAL_TYPE;
  /* The rates in ascending order without overlap (UAC2 5.2.1) */
  if (d->sampling_frequencies[0] == 0u)
    return U2PS_DESCRIPTION_SAMPLING_FREQUENCY_ORDER;
  for (unsigned i = 1; i < d->sampling_frequency_count; i++)
    if (d->sampling_frequencies[i] <= d->sampling_frequencies[i - 1u])
      return U2PS_DESCRIPTION_SAMPLING_FREQUENCY_ORDER;
  /* The initial rate: 0 for the first, otherwise one of the rates */
  bool listed = d->initial_sampling_frequency == 0u;
  for (unsigned i = 0; i < d->sampling_frequency_count; i++)
    listed = listed || d->sampling_frequencies[i] == d->initial_sampling_frequency;
  if (!listed)
    return U2PS_DESCRIPTION_INITIAL_SAMPLING_FREQUENCY;
  /* High-speed isochronous payloads of at most 1024 bytes (USB 2.0 5.6.3;
   * the stack has no high-bandwidth endpoints), and no more than the port
   * is built for, U2PS_ISO_OUT_MPS_MAX (<= 1024). The product fits 32
   * bits: samples_max() is at most 2^32 / 8000 + 2. */
  for (unsigned i = 0; i < d->format_count; i++) {
    const u2ps_format_t *f = &d->formats[i];
    /* Formats 2.3.1.6: subslots of 1..4 bytes, bBitResolution <= 8 * subslot */
    if (f->subslot_size < 1u || f->subslot_size > 4u)
      return U2PS_DESCRIPTION_SUBSLOT_SIZE;
    if (f->bit_resolution < 1u || f->bit_resolution > 8u * f->subslot_size)
      return U2PS_DESCRIPTION_BIT_RESOLUTION;
    /* IEEE_FLOAT is 32 bits in 4-byte subslots (Formats 2.3.1.7.3) */
    if (f->audio_data_format == U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT) {
      if (f->subslot_size != 4u || f->bit_resolution != 32u)
        return U2PS_DESCRIPTION_AUDIO_DATA_FORMAT;
    } else if (f->audio_data_format != U2PS_AUDIO_DATA_FORMAT_PCM) {
      return U2PS_DESCRIPTION_AUDIO_DATA_FORMAT;
    }
    if (samples_max(d) * d->nr_channels * f->subslot_size > U2PS_ISO_OUT_MPS_MAX)
      return U2PS_DESCRIPTION_MAX_PACKET_SIZE;
  }
  if (HS_HEAD_LEN + (unsigned)ALT_LEN * d->format_count > U2PS_EP0_BUF)
    return U2PS_DESCRIPTION_CONFIGURATION_SIZE;
  return U2PS_DESCRIPTION_OK;
}

uint8_t u2ps_dev_itf_count(void)
{
  if (u2ps_dev.speed == U2PS_SPEED_HIGH)
    return 2u;
  return U2PS_REFUSE_FS ? 0u : 1u;
}

/* Device descriptor (9.6.1, Table 9-8). bcdUSB is 0200H, as a high-speed
 * capable device has (9.2.6.6); a higher version would announce a BOS
 * descriptor, which the stack does not serve. The class codes say that the
 * function is described at the interface level by an IAD (UAC2 4.2). One
 * descriptor serves both speeds, so bMaxPacketSize0 is the 64 that high
 * speed requires; one configuration per speed. */
uint16_t u2ps_dev_build_device(uint8_t *buf)
{
  const u2ps_dev_description_t *d = u2ps_dev.desc;
  uint8_t *p = buf;
  p = put8(p, U2PS_DEVICE_LEN);
  p = put8(p, U2PS_DESC_DEVICE);
  p = put16(p, 0x0200u);
  p = put8(p, U2PS_DEVICE_CLASS_MISC);
  p = put8(p, U2PS_DEVICE_SUBCLASS_IAD);
  p = put8(p, U2PS_DEVICE_PROTOCOL_IAD);
  p = put8(p, U2PS_EP0_MPS);
  p = put16(p, d->id_vendor);
  p = put16(p, d->id_product);
  p = put16(p, d->bcd_device);
  p = put8(p, d->manufacturer != NULL ? 1u : 0u);
  p = put8(p, d->product != NULL ? 2u : 0u);
  p = put8(p, d->serial_number != NULL ? 3u : 0u);
  p = put8(p, 1u);
  return (uint16_t)(p - buf);
}

/* The device at the other speed (9.6.2, Table 9-9): the device descriptor's
 * version and class codes (UAC2 4.3), EP0's 64 bytes and one configuration.
 * Only a device that enumerates at both speeds has it. */
uint16_t u2ps_dev_build_qualifier(uint8_t *buf)
{
  if (U2PS_REFUSE_FS)
    return 0;
  uint8_t *p = buf;
  p = put8(p, U2PS_QUALIFIER_LEN);
  p = put8(p, U2PS_DESC_DEVICE_QUALIFIER);
  p = put16(p, 0x0200u);
  p = put8(p, U2PS_DEVICE_CLASS_MISC);
  p = put8(p, U2PS_DEVICE_SUBCLASS_IAD);
  p = put8(p, U2PS_DEVICE_PROTOCOL_IAD);
  p = put8(p, U2PS_EP0_MPS);
  p = put8(p, 1u);
  p = put8(p, 0u);
  return (uint16_t)(p - buf);
}

/* Standard interface descriptor (9.6.5, Table 9-12) of the audio class
 * (UAC2 4.7.1, 4.9.1) */
static uint8_t *put_itf(uint8_t *p, uint8_t number, uint8_t alt, uint8_t endpoints,
                        uint8_t subclass)
{
  p = put8(p, U2PS_ITF_LEN);
  p = put8(p, U2PS_DESC_INTERFACE);
  p = put8(p, number);
  p = put8(p, alt);
  p = put8(p, endpoints);
  p = put8(p, U2PS_AUDIO);
  p = put8(p, subclass);
  p = put8(p, U2PS_IP_VERSION_02_00);
  return put8(p, 0u); /* iInterface */
}

/* Standard endpoint descriptor (9.6.6, Table 9-13) */
static uint8_t *put_ep(uint8_t *p, uint8_t address, uint8_t attributes, uint16_t mps,
                       uint8_t interval)
{
  p = put8(p, U2PS_EP_LEN);
  p = put8(p, U2PS_DESC_ENDPOINT);
  p = put8(p, address);
  p = put8(p, attributes);
  p = put16(p, mps);
  return put8(p, interval);
}

/* One streaming alternate setting with its format and endpoints */
static uint8_t *put_alt(uint8_t *p, const u2ps_dev_description_t *d, uint8_t alt)
{
  const u2ps_format_t *f = &d->formats[alt - 1u];

  p = put_itf(p, U2PS_ITF_AS, alt, 2u, U2PS_SUBCLASS_AUDIOSTREAMING);
  /* AS general (UAC2 Table 4-27): linked to the Input Terminal, no
   * Controls, Type I with the format's one Audio Data Format bit
   * (Formats Table A-2), the cluster of the Input Terminal */
  p = put8(p, U2PS_AS_GENERAL_LEN);
  p = put8(p, U2PS_CS_INTERFACE);
  p = put8(p, U2PS_AS_GENERAL);
  p = put8(p, U2PS_ID_INPUT_TERM);
  p = put8(p, 0u);
  p = put8(p, U2PS_FORMAT_TYPE_I);
  p = put32(p, f->audio_data_format == U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT ? U2PS_FORMAT_IEEE_FLOAT
                                                             : U2PS_FORMAT_PCM);
  p = put8(p, d->nr_channels);
  p = put32(p, d->channel_config);
  p = put8(p, 0u); /* iChannelNames */
  /* Type I format (Formats Table 2-2), the same for both Audio Data
   * Formats */
  p = put8(p, U2PS_FORMAT_I_LEN);
  p = put8(p, U2PS_CS_INTERFACE);
  p = put8(p, U2PS_AS_FORMAT_TYPE);
  p = put8(p, U2PS_FORMAT_TYPE_I);
  p = put8(p, f->subslot_size);
  p = put8(p, f->bit_resolution);
  /* Asynchronous isochronous data endpoint, every microframe (UAC2 Table
   * 4-33) */
  p = put_ep(p, U2PS_EP_DATA, U2PS_EP_ISO | U2PS_EP_ASYNC, u2ps_dev_mps(d, f),
             U2PS_EP_DATA_INTERVAL);
  /* Its class-specific descriptor (UAC2 Table 4-34): short packets are
   * fine (D7 = 0), no Controls; bLockDelayUnits and wLockDelay 0, as an
   * asynchronous endpoint must have them (UAC2 4.10.1.2, p. 87) */
  p = put8(p, U2PS_CS_EP_LEN);
  p = put8(p, U2PS_CS_ENDPOINT);
  p = put8(p, U2PS_EP_GENERAL);
  p = put8(p, 0u);
  p = put8(p, 0u);
  p = put8(p, 0u);
  p = put16(p, 0u);
  /* Explicit feedback endpoint (UAC2 Table 4-35), 4 bytes (USB 2.0
   * 5.12.4.2) */
  return put_ep(p, U2PS_EP_FB, U2PS_EP_ISO | U2PS_EP_USAGE_FB, U2PS_FB_SIZE,
                U2PS_EP_FB_INTERVAL);
}

uint16_t u2ps_dev_build_config(uint8_t *buf, bool high_speed, uint8_t type)
{
  const u2ps_dev_description_t *d = u2ps_dev.desc;
  uint8_t itfs = high_speed ? 2u : 1u;
  uint16_t total = high_speed ? (uint16_t)(HS_HEAD_LEN + ALT_LEN * d->format_count)
                              : (uint16_t)HEAD_LEN;
  uint8_t *p = buf;

  if (U2PS_REFUSE_FS && !high_speed)
    return 0;
  /* Configuration (9.6.3, Table 9-10): bmAttributes D7 is reserved and
   * set; D6 self-powered; no remote wakeup. bMaxPower in 2 mA units. */
  p = put8(p, U2PS_CONFIG_LEN);
  p = put8(p, type);
  p = put16(p, total);
  p = put8(p, itfs);
  p = put8(p, U2PS_CONFIG_VALUE);
  p = put8(p, 0u); /* iConfiguration */
  p = put8(p, 0x80u | (d->self_powered ? U2PS_CONFIG_SELF_POWERED : 0u));
  p = put8(p, (d->max_power_ma + 1u) / 2u);
  /* IAD (UAC2 Table 4-3) over all interfaces of the function */
  p = put8(p, U2PS_IAD_LEN);
  p = put8(p, U2PS_DESC_IAD);
  p = put8(p, U2PS_ITF_AC);
  p = put8(p, itfs);
  p = put8(p, U2PS_AUDIO);
  p = put8(p, U2PS_FUNCTION_SUBCLASS_UNDEFINED);
  p = put8(p, U2PS_IP_VERSION_02_00);
  p = put8(p, 0u); /* iFunction */
  p = put_itf(p, U2PS_ITF_AC, 0u, 0u, U2PS_SUBCLASS_AUDIOCONTROL);
  /* Class-specific AC header (UAC2 Table 4-5) */
  p = put8(p, U2PS_AC_HEADER_LEN);
  p = put8(p, U2PS_CS_INTERFACE);
  p = put8(p, U2PS_AC_HEADER);
  p = put16(p, U2PS_BCD_ADC);
  p = put8(p, d->category);
  p = put16(p, U2PS_AC_HEADER_LEN + (high_speed ? AC_BODY_LEN : 0u));
  p = put8(p, 0u); /* bmControls */
  if (high_speed) {
    /* Clock Source (UAC2 Table 4-6): internal, programmable by the host;
     * Clock Frequency Control read-write, Clock Validity Control read */
    p = put8(p, U2PS_CLOCK_SOURCE_LEN);
    p = put8(p, U2PS_CS_INTERFACE);
    p = put8(p, U2PS_AC_CLOCK_SOURCE);
    p = put8(p, U2PS_ID_CLOCK);
    p = put8(p, U2PS_CLOCK_INTERNAL_PROGRAMMABLE);
    p = put8(p, (U2PS_CTRL_READ_WRITE << U2PS_CLOCK_FREQ_POS) |
                    (U2PS_CTRL_READ << U2PS_CLOCK_VALID_POS));
    p = put8(p, 0u); /* bAssocTerminal */
    p = put8(p, 0u); /* iClockSource */
    /* Input Terminal (UAC2 Table 4-9): the USB OUT endpoint, clocked by
     * the Clock Source, with the function's channel cluster */
    p = put8(p, U2PS_INPUT_TERM_LEN);
    p = put8(p, U2PS_CS_INTERFACE);
    p = put8(p, U2PS_AC_INPUT_TERMINAL);
    p = put8(p, U2PS_ID_INPUT_TERM);
    p = put16(p, U2PS_TERMINAL_USB_STREAMING);
    p = put8(p, 0u); /* bAssocTerminal */
    p = put8(p, U2PS_ID_CLOCK);
    p = put8(p, d->nr_channels);
    p = put32(p, d->channel_config);
    p = put8(p, 0u);  /* iChannelNames */
    p = put16(p, 0u); /* bmControls */
    p = put8(p, 0u);  /* iTerminal */
    /* Output Terminal (UAC2 Table 4-10), fed by the Input Terminal */
    p = put8(p, U2PS_OUTPUT_TERM_LEN);
    p = put8(p, U2PS_CS_INTERFACE);
    p = put8(p, U2PS_AC_OUTPUT_TERMINAL);
    p = put8(p, U2PS_ID_OUTPUT_TERM);
    p = put16(p, d->terminal_type);
    p = put8(p, 0u); /* bAssocTerminal */
    p = put8(p, U2PS_ID_INPUT_TERM);
    p = put8(p, U2PS_ID_CLOCK);
    p = put16(p, 0u); /* bmControls */
    p = put8(p, 0u);  /* iTerminal */
    /* AudioStreaming alternate setting 0: no endpoints, no bandwidth */
    p = put_itf(p, U2PS_ITF_AS, 0u, 0u, U2PS_SUBCLASS_AUDIOSTREAMING);
    for (unsigned alt = 1; alt <= d->format_count; alt++)
      p = put_alt(p, d, (uint8_t)alt);
  }
  return (uint16_t)(p - buf);
}

/* String descriptors (9.6.7): 0 lists the one LANGID, 1..3 are the
 * description's strings in UTF-16LE. None at all if the description has no
 * strings. */
uint16_t u2ps_dev_build_string(uint8_t *buf, uint8_t index)
{
  const u2ps_dev_description_t *d = u2ps_dev.desc;
  uint8_t *p = buf + 2;

  if (!has_strings(d))
    return 0;
  if (index == 0u) {
    p = put16(p, U2PS_LANGID);
  } else {
    const char *s = string_of(d, index);
    if (s == NULL)
      return 0;
    for (; *s != '\0'; s++)
      p = put16(p, (uint8_t)*s);
  }
  buf[0] = (uint8_t)(p - buf);
  buf[1] = U2PS_DESC_STRING;
  return buf[0];
}
