/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dev_priv.h: state and internal calls shared by the files of the
 * device stack. Not part of the app API.
 */
#ifndef U2PS_DEV_PRIV_H
#define U2PS_DEV_PRIV_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_dev.h"
#include "u2ps_dport.h"

/* The audio function's fixed layout, which the description does not set:
 * interface 0 is the AudioControl interface, interface 1 the AudioStreaming
 * interface (UAC2 4.6: the AudioControl interface first, the streaming
 * interfaces right after it). The entity IDs are unique within the function
 * (UAC2 4.7.2). */
#define U2PS_DEV_ITF_MAX     2u
#define U2PS_ITF_AC          0u
#define U2PS_ITF_AS          1u
#define U2PS_ID_INPUT_TERM   1u
#define U2PS_ID_OUTPUT_TERM  3u
#define U2PS_ID_CLOCK        4u
#define U2PS_CONFIG_VALUE    1u
#define U2PS_EP_DATA         0x01u /* bEndpointAddress: EP1 OUT */
#define U2PS_EP_FB           0x81u /* EP1 IN */
#define U2PS_EP_DATA_INTERVAL 1u   /* every microframe */
#define U2PS_EP_FB_INTERVAL   4u   /* 2^(4-1) = 8 microframes (USB 2.0 Table 9-13) */
#define U2PS_FB_SIZE          4u   /* 16.16 at high speed (USB 2.0 5.12.4.2) */
#define U2PS_LANGID           0x0409u

/* Device states (USB 2.0 9.1.1, Table 9-1). Suspended is kept apart. */
typedef enum {
  U2PS_DEV_DEFAULT,
  U2PS_DEV_ADDRESS,
  U2PS_DEV_CONFIGURED
} u2ps_dev_state_t;

/* Control pipe stages */
typedef enum {
  U2PS_CTRL_IDLE,     /* no request, or the last one finished or stalled */
  U2PS_CTRL_DATA_IN,  /* ep0_in armed */
  U2PS_CTRL_DATA_OUT, /* ep0_out armed */
  U2PS_CTRL_STATUS,   /* ep0_status armed */
  U2PS_CTRL_PENDING,  /* waiting for u2ps_dev_reply(); EP0 NAKs */
  U2PS_CTRL_HELD      /* SET_INTERFACE's status waits for the stream's open; EP0 NAKs */
} u2ps_ctrl_stage_t;

/* What the stack does once the status stage has completed */
typedef enum {
  U2PS_AFTER_NONE,
  U2PS_AFTER_ADDRESS,  /* SET_ADDRESS: tell the port the address before the status stage */
  U2PS_AFTER_TEST_MODE /* enter test mode */
} u2ps_after_t;

/* The stream, from the device stack's side */
typedef enum {
  U2PS_DEV_STREAM_IDLE,   /* none open; class memory is the app's */
  U2PS_DEV_STREAM_OPEN,   /* stream_open succeeded */
  U2PS_DEV_STREAM_CLOSING, /* stream_close called, on_closed not yet come */
  U2PS_DEV_STREAM_STARTING /* the app deferred the start: u2ps_dev_stream_open() opens it */
} u2ps_stream_state_t;

