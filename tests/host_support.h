/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * host_support.h: what the host stack tests share: the app callbacks with a
 * log, the device model (hmodel.h) loaded from fixtures, and a
 * list of descriptors to build and change configuration descriptors with.
 */
#ifndef HOST_SUPPORT_H
#define HOST_SUPPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "fake_hport.h"
#include "fixture.h"
#include "hmodel.h"
#include "u2ps_host.h"

/* ---- The app ---------------------------------------------------------------- */

/* The app callbacks record here. 'events' lists them in order, one letter
 * each, as long as it has room: D on_device, E on_end, S stream start, X
 * stream stop, s sent, h hole, T on_test_device, R on_resumed. The device
 * given at on_device is copied, with its strings and arrays. */
typedef struct {
  char events[256];
  unsigned nevents;
  unsigned devices, ends;
  u2ps_host_end_t end;      /* the reason of the last on_end */
  u2ps_host_refusal_t refusal; /* its refusal reason */
  u2ps_hfake_from_t end_from;    /* where the last on_end ran: masked thread or upcall */
  unsigned calls_at_end;         /* port calls logged before the last on_end */
  u2ps_hfake_from_t device_from; /* where the last on_device ran */
  u2ps_host_device_t dev;        /* the last device, pointing into the copies below */
  const u2ps_host_device_t *dev_given; /* the pointer on_device gave */
  char manufacturer[128], product[128], serial_number[128];
  u2ps_host_sub_range_t sub_ranges[64];
  u2ps_host_alternate_setting_t alternate_settings[32];
  /* The stream */
  unsigned stream_starts, stream_stops;
  u2ps_host_stream_info_t info;  /* of the last on_stream_start */
  u2ps_host_stop_t stop;         /* the reason of the last on_stream_stop */
  u2ps_host_stop_request_t stop_request; /* its request */
  u2ps_hfake_from_t stop_from;   /* where it ran */
  unsigned calls_at_stop;        /* port calls logged before it */
  unsigned queued_at_stop;       /* packets in the fake port's queue at it */
  unsigned sent;                 /* on_sent calls */
  u2ps_uframe_t sent_uframe;     /* of the last */
  unsigned nholes;               /* on_hole calls; the first HAPP_HOLES kept */
  struct {
    uint16_t uframes;
    u2ps_miss_t why;
    unsigned sent_before; /* on_sent calls before it */
  } holes[16];
  /* The test device and the resume */
  unsigned test_devices;          /* on_test_device calls */
  uint16_t test_pid, test_bcd;    /* the last one's arguments */
  u2ps_hfake_from_t test_from;    /* where it ran */
  uint32_t test_ms;               /* the fake's time at it */
  unsigned calls_at_test;         /* port calls logged before it */
  unsigned resumes;               /* on_resumed calls */
  bool resumed_remote;            /* the last one's argument */
  u2ps_hfake_from_t resumed_from;
  uint32_t resumed_ms;
  unsigned calls_at_resumed;
} happ_log_t;
#define HAPP_HOLES 16u
extern happ_log_t happ_log;

/* What the app does beyond its log; happ_clear() sets them to NULL. Each
 * runs inside its callback, after the log. */
extern void (*happ_stream_start_hook)(const u2ps_host_stream_info_t *info);
extern void (*happ_sent_hook)(u2ps_uframe_t uframe);
extern void (*happ_hole_hook)(uint16_t uframes, u2ps_miss_t why);
extern void (*happ_stream_stop_hook)(u2ps_host_stop_t why);
extern void (*happ_end_hook)(u2ps_host_end_t why);

/* Clear the app's log and its hooks */
void happ_clear(void);
/* The log holds an on_device or an on_end */
bool happ_done(void);

/* ---- A player and a sink for the stream ------------------------------------------ */

/* The bytes every stream carries: byte i of the stream is hgen(i) */
uint8_t hgen(uint64_t i);

/* The player: from on_stream_start on, and at each on_sent, it queues
 * packets of the size the stack gives until the port's queue holds
 * 'depth' packets, each in its own aligned buffer, filled from hgen().
 * hplayer_start() installs it as the app's stream start and sent hooks. */
