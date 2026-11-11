/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * dev_support.h: what the device stack tests share: the descriptor fixtures,
 * the test description for u2ps_dev_init() with the descriptors it must
 * give, the app callbacks with a replaceable request handler and a log of
 * the stream, and short forms of common requests.
 */
#ifndef DEV_SUPPORT_H
#define DEV_SUPPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "fake_dport.h"
#include "fixture.h"
#include "u2ps_ch9.h"
#include "u2ps_dev.h"

/* The test description: VID 0x1234, PID 0x5678, bcdDevice 0x0100;
 * manufacturer, product and serial strings of 7, 31 and 63 characters;
 * bus-powered, 100 mA; I/O box, speaker, 2 channels (front left, right);
 * the formats 16/2, 24/4, 32/4, 24/3 (bits/bytes) of
 * uac2-hs-default.txt; the rates 44.1 to 768 kHz (test_rates).
 * test_desc_load() fills it; tests may change it and call it again. */
extern u2ps_dev_description_t test_desc;
extern const u2ps_format_t test_formats[4];
extern const uint32_t test_rates[10];
#define TEST_RATE_COUNT 10u
#define TEST_LANGID 0x0409u

/* What the description must give: the device descriptor, the HS
 * configuration uac2-hs-default.txt, the minimal FS configuration
 * uac2-fs-minimal.txt, and string descriptors 0 (LANGID 0x0409), 1 (16
 * bytes), 2 (64 bytes) and 3 (128 bytes) */
extern uint8_t test_device[18];
extern uint8_t test_config_hs[512];
extern uint16_t test_config_hs_len;
extern uint8_t test_config_fs[512];
extern uint16_t test_config_fs_len;
extern const uint8_t *const test_strings[4];

/* Load the fixtures and fill test_desc; false if a fixture is missing */
bool test_desc_load(void);

/* Reset the fake, report VBUS present (VBUS mode app), init the stack with
 * test_desc (a re-init after the first) at its initial sampling
 * frequency, clear the app's log, connect, and reset the bus at
 * 'speed'. */
void dev_start(u2ps_speed_t speed);

/* The app callbacks record here. 'events' lists the stream callbacks in
 * order, one letter each: S start, P packet, H hole, Z pause, X stop,
 * F fault, R rate; and C configured, D unconfigured, B bus reset, U
 * suspend, W resume, V VBUS present, v VBUS absent. */
typedef struct {
  unsigned resets, suspends, resumes, requests;
  u2ps_request_t last;             /* the last request seen */
  uint8_t last_data[U2PS_EP0_BUF]; /* its host-to-device data */

  char events[256];
  unsigned nevents;
  unsigned starts, stops, packets, rates;
  unsigned faults[4];                /* per u2ps_fault_t */
  bool fault_masked;                 /* the vectors were masked in the last on_fault */
  unsigned configured, unconfigured; /* on_configured(true) and (false) calls */
  u2ps_config_end_t config_end;       /* 'why' of the last on_configured */
  bool vbus_masked;                   /* the vectors were masked in the last on_vbus */
  unsigned calls_at_vbus;             /* port calls logged before the last on_vbus */
  u2ps_speed_t speed_at_configured;   /* u2ps_dev_speed() inside the last on_configured(true) */
  u2ps_fake_stream_state_t state_at_start; /* the fake's stream state inside the last start */
  unsigned opens_at_start;           /* stream_open calls logged before it */
  u2ps_stream_info_t info;           /* of the last start */
  u2ps_stream_cfg_t cfg;             /* given at the last start */
  uint32_t fb_offered;               /* the fb_value the stack offered then */
  const uint8_t *packet_data;        /* the last packet */
  uint16_t packet_len;
  u2ps_uframe_t packet_uframe;
  uint8_t packet_first;              /* its first byte, if any */
  unsigned nholes;
  struct {
    uint16_t uframes;
    u2ps_miss_t why;
  } holes[16];
  uint32_t hole_total[4];            /* microframes of every on_hole, by u2ps_miss_t */
  bool hole_masked;                  /* the vectors were masked in the last on_hole */
  unsigned npauses;
  uint16_t pauses[8];
  uint32_t rate;                     /* the last rate offered (chosen) */
  uint32_t rate_requested;           /* the value the host sent with it */
  uint32_t rate_id;
} app_log_t;
extern app_log_t app_log;

/* What the app does; dev_start() sets the defaults */
typedef struct {
  bool decline;        /* on_stream_start returns U2PS_STREAM_REFUSE (default false) */
  bool defer;          /* on_stream_start returns U2PS_STREAM_DEFER (default false) */
  bool release_each;   /* release each packet inside on_packet (default true) */
  uint16_t count;      /* buffers lent (default 4) */
  uint32_t fb_value;   /* first feedback value; 0 keeps the stack's (default 0) */
  int rate_result;     /* what on_sampling_frequency returns (default 0: accept) */
  int start_result;    /* if not 0, what on_stream_start returns instead (default 0) */
} app_cfg_t;
extern app_cfg_t app_cfg;

/* The class memory the app lends: large enough for 8 buffers of 1024 */
extern uint8_t app_mem[8 * 1024];

/* u2ps_app_on_packet() calls this for each packet, after its log and
 * before the release, if set; dev_start() clears it */
extern void (*app_packet_hook)(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe);

/* Every callback that logs an event letter (app_log_t.events) calls this
 * with the letter, at the callback's start, if set; dev_start() clears
 * it */
extern void (*app_event_hook)(char event);

/* u2ps_app_on_request() calls this; NULL answers every request with a
 * request error */
extern int (*app_handler)(const u2ps_request_t *req);

/* The deferred start of the last on_stream_start: u2ps_dev_stream_open()
 * with the class memory on_stream_start would have lent */
u2ps_open_result_t app_open_deferred(void);

/* Short forms of u2ps_fake_host_control() */
u2ps_fake_host_result_t get_descriptor(uint8_t type, uint8_t index, uint16_t windex, uint16_t wlength,
                             uint8_t *data, uint16_t *actual);
u2ps_fake_host_result_t set_address(uint8_t addr);
u2ps_fake_host_result_t set_configuration(uint8_t value);
u2ps_fake_host_result_t set_interface(uint8_t itf, uint8_t alt);
/* SET CUR and GET CUR of the clock's Sampling Frequency Control */
u2ps_fake_host_result_t set_rate(uint32_t rate);
u2ps_fake_host_result_t get_rate(uint32_t *rate);

/* Address the device and configure it, at the current speed */
void dev_configure(void);

#endif /* DEV_SUPPORT_H */