typedef struct {
  const u2ps_dev_description_t *desc;
  bool in_upcall; /* inside an upcall, or a masked region that calls the app: no mask */
  bool allowed;   /* u2ps_dev_connect(true) and no detach since */
  bool attached;  /* the pull-up is on: allowed and VBUS present */
  bool reset_seen; /* a bus reset has come since the pull-up went on: suspend is reported */
  bool speed_known; /* on_speed has come since the last reset: u2ps_dev_speed() */
  u2ps_speed_t speed; /* for the requests: FULL until on_speed */
  u2ps_dev_state_t state;
  bool suspended; /* a suspend was reported and no resume since */
  uint8_t address;
  uint8_t config;                /* bConfigurationValue, 0 if not configured */
  uint8_t alt[U2PS_DEV_ITF_MAX]; /* alternate setting per interface */

  /* Control pipe */
  u2ps_setup_t setup;
  u2ps_ctrl_stage_t stage;
  u2ps_after_t after;
  uint8_t after_arg; /* the address or the test selector */
  uint32_t id;       /* of the current request; a new SETUP or reset moves it on */
  bool held_timed;     /* held_start holds the first tick of the held status */
  uint32_t held_start; /* ms */

  /* Clock */
  uint32_t rate;         /* the Sampling Frequency Control's CUR, Hz */
  uint32_t rate_pending; /* a SET CUR the app deferred */
  bool rate_deferred;    /* rate_pending waits for u2ps_dev_reply() */

  /* Stream */
  u2ps_stream_state_t stream;
  uint32_t stream_no;         /* number of the last stream opened; 0 before the first */
  uint32_t held;              /* packets of that stream the app holds */
  uint32_t release_errors;    /* releases ignored */
  uint32_t uframe_back;       /* packets not after the last one's microframe */
  uint32_t packets;           /* packets of this stream */
  uint32_t tick_packets;      /* packets when quiet_start was taken */
  bool quiet_timed;           /* quiet_start is valid */
  bool long_gap;              /* a tick saw no packet for more than the pause bound */
  uint32_t quiet_start;       /* ms: first tick that saw the stream's last packet */
  uint32_t quiet_ms;          /* ms without a new packet, as of the last tick */
  uint8_t open_alt;           /* alternate setting to start once the close ends; 0: none */
  uint8_t start_alt;          /* alternate setting of the last start */
  uint32_t start_rate;        /* the clock's rate at the last start */
  bool close_timed;           /* close_start holds the first tick of this close */
  bool close_fault;           /* U2PS_FAULT_CLOSE returned for this close */
  uint32_t close_start;       /* ms */
  u2ps_uframe_t last_uframe;  /* of the last packet, or U2PS_UFRAME_UNKNOWN */
  uint32_t missed[U2PS_MISS_CORRUPTED + 1]; /* hole reports since the last packet, per reason */
  uint32_t reported;          /* microframes reported at long gaps since the last packet;
                                 a stream's first packet has no gap to count it in */
} u2ps_dev_t;

extern u2ps_dev_t u2ps_dev;

/* VBUS as the stack knows it. While it is absent the upcalls ignore every
 * port event (u2ps_dev.h, u2ps_dev_vbus). Mode bus: always present. */
#if U2PS_VBUS == U2PS_VBUS_APP
extern bool u2ps_dev_vbus_on;
#define U2PS_DEV_VBUS_ON() u2ps_dev_vbus_on
#else
#define U2PS_DEV_VBUS_ON() true
#endif

/* The control buffer: every EP0 data stage uses it */
extern uint8_t u2ps_dev_ep0_buf[U2PS_EP0_BUF];