#define HPLAYER_LOG 65536u
typedef struct {
  bool on;              /* it queues (default true) */
  unsigned depth;       /* default 4 */
  uint32_t stream;
  uint16_t slot_bytes;  /* nr_channels * subslot_size of the stream */
  uint64_t bytes;       /* bytes queued */
  unsigned packets;     /* packets queued */
  uint64_t slots;       /* audio slots queued */
  unsigned refused;     /* queue calls that returned false */
  unsigned sent;        /* on_sent calls */
  uint64_t bytes_sent;  /* bytes of the packets reported sent */
  uint16_t log[HPLAYER_LOG]; /* each packet's slots, by its number since the start */
  u2ps_uframe_t sent_uframe[HPLAYER_LOG]; /* each packet's on_sent microframe */
} hplayer_t;
extern hplayer_t hplayer;
void hplayer_start(void);
/* Queue up to 'depth' packets now (thread, or inside a callback) */
void hplayer_fill(void);

/* The sink: checks that the bytes it gets follow hgen() without a gap, and
 * logs each packet's length and microframe */
typedef struct {
  uint64_t bytes;       /* bytes received (or skipped) */
  unsigned packets;
  unsigned bad;         /* packets whose bytes did not follow */
  uint16_t len[HPLAYER_LOG];
  u2ps_uframe_t uframe[HPLAYER_LOG];
} hsink_t;
extern hsink_t hsink;
void hsink_clear(void);
/* A packet arrives */
void hsink_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe);
/* A packet of len bytes is lost on the bus: its bytes are skipped */
void hsink_lost(uint16_t len, u2ps_uframe_t uframe);

/* ---- The device model (hmodel.h) --------------------------------------------- */

/* The default device of u2ps_hmodel_default(), with the configuration of
 * 'fixture' instead (none if NULL). Returns false if the fixture is
 * missing. */
bool hmodel_load(const char *fixture);
/* The model's other-speed configuration 'index' (USB 2.0 9.6.4) from
 * 'fixture', a configuration; false if the fixture is missing */
bool hmodel_load_other(uint8_t index, const char *fixture);

/* The ten rates of the test device, 44.1 to 768 kHz */
extern const uint32_t test_rates_host[10];

/* Reset the fake and the app's log, load the model with 'fixture' (as
 * u2ps_hfake_device), report VBUS present, init the stack (a re-init after the
 * first) and allow sessions. False if the fixture is missing. */
bool hstart(const char *fixture);
/* Connect the device and step until on_device or on_end, at most 20 s */
bool henumerate(void);

/* ---- Descriptor lists ---------------------------------------------------------- */

#define DL_MAX 200u

/* A configuration descriptor as a list of descriptors. Each keeps its
 * own length, which a test may set apart from its bLength. */
typedef struct {
  uint8_t d[DL_MAX][256];
  uint16_t len[DL_MAX];
  unsigned n;
} dl_t;

/* Split bytes into descriptors by bLength; false if they do not split */
bool dl_from(dl_t *l, const uint8_t *bytes, unsigned len);
/* A fixture as a list; false if missing */
bool dl_load(dl_t *l, const char *fixture);
/* Index of the nth descriptor of 'type' (and, if subtype >= 0, subtype in
 * byte 2), or -1 */
int dl_find(const dl_t *l, uint8_t type, int subtype, unsigned nth);
/* Index of the nth OUT endpoint (bEndpointAddress D7 clear), or -1 */
int dl_data_ep(const dl_t *l, unsigned nth);
/* Insert a descriptor of len bytes before index 'at' */
void dl_insert(dl_t *l, unsigned at, const uint8_t *desc, unsigned len);
/* Insert the descriptor given in hex text before 'at' */
void dl_insert_hex(dl_t *l, unsigned at, const char *hex);
/* Remove the descriptor at 'at' */
void dl_remove(dl_t *l, unsigned at);
/* Cut the descriptor at 'at' to len bytes, bLength too */
void dl_shorten(dl_t *l, unsigned at, unsigned len);
/* Keep only descriptors [0, n) */
void dl_truncate(dl_t *l, unsigned n);
/* Append descriptors [from, to) of src */
void dl_append(dl_t *l, const dl_t *src, unsigned from, unsigned to);
/* Join into out (at most max bytes); fix_total writes wTotalLength. Returns
 * the length. */
unsigned dl_join(const dl_t *l, uint8_t *out, unsigned max, bool fix_total);
/* Set a descriptor's 16- and 32-bit little-endian fields */
void dl_put16(dl_t *l, unsigned at, unsigned off, uint16_t v);
void dl_put32(dl_t *l, unsigned at, unsigned off, uint32_t v);

#endif /* HOST_SUPPORT_H */
