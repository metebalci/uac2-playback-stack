/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_host_walk.c: the descriptor walk and the RANGE reader
 * (u2ps_host_priv.h). The walk keeps no state of its own: the caller's
 * u2ps_host_walk_t holds the result and the scratch memory.
 *
 * A first pass checks every descriptor's length against wTotalLength and
 * finds the audio IADs, each of which bounds an audio function (UAC2 4.6,
 * Table 4-3): entity IDs are unique only within a function. For each
 * function in turn, until one plays:
 * - a pass over the function's interfaces collects its AudioControl
 *   interface, its Clock Entities, Terminals and Units, and which
 *   AudioStreaming interface links to which terminal;
 * - the USB streaming Input Terminal with a linked AudioStreaming interface
 *   gives the clock and the Output Terminal on its path;
 * - a pass over that AudioStreaming interface reads its alternate settings,
 *   kept in ascending order, each skipped with a reason if the stack cannot
 *   play it.
 * A malformed descriptor refuses the configuration; an alternate setting
 * the stack cannot play only is skipped.
 *
 * References: USB 2.0 Specification, Revision 2.0 (9.6); UAC2 is the USB
 * Device Class Definition for Audio Devices, Release 2.0 (4.6 to 4.10,
 * 5.2); Formats is its Audio Data Formats, Release 2.0 (2.3.1); Terminal
 * Types is its Terminal Types, Release 2.0.
 */
#include <string.h>

#include "u2ps_ch9.h"
#include "u2ps_host_priv.h"
#include "u2ps_uac2.h"

/* A high-speed isochronous transaction carries at most 1024 bytes (USB 2.0
 * 5.6.3); feedback is 4 bytes, 16.16, at high speed (5.12.4.2) */
#define ISO_PACKET_MAX 1024u
#define FEEDBACK_LEN   4u

/* An interface number past every real one: the scope of a configuration
 * without an IAD */
#define ITF_END 256u

static const u2ps_walk_entity_t *find_entity(const u2ps_host_walk_t *w, uint8_t id)
{
  for (unsigned i = 0; i < w->n_entities; i++)
    if (w->entities[i].id == id)
      return &w->entities[i];
  return NULL;
}

static void add_entity(u2ps_host_walk_t *w, u2ps_walk_entity_t e)
{
  if (w->n_entities < U2PS_WALK_ENTITY_MAX)
    w->entities[w->n_entities++] = e;
}

/* First pass: the lengths of all descriptors; the number of audio IADs
 * and of AudioControl interfaces */
static u2ps_host_refusal_t check_lengths(const uint8_t *desc, uint16_t total, unsigned *iads,
                                     unsigned *acs)
{
  *iads = *acs = 0;
  for (unsigned off = 0; off < total;) {
    if (total - off < 2u)
      return U2PS_HOST_REFUSAL_CUT;
    const uint8_t *d = desc + off;
    unsigned bl = d[0];
    if (bl < 2u)
      return U2PS_HOST_REFUSAL_LENGTH;
    if (bl > total - off)
      return U2PS_HOST_REFUSAL_CUT;
    if (off == 0u) {
      if (bl < U2PS_CONFIG_LEN)
        return U2PS_HOST_REFUSAL_LENGTH;
    } else if (d[1] == U2PS_DESC_INTERFACE) {
      if (bl < U2PS_ITF_LEN)
        return U2PS_HOST_REFUSAL_LENGTH;
      if (d[3] == 0u && d[5] == U2PS_AUDIO && d[6] == U2PS_SUBCLASS_AUDIOCONTROL)
        (*acs)++;
    } else if (d[1] == U2PS_DESC_ENDPOINT) {
      if (bl < U2PS_EP_LEN)
        return U2PS_HOST_REFUSAL_LENGTH;
    } else if (d[1] == U2PS_DESC_IAD) {
      if (bl < U2PS_IAD_LEN)
        return U2PS_HOST_REFUSAL_LENGTH;
      if (d[4] == U2PS_AUDIO)
        (*iads)++;
    }
    off += bl;
  }
  return U2PS_HOST_REFUSAL_NONE;
}

/* The function's AudioControl interface, its entities and the links of
 * its AudioStreaming interfaces: interfaces lo..hi-1 */