/* Read a little-endian 16-bit field */
static inline uint16_t u2ps_dev_get16(const uint8_t *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

/* ---- Descriptor builder (u2ps_dev_desc.c) ---- */

/* The first rule a description breaks, or U2PS_DESCRIPTION_OK if it is
 * valid (u2ps_dev_init) */
u2ps_dev_description_error_t u2ps_dev_description_check(const u2ps_dev_description_t *d);

/* wMaxPacketSize of the data endpoint for one format of the description */
uint16_t u2ps_dev_mps(const u2ps_dev_description_t *d, const u2ps_format_t *f);

/* Number of interfaces of the configuration at the current speed; 0 if the
 * device has none at this speed (U2PS_REFUSE_FS at full speed) */
uint8_t u2ps_dev_itf_count(void);

/* Each builder writes one descriptor into buf (at least U2PS_EP0_BUF
 * bytes) and returns its length, or 0 if the device has no such
 * descriptor. */
uint16_t u2ps_dev_build_device(uint8_t *buf);
uint16_t u2ps_dev_build_qualifier(uint8_t *buf);
/* The configuration of one speed, of descriptor type 'type'
 * (CONFIGURATION, or OTHER_SPEED_CONFIGURATION, USB 2.0 9.6.4) */
uint16_t u2ps_dev_build_config(uint8_t *buf, bool high_speed, uint8_t type);
uint16_t u2ps_dev_build_string(uint8_t *buf, uint8_t index);

/* ---- Standard requests (u2ps_dev_std.c) ---- */

/* A standard request. Returns a result as for u2ps_app_on_request(); a
 * device-to-host answer is written to the control buffer. */
int u2ps_dev_std_request(const u2ps_setup_t *s);

/* ---- Control pipe (u2ps_dev_ctrl.c) ---- */

/* Cancel the control transfer in progress, including a pending reply */
void u2ps_dev_ctrl_reset(void);

/* Act on a request's result: stall, wait, hold, send data or the status
 * stage */
void u2ps_dev_ctrl_answer(int result);

/* The result of the start SET_INTERFACE's status waits for: answered if
 * the request still holds its status; U2PS_REQ_HOLD changes nothing */
void u2ps_dev_ctrl_held_answer(int result);

/* ---- Audio function (u2ps_dev_audio.c) ---- */

/* A class request to an interface of the current configuration */
int u2ps_dev_class_request(const u2ps_request_t *req);

/* u2ps_dev_reply() answered the pending request with 'result' */
void u2ps_dev_audio_replied(int result);

/* SET_INTERFACE of the AudioStreaming interface, or alternate setting 0 at
 * SET_CONFIGURATION: starts or closes the stream. Returns 0, U2PS_REQ_STALL
 * or U2PS_REQ_HOLD (SET_INTERFACE's status waits for the open). */
int u2ps_dev_audio_set_alt(uint8_t alt);

/* Bus reset or detach: the port has stopped the stream itself */
void u2ps_dev_audio_reset(void);
/* The stream's end: the holes after its last packet, before on_stream_stop */
void u2ps_dev_audio_tail(void);

/* The rate the clock starts at: 'current' (the clock's rate before a
 * re-init, 0 at the first init) if the description lists it; otherwise
 * the description's initial rate, or its first rate */
uint32_t u2ps_dev_initial_rate(const u2ps_dev_description_t *d, uint32_t current);

/* ---- Device state (u2ps_dev.c) ---- */

/* The device leaves the Configured state, if it is in it, for reason
 * 'why': the app's on_configured(false, why). The caller sets the new
 * state. */
void u2ps_dev_unconfigure(u2ps_config_end_t why);

/* An internal result: SET_INTERFACE's status stage is held until the
 * stream opens (u2ps_dev_audio.c) */
#define U2PS_REQ_HOLD (-3)

#if U2PS_CONTROL_HOOK
#define U2PS_DEV_ON_CONTROL(setup, result) u2ps_app_on_control(setup, result)
#else
#define U2PS_DEV_ON_CONTROL(setup, result) ((void)0)
#endif

/* ---- Contract checks (U2PS_CONTRACT_CHECKS, u2ps_dev.c) ----
 * Without the checks every macro below is the plain code, so that build
 * is the one without them. */
#if U2PS_CONTRACT_CHECKS
/* The port's vectors masked through U2PS_DEV_MASK(): the flag covers the
 * port's masked time from before irq_mask(true) to after irq_mask(false) */
extern bool u2ps_dev_masked;
void u2ps_dev_mask(bool masked);
/* At the entry of a thread call: inside a callback, or nested in another */
void u2ps_dev_check_thread(void);
/* A violation: the app's hook */
#define U2PS_DEV_CONTRACT(violation) u2ps_app_on_contract(violation)
/* What a callback returned: on_request and on_sampling_frequency, and
 * on_stream_start; each returns its argument */
int u2ps_dev_check_request(int result);
u2ps_stream_start_t u2ps_dev_check_start(u2ps_stream_start_t decision);
/* A successful u2ps_dev_init() took the description: its checksum */
void u2ps_dev_check_init(const u2ps_dev_description_t *d);
/* u2ps_dev_tick(): the description unchanged; the tick's period while a
 * close or a held status is timed ('timed', read with the vectors masked) */
void u2ps_dev_check_description(void);
void u2ps_dev_check_tick(uint32_t now_ms, bool timed);
#define U2PS_DEV_MASK(masked)            u2ps_dev_mask(masked)
#define U2PS_DEV_CHECK_THREAD()          u2ps_dev_check_thread()
#define U2PS_DEV_REQUEST_RESULT(result)  u2ps_dev_check_request(result)
#define U2PS_DEV_START_RESULT(decision)  u2ps_dev_check_start(decision)
#else
#define U2PS_DEV_MASK(masked)            u2ps_dport_irq_mask(masked)
#define U2PS_DEV_CHECK_THREAD()          ((void)0)
#define U2PS_DEV_REQUEST_RESULT(result)  (result)
#define U2PS_DEV_START_RESULT(decision)  (decision)
#endif

#endif /* U2PS_DEV_PRIV_H */
