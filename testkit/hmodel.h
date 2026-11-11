/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * hmodel.h: a device model for the fake host port (fake_hport.h): a UAC2
 * sink that answers the host stack's control transfers from descriptors
 * it keeps, its clock's controls, SET_INTERFACE and GET_STATUS. The PC test kit plays
 * its scripted device with it, and the host tests change it field by
 * field.
 */
#ifndef U2PS_HMODEL_H
#define U2PS_HMODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "fake_hport.h"

/* The longest configuration descriptor the model sends: the largest
 * wTotalLength (USB 2.0 9.6.3), so a test of the host stack at its largest
 * U2PS_HOST_EP0_BUF, 65472, can send one byte more than that */
#define U2PS_HMODEL_CONFIG_MAX 0xFFFFu

typedef struct {
  uint8_t device[18];                        /* its device descriptor */
  uint8_t config[2][U2PS_HMODEL_CONFIG_MAX]; /* configuration descriptors by index */
  uint16_t config_len[2];
  /* Its device_qualifier (USB 2.0 9.6.2), as sent; length 0: none, a
   * full-speed-only device, which answers the request with a STALL */
  uint8_t qualifier[10];
  uint16_t qualifier_len;
  /* Other_speed_configuration descriptors by index (9.6.4), as sent; a
   * request for one before the device_qualifier is unexpected (9.6.2) */
  uint8_t other[2][U2PS_HMODEL_CONFIG_MAX];
  uint16_t other_len[2];
  /* String descriptors by index 0..3, as sent; length 0: none (STALL) */
  uint8_t string[4][256];
  uint16_t string_len[4];
  uint8_t range[1200]; /* RANGE of the Sampling Frequency Control, as sent */
  uint16_t range_len;
  uint32_t cur;        /* CUR of the Sampling Frequency Control; SET CUR sets it */
  uint8_t valid;       /* CUR of the Clock Validity Control */
  bool stall_range, stall_cur, stall_valid;
  bool stall_set_cur;  /* SET CUR stalls */
  bool fixed_cur;      /* SET CUR completes but leaves cur as it is */
  bool short_cur;      /* GET CUR of the Sampling Frequency Control answers 3 bytes */
  int stall_alt;       /* SET_INTERFACE to this alternate setting stalls; -1: none */
  uint8_t alt;         /* the alternate setting SET_INTERFACE selected */
  unsigned set_interfaces; /* SET_INTERFACE requests */
  unsigned get_status;     /* GET_STATUS(device) requests answered */
  uint8_t itf_as;      /* the AudioStreaming interface SET_INTERFACE must name */
  uint16_t clock_windex; /* wIndex of the clock's requests: Clock Source ID << 8 | AC interface */
  int hang_at;           /* the nth control transfer (0 first) gets no answer; -1: none */
  int stall_at;          /* the nth control transfer stalls; -1: none */
  int error_at;          /* the nth control transfer fails with errors; -1: none */
  /* State and counts */
  uint8_t address;       /* SET_ADDRESS sets it; requests to another address fail */
  uint8_t configuration; /* SET_CONFIGURATION sets it */
  bool qualified;        /* the device_qualifier was sent */
  unsigned requests;     /* control transfers seen */
  unsigned unexpected;   /* requests the model does not know (each is printed) */
} u2ps_hmodel_t;
extern u2ps_hmodel_t u2ps_hmodel;

/* The default device: device descriptor bcdUSB 0x0200, class EF/02/01,
 * bMaxPacketSize0 64, VID 0x1234, PID 0x5678, bcdDevice 0x0100, strings 1,
 * 2, 3 ("Maker", "Player Test", "SN-0001" in LANGID 0x0409), one
 * configuration: a UAC2 sink of 2 channels whose alternate settings 1 to
 * 4 carry the formats 16/2, 24/4, 32/4 and 24/3 (bits/bytes), each with
 * an asynchronous data endpoint and explicit feedback; RANGE of the ten
 * rates 44.1 to 768 kHz as discrete sub-ranges, CUR 48000, Clock Validity
 * 1, clock on wIndex 0x0400, AudioStreaming interface 1 */
void u2ps_hmodel_default(void);
/* A configuration descriptor given as bytes */
void u2ps_hmodel_set_config(uint8_t index, const uint8_t *desc, uint16_t len);
/* A device_qualifier from the device descriptor for the other speed:
 * bMaxPacketSize0 64 and n other-speed configurations */
void u2ps_hmodel_set_qualifier(uint8_t n);
/* An other-speed configuration given as a configuration descriptor's
 * bytes: it is sent with bDescriptorType OTHER_SPEED_CONFIGURATION */
void u2ps_hmodel_set_other(uint8_t index, const uint8_t *desc, uint16_t len);
/* RANGE of n discrete sub-ranges [rates[i], rates[i], 0] */
void u2ps_hmodel_set_rates(const uint32_t *rates, unsigned n);
/* The string descriptor of index i from ASCII text; NULL removes it */
void u2ps_hmodel_set_string(uint8_t index, const char *text);

/* The model as u2ps_hfake_device */
u2ps_hfake_answer_t u2ps_hmodel_answer(uint8_t addr, const u2ps_hfake_setup_t *setup, uint8_t *data,
                                       uint16_t *len);

#endif /* U2PS_HMODEL_H */