static u2ps_host_refusal_t collect(const uint8_t *desc, uint16_t total, unsigned lo, unsigned hi,
                               u2ps_host_walk_t *w)
{
  uint8_t cls = 0, sub = 0, proto = 0, itf = 0;
  bool in_scope = false, in_ac = false;

  for (unsigned off = desc[0]; off < total; off += desc[off]) {
    const uint8_t *d = desc + off;
    unsigned bl = d[0];
    if (d[1] == U2PS_DESC_INTERFACE) {
      itf = d[2];
      cls = d[5];
      sub = d[6];
      proto = d[7];
      in_scope = itf >= lo && itf < hi;
      in_ac = false;
      if (in_scope && cls == U2PS_AUDIO && sub == U2PS_SUBCLASS_AUDIOCONTROL &&
          (!w->ac_found || w->itf_ac == itf)) {
        w->ac_found = true;
        w->itf_ac = itf;
        w->ac_protocol = proto;
        in_ac = true;
      }
      continue;
    }
    if (d[1] != U2PS_CS_INTERFACE || !in_scope || cls != U2PS_AUDIO ||
        proto != U2PS_IP_VERSION_02_00)
      continue;
    if (bl < 3u)
      return U2PS_HOST_REFUSAL_LENGTH;
    uint8_t subtype = d[2];
    if (in_ac) {
      switch (subtype) {
      case U2PS_AC_HEADER:
        if (bl < U2PS_AC_HEADER_LEN)
          return U2PS_HOST_REFUSAL_LENGTH;
        w->header = true;
        w->bcd_adc = u2ps_le16(d + 3);
        break;
      case U2PS_AC_INPUT_TERMINAL:
        if (bl < U2PS_INPUT_TERM_LEN)
          return U2PS_HOST_REFUSAL_LENGTH;
        add_entity(w, (u2ps_walk_entity_t){
                        .subtype = subtype, .id = d[3], .a = d[7], .type = u2ps_le16(d + 4)});
        break;
      case U2PS_AC_OUTPUT_TERMINAL:
        if (bl < U2PS_OUTPUT_TERM_LEN)
          return U2PS_HOST_REFUSAL_LENGTH;
        add_entity(w, (u2ps_walk_entity_t){
                        .subtype = subtype, .id = d[3], .a = d[7], .type = u2ps_le16(d + 4)});
        break;
      case U2PS_AC_FEATURE_UNIT:
        if (bl < U2PS_FEATURE_UNIT_LEN)
          return U2PS_HOST_REFUSAL_LENGTH;
        add_entity(w, (u2ps_walk_entity_t){.subtype = subtype, .id = d[3], .a = d[4]});
        break;
      case U2PS_AC_CLOCK_SOURCE:
        if (bl < U2PS_CLOCK_SOURCE_LEN)
          return U2PS_HOST_REFUSAL_LENGTH;
        add_entity(w, (u2ps_walk_entity_t){.subtype = subtype, .id = d[3], .a = d[5]});
        break;
      case U2PS_AC_CLOCK_SELECTOR:
        if (bl < U2PS_CLOCK_SELECTOR_LEN || bl < U2PS_CLOCK_SELECTOR_LEN + (unsigned)d[4])
          return U2PS_HOST_REFUSAL_LENGTH;
        add_entity(w, (u2ps_walk_entity_t){
                        .subtype = subtype, .id = d[3], .a = d[4], .b = d[4] > 0u ? d[5] : 0u});
        break;
      case U2PS_AC_MIXER_UNIT:
      case U2PS_AC_SELECTOR_UNIT:
      case U2PS_AC_EFFECT_UNIT:
      case U2PS_AC_PROCESSING_UNIT:
      case U2PS_AC_EXTENSION_UNIT:
      case U2PS_AC_CLOCK_MULTIPLIER:
      case U2PS_AC_SAMPLE_RATE_CONVERTER:
        /* Another entity: only its ID, which ends a path to the Output
         * Terminal */
        if (bl < 4u)
          return U2PS_HOST_REFUSAL_LENGTH;
        add_entity(w, (u2ps_walk_entity_t){.subtype = subtype, .id = d[3]});
        break;
      default:
        break;
      }
    } else if (sub == U2PS_SUBCLASS_AUDIOSTREAMING && subtype == U2PS_AS_GENERAL) {
      if (bl < U2PS_AS_GENERAL_LEN)
        return U2PS_HOST_REFUSAL_LENGTH;
      if (w->n_links < U2PS_WALK_LINK_MAX) {
        w->link_itf[w->n_links] = itf;
        w->link_terminal[w->n_links++] = d[3];
      }
    }
  }
  return U2PS_HOST_REFUSAL_NONE;
}

/* wTerminalType of the first Output Terminal that the Input Terminal 'it'
 * feeds directly or through Feature Units only (UAC2 Tables 4-10, 4-13); 0
 * if none. A path is never longer than the entities, so a loop ends. */
static uint16_t output_terminal_type(const u2ps_host_walk_t *w, uint8_t it)
{
  for (unsigned i = 0; i < w->n_entities; i++) {
    const u2ps_walk_entity_t *ot = &w->entities[i];
    if (ot->subtype != U2PS_AC_OUTPUT_TERMINAL)
      continue;
    uint8_t id = ot->a;
    for (unsigned steps = 0; steps < w->n_entities; steps++) {
      if (id == it)
        return ot->type;
      const u2ps_walk_entity_t *e = find_entity(w, id);
      if (e == NULL || e->subtype != U2PS_AC_FEATURE_UNIT)
        break;
      id = e->a;
    }
  }
  return 0;
}

/* An alternate setting of the AudioStreaming interface, while the walk is
 * in it */
typedef struct {
  u2ps_host_alternate_setting_t a;
  u2ps_host_alt_ep_t ep;
  uint8_t endpoints; /* bNumEndpoints */
  uint8_t seen;      /* endpoint descriptors seen */
  bool general;      /* AS general seen */
  bool format;       /* format type seen */
  bool data;         /* data endpoint seen */
  bool fb;           /* feedback endpoint seen */
  uint8_t fb_addr, fb_interval;
  uint16_t fb_mps;   /* wMaxPacketSize as declared */
  /* Why it cannot play */
  bool not_pcm_or_float, high_bandwidth, interval, implicit, max_packets_only, packet_size;
} alt_walk_t;

static u2ps_host_refusal_t alt_descriptor(alt_walk_t *aw, const uint8_t *d)
{
  unsigned bl = d[0];
  if ((d[1] == U2PS_CS_INTERFACE || d[1] == U2PS_CS_ENDPOINT) && bl < 3u)
    return U2PS_HOST_REFUSAL_LENGTH;
  if (d[1] == U2PS_CS_INTERFACE && d[2] == U2PS_AS_GENERAL) {
    if (bl < U2PS_AS_GENERAL_LEN)
      return U2PS_HOST_REFUSAL_LENGTH;
    uint32_t formats = u2ps_le32(d + 6);
    /* Type I PCM, or IEEE_FLOAT; PCM where a device declares both
     * (Formats A.2.1) */
    if (d[5] != U2PS_FORMAT_TYPE_I)
      aw->not_pcm_or_float = true;
    else if ((formats & U2PS_FORMAT_PCM) != 0u)
      aw->a.format.audio_data_format = U2PS_AUDIO_DATA_FORMAT_PCM;
    else if ((formats & U2PS_FORMAT_IEEE_FLOAT) != 0u)
      aw->a.format.audio_data_format = U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT;
    else
      aw->not_pcm_or_float = true;
    aw->a.nr_channels = d[10];
    aw->a.channel_config = u2ps_le32(d + 11);
    aw->general = true;
  } else if (d[1] == U2PS_CS_INTERFACE && d[2] == U2PS_AS_FORMAT_TYPE) {
    if (bl < U2PS_FORMAT_I_LEN)
      return U2PS_HOST_REFUSAL_LENGTH;
    aw->format = true;
    if (d[3] != U2PS_FORMAT_TYPE_I) {
      aw->not_pcm_or_float = true;
    } else {
      /* Formats 2.3.1.6: subslots of 1..4 bytes, bBitResolution <= 8 *
       * bSubslotSize */
      uint8_t slot = d[4], res = d[5];
      if (slot < 1u || slot > 4u || res == 0u || res > 8u * slot)
        return U2PS_HOST_REFUSAL_FORMAT;
      aw->a.format.subslot_size = slot;
      aw->a.format.bit_resolution = res;
    }
  } else if (d[1] == U2PS_CS_ENDPOINT && d[2] == U2PS_EP_GENERAL) {
    if (bl < U2PS_CS_EP_LEN)
      return U2PS_HOST_REFUSAL_LENGTH;
    if ((d[3] & U2PS_EP_MAX_PACKETS_ONLY) != 0u)
      aw->max_packets_only = true;
  } else if (d[1] == U2PS_DESC_ENDPOINT) {
    uint8_t addr = d[2], attr = d[3], interval = d[6];
    uint16_t mps = u2ps_le16(d + 4);
    unsigned usage = (attr >> U2PS_EP_USAGE_POS) & 3u;
    aw->seen++;
    if ((attr & U2PS_EP_TYPE_MASK) != U2PS_EP_ISO)
      return U2PS_HOST_REFUSAL_ENDPOINT;
    if (usage == U2PS_EP_USAGE_IMPLICIT) {
      aw->implicit = true;
    } else if (usage == U2PS_EP_USAGE_DATA) {
      if ((addr & U2PS_EP_DIR_IN) != 0u || aw->data)
        return U2PS_HOST_REFUSAL_ENDPOINT;
      aw->data = true;
      aw->ep.ep_out = addr;
      aw->a.max_packet_size = (uint16_t)(mps & 0x7FFu);
      aw->a.synchronization_type = (u2ps_synchronization_type_t)((attr >> U2PS_EP_SYNC_POS) & 3u);
      if (((mps >> 11) & 3u) != 0u)
        aw->high_bandwidth = true;
      if (interval != 1u)
        aw->interval = true;
      if ((mps & 0x7FFu) > ISO_PACKET_MAX)
        aw->packet_size = true;
    } else if (usage == U2PS_EP_USAGE_FEEDBACK) {
      if (aw->fb)
        return U2PS_HOST_REFUSAL_ENDPOINT;
      aw->fb = true;
      aw->fb_addr = addr;
      aw->fb_mps = mps;
      aw->fb_interval = interval;
    } else {
      return U2PS_HOST_REFUSAL_ENDPOINT;
    }
  }
  return U2PS_HOST_REFUSAL_NONE;
}

static void keep(u2ps_host_walk_t *w, const alt_walk_t *aw)
{
  uint8_t alt = aw->a.alternate_setting;
  if (w->alt_count == U2PS_HOST_ALT_MAX) {
    /* Full: the lowest-numbered alternate settings stay */
    w->alt_dropped++;
    if (alt > w->alts[U2PS_HOST_ALT_MAX - 1u].alternate_setting)
      return;
    w->alt_count--;
  }
  unsigned i = w->alt_count++;
  /* Never true (a full list dropped one above); it bounds i for the
   * compiler, which otherwise sees alts[1] at U2PS_HOST_ALT_MAX 1 */
  if (i >= U2PS_HOST_ALT_MAX)
    return;
  for (; i > 0u && w->alts[i - 1u].alternate_setting > alt; i--) {
    w->alts[i] = w->alts[i - 1u];
    w->eps[i] = w->eps[i - 1u];
  }
  w->alts[i] = aw->a;
  w->eps[i] = aw->ep;
}

static u2ps_host_refusal_t alt_finish(alt_walk_t *aw, u2ps_host_walk_t *w, bool *alt0)
{
  u2ps_host_alternate_setting_t *a = &aw->a;
  if (aw->seen != aw->endpoints)
    return U2PS_HOST_REFUSAL_ENDPOINT;
  if (a->alternate_setting == 0u) {
    if (aw->endpoints != 0u)
      return U2PS_HOST_REFUSAL_NO_STREAMING;
    *alt0 = true;
    return U2PS_HOST_REFUSAL_NONE;
  }
  if (!aw->general || !aw->format)
    return U2PS_HOST_REFUSAL_FORMAT;
  if (!aw->data)
    return U2PS_HOST_REFUSAL_ENDPOINT;
  /* Explicit feedback only for an asynchronous endpoint (USB 2.0 9.6.6);
   * adaptive, synchronous and No Synchronization play at the nominal rate,
   * and a feedback endpoint they declare is ignored */
  if (a->synchronization_type == U2PS_SYNCHRONIZATION_TYPE_ASYNCHRONOUS) {
    if (aw->fb) {
      if ((aw->fb_addr & U2PS_EP_DIR_IN) == 0u || (aw->fb_mps >> 11) != 0u ||
          (aw->fb_mps & 0x7FFu) < FEEDBACK_LEN || aw->fb_interval == 0u || aw->fb_interval > 16u)
        return U2PS_HOST_REFUSAL_ENDPOINT;
      a->explicit_feedback = true;
      aw->ep.ep_fb = aw->fb_addr;
      aw->ep.fb_mps = (uint16_t)(aw->fb_mps & 0x7FFu);
      aw->ep.fb_interval = aw->fb_interval;
    } else {
      aw->implicit = true;
    }
  }
  /* IEEE_FLOAT is 32 bits in 4-byte subslots (Formats 2.3.1.7.3) */
  if (a->format.audio_data_format == U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT &&
      (a->format.subslot_size != 4u || a->format.bit_resolution != 32u))
    aw->not_pcm_or_float = true;
  if (a->nr_channels == 0u)
    aw->not_pcm_or_float = true;
  a->skip = aw->not_pcm_or_float  ? U2PS_HOST_SKIP_FORMAT
            : aw->high_bandwidth  ? U2PS_HOST_SKIP_HIGH_BANDWIDTH
            : aw->interval        ? U2PS_HOST_SKIP_INTERVAL
            : aw->implicit        ? U2PS_HOST_SKIP_IMPLICIT_FEEDBACK
            : aw->max_packets_only ? U2PS_HOST_SKIP_MAX_PACKETS_ONLY
            : aw->packet_size     ? U2PS_HOST_SKIP_PACKET_SIZE
                                  : U2PS_HOST_SKIP_NONE;
  uint8_t bit = (uint8_t)(1u << (a->alternate_setting & 7u));
  if ((w->alt_seen[a->alternate_setting >> 3] & bit) != 0u)
    return U2PS_HOST_REFUSAL_ALT_REPEATED;
  w->alt_seen[a->alternate_setting >> 3] |= bit;
  keep(w, aw);
  return U2PS_HOST_REFUSAL_NONE;
}

/* The alternate settings of the AudioStreaming interface w->itf_as */
static u2ps_host_refusal_t alternate_settings(const uint8_t *desc, uint16_t total,
                                          u2ps_host_walk_t *w)
{
  alt_walk_t aw;
  bool in = false, alt0 = false;
  u2ps_host_refusal_t e;

  for (unsigned off = desc[0]; off < total; off += desc[off]) {
    const uint8_t *d = desc + off;
    if (d[1] == U2PS_DESC_INTERFACE) {
      if (in && (e = alt_finish(&aw, w, &alt0)) != U2PS_HOST_REFUSAL_NONE)
        return e;
      in = d[2] == w->itf_as;
      if (in) {
        memset(&aw, 0, sizeof aw);
        aw.a.alternate_setting = d[3];
        aw.endpoints = d[4];
      }
    } else if (in && (e = alt_descriptor(&aw, d)) != U2PS_HOST_REFUSAL_NONE) {
      return e;
    }
  }
  if (in && (e = alt_finish(&aw, w, &alt0)) != U2PS_HOST_REFUSAL_NONE)
    return e;
  if (!alt0)
    return U2PS_HOST_REFUSAL_NO_STREAMING;
  if (w->alt_count == 0u)
    return U2PS_HOST_REFUSAL_NO_ALT;
  for (unsigned i = 0; i < w->alt_count; i++)
    if (w->alts[i].skip == U2PS_HOST_SKIP_NONE)
      return U2PS_HOST_REFUSAL_NONE;
  return U2PS_HOST_REFUSAL_ALL_SKIPPED;
}

/* The audio function on interfaces lo..hi-1 */
static u2ps_host_refusal_t function(const uint8_t *desc, uint16_t total, unsigned lo, unsigned hi,
                                u2ps_host_walk_t *w)
{
  memset(w, 0, sizeof *w);
  u2ps_host_refusal_t e = collect(desc, total, lo, hi, w);
  if (e != U2PS_HOST_REFUSAL_NONE)
    return e;
  if (!w->ac_found)
    return U2PS_HOST_REFUSAL_NOT_AUDIO;
  if (w->ac_protocol == U2PS_IP_UNDEFINED)
    return U2PS_HOST_REFUSAL_UAC1;
  if (w->ac_protocol != U2PS_IP_VERSION_02_00 || !w->header || w->bcd_adc != U2PS_BCD_ADC)
    return U2PS_HOST_REFUSAL_VERSION;

  /* The first USB streaming Input Terminal with an AudioStreaming
   * interface linked to it */
  const u2ps_walk_entity_t *it = NULL;
  bool streaming_terminal = false;
  for (unsigned i = 0; i < w->n_entities && it == NULL; i++) {
    const u2ps_walk_entity_t *e2 = &w->entities[i];
    if (e2->subtype != U2PS_AC_INPUT_TERMINAL || e2->type != U2PS_TERMINAL_USB_STREAMING)
      continue;
    streaming_terminal = true;
    for (unsigned k = 0; k < w->n_links; k++)
      if (w->link_terminal[k] == e2->id && w->link_itf[k] != w->itf_ac) {
        it = e2;
        w->itf_as = w->link_itf[k];
        break;
      }
  }
  if (!streaming_terminal)
    return U2PS_HOST_REFUSAL_NO_TERMINAL;
  if (it == NULL)
    return U2PS_HOST_REFUSAL_NO_STREAMING;

  /* Its clock: a Clock Source, or a Clock Selector with one input that is
   * one. With more inputs the device picks the clock, so the requests could
   * go to the wrong one. */
  const u2ps_walk_entity_t *source = find_entity(w, it->a);
  if (source != NULL && source->subtype == U2PS_AC_CLOCK_SELECTOR)
    source = source->a == 1u ? find_entity(w, source->b) : NULL;
  if (source == NULL || source->subtype != U2PS_AC_CLOCK_SOURCE)
    return U2PS_HOST_REFUSAL_CLOCK;

  w->terminal = it->id;
  w->terminal_type = output_terminal_type(w, it->id);
  w->clock = source->id;
  /* bmControls D1..0 Clock Frequency, D3..2 Clock Validity (UAC2 Table
   * 4-6) */
  w->clock_programmable =
    ((source->a >> U2PS_CLOCK_FREQ_POS) & 3u) == U2PS_CTRL_READ_WRITE;
  w->clock_valid_readable = ((source->a >> U2PS_CLOCK_VALID_POS) & 3u) != 0u;
  return alternate_settings(desc, total, w);
}

u2ps_host_refusal_t u2ps_host_walk(const uint8_t *desc, uint16_t len, u2ps_host_walk_t *w)
{
  memset(w, 0, sizeof *w);
  if (len < U2PS_CONFIG_LEN)
    return U2PS_HOST_REFUSAL_CUT;
  /* An other-speed configuration has a configuration's layout (USB 2.0
   * 9.6.4) */
  if (desc[1] != U2PS_DESC_CONFIGURATION && desc[1] != U2PS_DESC_OTHER_SPEED)
    return U2PS_HOST_REFUSAL_NOT_CONFIG;
  uint16_t total = u2ps_le16(desc + 2);
  if (total > len)
    return U2PS_HOST_REFUSAL_CUT;
  if (total < U2PS_CONFIG_LEN)
    return U2PS_HOST_REFUSAL_LENGTH;
  unsigned iads, acs;
  u2ps_host_refusal_t e = check_lengths(desc, total, &iads, &acs);
  if (e != U2PS_HOST_REFUSAL_NONE)
    return e;

  /* No audio IAD: the whole configuration is the function, but only with
   * the option and one AudioControl interface (a deviation from UAC2 4.6) */
  if (iads == 0u) {
    if (acs == 0u)
      return U2PS_HOST_REFUSAL_NOT_AUDIO;
    if (!U2PS_HOST_ACCEPT_NO_IAD || acs != 1u)
      return U2PS_HOST_REFUSAL_NO_IAD;
    return function(desc, total, 0, ITF_END, w);
  }

  /* Each audio IAD's function in turn: the first that plays, otherwise the
   * first one's reason, with its result */
  unsigned first_lo = 0, first_hi = 0;
  u2ps_host_refusal_t first = U2PS_HOST_REFUSAL_NONE;
  for (unsigned off = desc[0]; off < total; off += desc[off]) {
    const uint8_t *d = desc + off;
    if (d[1] != U2PS_DESC_IAD || d[4] != U2PS_AUDIO)
      continue;
    unsigned lo = d[2], hi = (unsigned)d[2] + d[3];
    e = function(desc, total, lo, hi, w);
    if (e == U2PS_HOST_REFUSAL_NONE)
      return e;
    if (first == U2PS_HOST_REFUSAL_NONE) {
      first = e;
      first_lo = lo;
      first_hi = hi;
    }
  }
  if (iads > 1u)
    function(desc, total, first_lo, first_hi, w);
  return first;
}

uint16_t u2ps_host_range(const uint8_t *reply, uint16_t len, u2ps_host_sub_range_t *out,
                         uint16_t max)
{
  if (len < 2u + 12u)
    return 0;
  unsigned n = u2ps_le16(reply);
  if (n > (len - 2u) / 12u)
    n = (len - 2u) / 12u;
  if (n > max)
    n = max;
  for (unsigned i = 0; i < n; i++) {
    const uint8_t *p = reply + 2u + 12u * i;
    out[i].min = u2ps_le32(p);
    out[i].max = u2ps_le32(p + 4);
    out[i].res = u2ps_le32(p + 8);
  }
  return (uint16_t)n;
}
