/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_host.c: host stack start-up and re-init, the session's permission,
 * VBUS, test mode, the enumeration as a state machine, the test device's
 * sequences, suspend and resume, and the requests of a stream's start and
 * stop: the waits run on u2ps_host_tick(), the requests on
 * u2ps_host_on_control(). The descriptor walk is in
 * u2ps_host_walk.c, the stream's packet path in u2ps_host_stream.c.
 *
 * Context: every call from thread masks the port's vectors for its whole
 * body, so its port calls and the app callbacks it makes run masked; the
 * upcalls need no mask. Both set u2ps_host_in_upcall, so that the app's
 * calls of any context inside them do not mask again.
 *
 * References: USB 2.0 Specification, Revision 2.0; UAC2 is the USB Device
 * Class Definition for Audio Devices, Release 2.0.
 */
#include <string.h>

#include "u2ps_ch9.h"
#include "u2ps_host_priv.h"
#include "u2ps_hport.h"
#include "u2ps_uac2.h"

/* In DMA memory through its section, and on whole 32-byte cache lines, so
 * the port can clean and invalidate it alone (u2ps_hport.h). A multiple of
 * 64: a control IN data stage reserves whole packets of endpoint 0's
 * size, up to 64 bytes (USB 2.0 5.5.3), so a buffer that ends inside one
 * would let a device that ignores wLength write past it. */
_Static_assert(U2PS_HOST_EP0_BUF >= 256u && U2PS_HOST_EP0_BUF % 64u == 0u &&
                 U2PS_HOST_EP0_BUF <= 0xFFC0u,
               "U2PS_HOST_EP0_BUF must be a multiple of 64, 256..65472: set -DU2PS_HOST_EP0_BUF "
               "(default 1024)");
_Static_assert(U2PS_HOST_ALT_MAX >= 1u && U2PS_HOST_ALT_MAX <= 255u,
               "U2PS_HOST_ALT_MAX must be 1..255: set -DU2PS_HOST_ALT_MAX (default 16)");
_Static_assert(U2PS_HOST_SUB_RANGE_MAX >= 1u &&
                 2u + 12u * U2PS_HOST_SUB_RANGE_MAX <= U2PS_HOST_EP0_BUF,
               "U2PS_HOST_SUB_RANGE_MAX must be at least 1, and its RANGE, 2 + 12 * "
               "U2PS_HOST_SUB_RANGE_MAX bytes, must fit U2PS_HOST_EP0_BUF: lower "
               "-DU2PS_HOST_SUB_RANGE_MAX (default 32) or raise -DU2PS_HOST_EP0_BUF");
_Static_assert(U2PS_HOST_QUEUE_MAX >= 1u && U2PS_HOST_QUEUE_MAX <= 255u,
               "U2PS_HOST_QUEUE_MAX must be 1..255 (the DWC2 host port's queue): set "
               "-DU2PS_HOST_QUEUE_MAX (default 16)");
/* 8 mA to 5 A: the range of IA_VBUS_RATED (OTG and EH Compliance Plan
 * Rev. 1.2, section 3, p. 11; 6.7.4 step 1, p. 32) */
_Static_assert(U2PS_HOST_IA_VBUS_RATED_MA >= 8u && U2PS_HOST_IA_VBUS_RATED_MA <= 5000u,
               "U2PS_HOST_IA_VBUS_RATED_MA must be 8..5000: set -DU2PS_HOST_IA_VBUS_RATED_MA to the "
               "mA the board's VBUS supply of the host port is rated for (no default)");
_Static_assert(U2PS_HOST_CONTROL_MAX >= 1u && U2PS_HOST_CONTROL_MAX <= 65535u,
               "U2PS_HOST_CONTROL_MAX must be 1..65535 ms: set -DU2PS_HOST_CONTROL_MAX "
               "(default 5000)");

_Alignas(32) static uint8_t setup_buf[32] __attribute__((section(U2PS_DMA_SECTION)));
_Alignas(32) static uint8_t ctrl_buf[U2PS_HOST_EP0_BUF]
  __attribute__((section(U2PS_DMA_SECTION)));

/* The waits of the enumeration, in ms */
#define T_ATTDB  100u /* debounce after a connect (7.1.7.3) */
#define T_DRSTR  50u  /* reset of a root port (7.1.7.5) */
#define T_RSTRCY 10u  /* reset recovery (9.2.6.2) */
#define T_SETADR 2u   /* after SET_ADDRESS (9.2.6.3) */
#define T_DRSMDN 20u  /* resume K driven by the host (7.1.7.7) */
#define T_RSMRCY 10u  /* resume recovery (7.1.7.7) */
/* The stack's bound on a port that does not end a resume, from the first
 * resume(false) */
#define T_RESUME_END 50u
#define T_TEST   15000u /* the test device's waits (EHSET Procedure 3.4) */

#define EP0_MPS_HS    64u  /* bMaxPacketSize0 at high speed (9.6.1) */
/* The packet size of the first request at full and low speed: it reads
 * the first 8 bytes of the device descriptor (5.5.3) */
#define EP0_MPS_FIRST 8u
#define HUB_CLASS     0x09u   /* bDeviceClass of a hub (11.23.1) */
#define TEST_VID      0x1A0Au /* idVendor of the test device */
#define ADDRESS       1u   /* the one device's address */
#define STRING_LEN    255u /* wLength of a string request: the largest bLength */
#define STRING_CHARS  126u /* UTF-16 code units of the longest string (9.6.7) */

typedef enum {
  ST_IDLE,         /* no session: a device, the permission or VBUS is missing */
  ST_DEBOUNCE,     /* T_ATTDB of connection */
  ST_RESET,        /* reset segments until T_DRSTR has passed */
  ST_RESET_END,    /* reset(false) called; on_enabled awaited */
  ST_RECOVERY,     /* T_RSTRCY after the reset */
  ST_ADDRESS_WAIT, /* T_SETADR after SET_ADDRESS */
  ST_CONTROL,      /* the request of 'step' in flight */
  ST_CONFIGURED,   /* on_device has come; no request in flight */
  ST_ENDED,        /* the session failed; the device is left alone */
  ST_TEST_MODE,    /* until a re-init */
  ST_TD_WAIT,      /* the test device's T_TEST before its next step */
  ST_TD,           /* the test device's sequence is done; its session stays */
  ST_SUSPENDED,    /* the port is suspended: by the app, or by test device 0x0106 */
  ST_RESUME_K,     /* the resume K: T_DRSMDN */
  ST_RESUME_END,   /* resume(false) at every tick until the port is out of resume */
  ST_RESUME_RCY    /* T_RSMRCY after the resume, then GET_STATUS */
} state_t;

/* The requests of the enumeration, in order */
typedef enum {
  STEP_DEVICE8,    /* GET_DESCRIPTOR(device, 64) at address 0 */
  STEP_ADDRESS,    /* SET_ADDRESS */
  STEP_DEVICE,     /* GET_DESCRIPTOR(device, 18) */
  STEP_CONFIG9,    /* GET_DESCRIPTOR(configuration or other_speed_configuration, 9) */
  STEP_CONFIG,     /* GET_DESCRIPTOR(the same, wTotalLength) */
  STEP_QUALIFIER,  /* GET_DESCRIPTOR(device_qualifier, 10) at full and low speed */
  STEP_LANGID,     /* GET_DESCRIPTOR(string 0) */
  STEP_STRING,     /* GET_DESCRIPTOR(string) */
  STEP_SET_CONFIG, /* SET_CONFIGURATION */
  STEP_RANGE,      /* RANGE of the Sampling Frequency Control */
  STEP_CUR,        /* CUR of the Sampling Frequency Control */
  STEP_VALID,      /* CUR of the Clock Validity Control */
  /* A stream's start and stop */
  STEP_S_SET_FREQ, /* SET CUR of the Sampling Frequency Control */
  STEP_S_GET_FREQ, /* GET CUR of it, read back */
  STEP_S_VALID,    /* CUR of the Clock Validity Control */
  STEP_S_ALT,      /* SET_INTERFACE to the stream's alternate setting */
  STEP_S_ALT0,     /* SET_INTERFACE to alternate setting 0 */
  STEP_GET_STATUS, /* GET_STATUS(device) after a resume */
  /* The test device's requests */
  STEP_TD_DESC,    /* 0x0107: GET_DESCRIPTOR(device, 18) */
  STEP_TD_SETUP,   /* 0x0108: its SETUP stage */
  STEP_TD_DATA     /* 0x0108: its data and status stages */
} step_t;

/* Where a stream stands */
typedef enum {
  SS_IDLE,     /* no stream */
  SS_STARTING, /* the requests of its start */
  SS_OPEN,     /* the port's stream is open */
  SS_CLOSING,  /* stream_close called; on_closed awaited */
  SS_ALT0      /* SET_INTERFACE 0 in flight, then on_stream_stop */
} stream_state_t;

static struct {
  bool port_up;   /* u2ps_hport_init() has succeeded */
  bool allowed;   /* u2ps_host_connect(true) */
  bool connected; /* the port reported a device */
  bool session;   /* a session runs: its on_end is owed */
  state_t state;
  uint32_t now;   /* the last tick's time */
  bool wait_known;  /* the wait's start is known */
  uint32_t wait_start;
  /* The control transfer in flight */
  step_t step;
  bool ctl_known; /* its start is known */
  uint32_t ctl_start;
  bool aborted;   /* aborted at its timeout */
  /* What the enumeration has read */
  u2ps_speed_t speed; /* the port's, at the reset's end */
  uint8_t mps0;       /* the packet size of endpoint 0: bMaxPacketSize0 once read */
  uint16_t td_pid;    /* the test device's PID; 0 for any other device */
  bool power_refused; /* a configuration was playable apart from its power */
  /* The configurations read are the other-speed ones (USB 2.0 9.6.4) */
  bool other_speed;
  uint8_t config_index, config_count, config_value;
  uint8_t string_ids[3]; /* iManufacturer, iProduct, iSerialNumber */
  uint8_t string_at;     /* the string read now, 0..2 */
  uint16_t langid;
  char strings[3][STRING_CHARS + 1u];
  bool has_string[3];
  u2ps_host_refusal_t refusal; /* the first configuration's, for on_end(NOT_PLAYABLE) */
  u2ps_host_walk_t walk;
  u2ps_host_sub_range_t sub_ranges[U2PS_HOST_SUB_RANGE_MAX];
  u2ps_host_device_t dev;
  uint32_t control_timeouts;
  /* Suspend and resume */
  bool remote;           /* the device's wakeup began the resume in progress, or the last */
  uint32_t resume_first; /* the first resume(false)'s tick */
  uint32_t wakeups;
  /* The stream */
  stream_state_t ss;
  uint32_t stream;            /* its number; the last one's after it */
  uint8_t alt;                /* index of its alternate setting in walk.alts */
  u2ps_host_stop_t stop_why;  /* the reason on_stream_stop gives after SET_INTERFACE 0 */
  bool cancel;                /* the app stopped the start: it ends at its request's completion */
} u2ps_host;

/* Outside u2ps_host, which u2ps_host_init() resets: a report before the
 * first init counts, and a re-init keeps the level */
static bool vbus_on;

static void mask(void)
{
  if (u2ps_host.port_up) {
    U2PS_HOST_MASK(true);
    u2ps_host_in_upcall = true;
  }
}

static void unmask(void)
{
  if (u2ps_host.port_up) {
    u2ps_host_in_upcall = false;
    U2PS_HOST_MASK(false);
  }
}

/* The stream has stopped, or its start ended, for reason 'why' (and with
 * U2PS_HOST_STOP_REFUSED the request refused): its one on_stream_stop */
static void stream_stopped(u2ps_host_stop_t why, u2ps_host_stop_request_t request)
{
  u2ps_host.ss = SS_IDLE;
  u2ps_host_stream_end();
  U2PS_HOST_CALLBACK(u2ps_app_host_on_stream_stop(why, request));
}

/* The session ends for reason 'why', with its one on_end, after the
 * stream's on_stream_stop; the port is disabled unless it stopped itself
 * (a disconnect, a disabled port). An open stream's packets return to the
 * app at on_stream_stop, so the port stops first; a re-init and test mode
 * reset or disable it after anyway. */
static void session_end(u2ps_host_end_t why, bool disable, state_t next)
{
  bool had = u2ps_host.session;
  u2ps_host.session = false;
  u2ps_host.state = next;
  if (u2ps_host.ss != SS_IDLE) {
    bool port_runs = disable || why == U2PS_HOST_END_REINIT || why == U2PS_HOST_END_TEST_MODE;
    if (port_runs && (u2ps_host.ss == SS_OPEN || u2ps_host.ss == SS_CLOSING)) {
      u2ps_hport_disable();
      disable = false;
    }
    stream_stopped(U2PS_HOST_STOP_END, U2PS_HOST_STOP_REQUEST_NONE);
  }
  if (had)
    U2PS_HOST_CALLBACK(u2ps_app_host_on_end(
      why, why == U2PS_HOST_END_NOT_PLAYABLE ? u2ps_host.refusal : U2PS_HOST_REFUSAL_NONE));
  if (disable && had)
    u2ps_hport_disable();
}

static void fail(u2ps_host_end_t why)
{
  session_end(why, true, ST_ENDED);
}

/* No reason to end: the enumeration goes on */
#define GO_ON ((u2ps_host_end_t)0)

/* Why a required request that did not complete ends the session */
static u2ps_host_end_t control_end(u2ps_hport_control_t result)
{
  return result == U2PS_HPORT_CONTROL_STALL ? U2PS_HOST_END_STALL : U2PS_HOST_END_ERROR;
}

/* A session may start: a device, the permission and VBUS, and no session
 * yet. The debounce begins at the next tick. */
static void maybe_start(void)
{
  if (u2ps_host.state == ST_IDLE && u2ps_host.allowed && u2ps_host.connected && vbus_on) {
    u2ps_host.state = ST_DEBOUNCE;
    u2ps_host.wait_known = false;
  }
}

/* Whether 'ms' have passed since the wait's start: the first tick after it
 * began, or the tick that began it. More than ms counts of the app's
 * millisecond counter: the counter may have been read just before its
 * next count at the start and just after one at the end, so ms counts can
 * be as little as ms - 1 ms; ms + 1 counts are always more than ms. */
static bool waited(uint32_t ms)
{
  if (!u2ps_host.wait_known) {
    u2ps_host.wait_known = true;
    u2ps_host.wait_start = u2ps_host.now;
    return false;
  }
  return u2ps_host.now - u2ps_host.wait_start > ms;
}

static void wait_from_now(state_t next)
{
  u2ps_host.state = next;
  u2ps_host.wait_known = false;
}

/* In the tick: a wait that starts at this tick */
static void wait_from_tick(state_t next)
{
  u2ps_host.state = next;
  u2ps_host.wait_known = true;
  u2ps_host.wait_start = u2ps_host.now;
}

/* The SETUP packet of a request */
static void setup_packet(uint8_t bm, uint8_t request, uint16_t value, uint16_t index,
                         uint16_t length)
{
  setup_buf[0] = bm;
  setup_buf[1] = request;
  setup_buf[2] = (uint8_t)value;
  setup_buf[3] = (uint8_t)(value >> 8);
  setup_buf[4] = (uint8_t)index;
  setup_buf[5] = (uint8_t)(index >> 8);
  setup_buf[6] = (uint8_t)length;
  setup_buf[7] = (uint8_t)(length >> 8);
}

/* A request of 'step' is in flight; its time starts at the next tick */
static void in_flight(step_t step)
{
  u2ps_host.state = ST_CONTROL;
  u2ps_host.step = step;
  u2ps_host.ctl_known = false;
  u2ps_host.aborted = false;
}

/* Starts a control transfer */
static void control(step_t step, uint8_t addr, uint8_t bm, uint8_t request, uint16_t value,
                    uint16_t index, uint16_t length)
{
  setup_packet(bm, request, value, index, length);
  in_flight(step);
  u2ps_hport_control(addr, u2ps_host.mps0, setup_buf, ctrl_buf);
}

static void get_descriptor(step_t step, uint8_t addr, uint8_t type, uint8_t index, uint16_t lang,
                           uint16_t length)
{
  control(step, addr, U2PS_REQ_DIR_IN, U2PS_GET_DESCRIPTOR, (uint16_t)(type << 8 | index), lang,
          length);
}

/* A request to the clock: Control Selector 'cs', channel 0, the Clock
 * Source on the function's AudioControl interface (UAC2 5.2.2) */
static void clock_request(step_t step, uint8_t request, uint8_t cs, uint16_t length)
{
  control(step, ADDRESS, U2PS_REQ_DIR_IN | U2PS_REQ_TYPE_CLASS | U2PS_REQ_RCPT_ITF, request,
          (uint16_t)(cs << 8), (uint16_t)(u2ps_host.walk.clock << 8 | u2ps_host.walk.itf_ac),
          length);
}

/* SET CUR of the clock's Sampling Frequency Control, 4 bytes (UAC2
 * 5.2.5.1.1) */
static void set_frequency(uint32_t hz)
{
  ctrl_buf[0] = (uint8_t)hz;
  ctrl_buf[1] = (uint8_t)(hz >> 8);
  ctrl_buf[2] = (uint8_t)(hz >> 16);
  ctrl_buf[3] = (uint8_t)(hz >> 24);
  control(STEP_S_SET_FREQ, ADDRESS, U2PS_REQ_TYPE_CLASS | U2PS_REQ_RCPT_ITF, U2PS_REQ_CUR,
          (uint16_t)(U2PS_CS_SAM_FREQ_CONTROL << 8),
          (uint16_t)(u2ps_host.walk.clock << 8 | u2ps_host.walk.itf_ac), 4);
}

/* SET_INTERFACE of the AudioStreaming interface (USB 2.0 9.4.10) */
static void set_interface(step_t step, uint8_t alternate_setting)
{
  control(step, ADDRESS, U2PS_REQ_RCPT_ITF, U2PS_SET_INTERFACE, alternate_setting,
          u2ps_host.walk.itf_as, 0);
}

/* The descriptor type of the configurations read: other-speed ones have
 * a configuration's layout (USB 2.0 9.6.4) */
static uint8_t config_type(void)
{
  return u2ps_host.other_speed ? U2PS_DESC_OTHER_SPEED : U2PS_DESC_CONFIGURATION;
}

static void read_config_head(void)
{
  get_descriptor(STEP_CONFIG9, ADDRESS, config_type(), u2ps_host.config_index, 0,
                 U2PS_CONFIG_LEN);
}

/* The next string to read from 'from' on, or SET_CONFIGURATION */
static void next_string(uint8_t from)
{
  for (uint8_t k = from; k < 3u; k++)
    if (u2ps_host.string_ids[k] != 0u) {
      u2ps_host.string_at = k;
      get_descriptor(STEP_STRING, ADDRESS, U2PS_DESC_STRING, u2ps_host.string_ids[k],
                     u2ps_host.langid, STRING_LEN);
      return;
    }
  control(STEP_SET_CONFIG, ADDRESS, 0, U2PS_SET_CONFIGURATION, u2ps_host.config_value, 0, 0);
}

/* The strings, if the device has any and the library reads them */
static void read_strings(void)
{
  bool any = u2ps_host.string_ids[0] != 0u || u2ps_host.string_ids[1] != 0u ||
             u2ps_host.string_ids[2] != 0u;
  if (U2PS_HOST_STRINGS && any) {
    get_descriptor(STEP_LANGID, ADDRESS, U2PS_DESC_STRING, 0, 0, STRING_LEN);
    return;
  }
  next_string(3);
}

/* A string descriptor of len bytes as ASCII: each UTF-16 code unit outside
 * 0x20..0x7E becomes '?'. False if it is no string descriptor. */
static bool keep_string(const uint8_t *s, uint16_t len, char *out)
{
  if (len < 2u || s[0] < 2u || s[1] != U2PS_DESC_STRING)
    return false;
  unsigned n = s[0] < len ? s[0] : len;
  unsigned chars = (n - 2u) / 2u;
  if (chars > STRING_CHARS)
    chars = STRING_CHARS;
  for (unsigned i = 0; i < chars; i++) {
    uint16_t u = u2ps_le16(s + 2u + 2u * i);
    out[i] = u >= 0x20u && u <= 0x7Eu ? (char)u : '?';
  }
  out[chars] = '\0';
  return true;
}

/* The enumeration is done: the device as the app sees it */
static void configured(void)
{
  u2ps_host_device_t *d = &u2ps_host.dev;
  const u2ps_host_walk_t *w = &u2ps_host.walk;
  d->manufacturer = u2ps_host.has_string[0] ? u2ps_host.strings[0] : NULL;
  d->product = u2ps_host.has_string[1] ? u2ps_host.strings[1] : NULL;
  d->serial_number = u2ps_host.has_string[2] ? u2ps_host.strings[2] : NULL;
  d->terminal_type = w->terminal_type;
  d->sampling_frequency_host_programmable = w->clock_programmable;
  d->sub_ranges = u2ps_host.sub_ranges;
  d->alternate_setting_count = w->alt_count;
  d->alternate_settings = w->alts;
  u2ps_host.state = ST_CONFIGURED;
  u2ps_app_host_on_device(d);
}

/* ---- A stream's start and stop -------------------------------------------------- */

/* The device refused request 'request' of the start (a STALL, or a
 * frequency it cannot report): no stream */
static u2ps_host_end_t start_refused(u2ps_host_stop_request_t request)
{
  u2ps_host.state = ST_CONFIGURED;
  stream_stopped(U2PS_HOST_STOP_REFUSED, request);
  return GO_ON;
}

/* SET_INTERFACE 0, then on_stream_stop(why) */
static void unselect(u2ps_host_stop_t why)
{
  u2ps_host.ss = SS_ALT0;
  u2ps_host.stop_why = why;
  set_interface(STEP_S_ALT0, 0);
}

/* The app stopped the start (u2ps_host_stream_stop), and its request in
 * flight has completed: no further request of the start; SET_INTERFACE 0
 * first if the device accepted SET_INTERFACE ('selected') */
static u2ps_host_end_t start_cancelled(bool selected)
{
  if (selected) {
    unselect(U2PS_HOST_STOP_APP);
    return GO_ON;
  }
  u2ps_host.state = ST_CONFIGURED;
  stream_stopped(U2PS_HOST_STOP_APP, U2PS_HOST_STOP_REQUEST_NONE);
  return GO_ON;
}

/* The start's last request: SET_INTERFACE to the stream's alternate
 * setting */
static void select_alternate_setting(void)
{
  set_interface(STEP_S_ALT, u2ps_host.walk.alts[u2ps_host.alt].alternate_setting);
}

/* The alternate setting is selected: the port opens the stream, with the
 * feedback endpoint only for explicit feedback, and the app learns what it
 * plays */
static void stream_open(void)
{
  const u2ps_host_alternate_setting_t *a = &u2ps_host.walk.alts[u2ps_host.alt];
  const u2ps_host_alt_ep_t *ep = &u2ps_host.walk.eps[u2ps_host.alt];
  u2ps_hstream_t hs = {
    .addr = ADDRESS, .ep_out = (uint8_t)(ep->ep_out & 0x0Fu), .mps = a->max_packet_size};
  if (a->explicit_feedback) {
    hs.ep_fb = (uint8_t)(ep->ep_fb & 0x0Fu);
    hs.fb_interval = ep->fb_interval;
    hs.fb_mps = ep->fb_mps;
  }
  if (!u2ps_hport_stream_open(&hs)) {
    unselect(U2PS_HOST_STOP_OPEN);
    return;
  }
  uint32_t fs = u2ps_host.dev.sampling_frequency;
  u2ps_host.state = ST_CONFIGURED;
  u2ps_host.ss = SS_OPEN;
  u2ps_host_stream_begin(u2ps_host.stream, a, fs);
  u2ps_host_stream_info_t info = {
    .stream = u2ps_host.stream,
    .alternate_setting = a->alternate_setting,
    .format = a->format,
    .nr_channels = a->nr_channels,
    .sampling_frequency = fs,
    .max_packet_size = a->max_packet_size,
    .synchronization_type = a->synchronization_type,
    .explicit_feedback = a->explicit_feedback,
    .nominal_rate = (uint32_t)((((uint64_t)fs << 16) + 4000u) / 8000u),
    .clock_validity = u2ps_host.dev.clock_validity};
  u2ps_app_host_on_stream_start(&info);
}

/* ---- The test device, power, endpoint 0 --------------------------------------------- */

/* The PIDs of the test device that the stack runs (u2ps_host.h) */
static bool test_pid(uint16_t pid)
{
  return (pid >= 0x0101u && pid <= 0x0104u) || (pid >= 0x0106u && pid <= 0x0108u) ||
         pid == 0x0200u;
}

/* The configuration descriptor's bMaxPower, in 2 mA units (Table 9-10),
 * within the port's budget */
static bool within_budget(const uint8_t *config)
{
  return (uint32_t)config[8] * 2u <= U2PS_HOST_IA_VBUS_RATED_MA;
}

/* bMaxPacketSize0 that the port's speed allows (5.5.3, 9.6.1) */
static bool ep0_allowed(uint8_t m)
{
  switch (u2ps_host.speed) {
  case U2PS_SPEED_HIGH:
    return m == EP0_MPS_HS;
  case U2PS_SPEED_FULL:
    return m == 8u || m == 16u || m == 32u || m == 64u;
  default:
    return m == 8u;
  }
}

/* The test device is configured: the app learns its PID, and its test
 * follows. PIDs 0x0101-0x0104 select the test modes SE0_NAK, J, K and
 * Packet (USB 2.0 Table 9-7); 0x0108 sends the SETUP of GET_DESCRIPTOR at
 * once; 0x0106 and 0x0107 wait T_TEST first; 0x0200 stays. */
static void test_device(void)
{
  static const uint8_t selectors[4] = {U2PS_TEST_SE0_NAK, U2PS_TEST_J, U2PS_TEST_K,
                                       U2PS_TEST_PACKET};
  uint16_t pid = u2ps_host.td_pid;
  u2ps_app_host_on_test_device(pid, u2ps_host.dev.bcd_device);
  if (pid <= 0x0104u) {
    session_end(U2PS_HOST_END_TEST_MODE, false, ST_TEST_MODE);
    u2ps_hport_disable();
    u2ps_hport_test_mode(selectors[pid - 0x0101u]);
  } else if (pid == 0x0108u) {
    setup_packet(U2PS_REQ_DIR_IN, U2PS_GET_DESCRIPTOR, (uint16_t)(U2PS_DESC_DEVICE << 8), 0,
                 U2PS_DEVICE_LEN);
    in_flight(STEP_TD_SETUP);
    u2ps_hport_control_setup(ADDRESS, u2ps_host.mps0, setup_buf);
  } else if (pid == 0x0200u) {
    u2ps_host.state = ST_TD;
  } else {
    wait_from_now(ST_TD_WAIT);
  }
}

/* The completion of each request of the enumeration: GO_ON, or the reason
 * the session ends */
static u2ps_host_end_t step_done(u2ps_hport_control_t result, uint16_t len)
{
  const uint8_t *b = ctrl_buf;
  bool ok = result == U2PS_HPORT_CONTROL_OK;
  bool stall = result == U2PS_HPORT_CONTROL_STALL;

  switch (u2ps_host.step) {
  case STEP_DEVICE8:
    if (!ok)
      return control_end(result);
    if (len < 8u || b[1] != U2PS_DESC_DEVICE)
      return U2PS_HOST_END_DESCRIPTOR;
    if (!ep0_allowed(b[7]))
      return U2PS_HOST_END_EP0_MPS;
    u2ps_host.mps0 = b[7];
    control(STEP_ADDRESS, 0, 0, U2PS_SET_ADDRESS, ADDRESS, 0, 0);
    return GO_ON;
  case STEP_ADDRESS:
    if (!ok)
      return control_end(result);
    wait_from_now(ST_ADDRESS_WAIT);
    return GO_ON;
  case STEP_DEVICE:
    if (!ok)
      return control_end(result);
    if (len < U2PS_DEVICE_LEN || b[0] < U2PS_DEVICE_LEN || b[1] != U2PS_DESC_DEVICE || b[17] == 0u)
      return U2PS_HOST_END_DESCRIPTOR;
    if (b[4] == HUB_CLASS)
      return U2PS_HOST_END_HUB;
    u2ps_host.dev.id_vendor = u2ps_le16(b + 8);
    u2ps_host.dev.id_product = u2ps_le16(b + 10);
    u2ps_host.dev.bcd_device = u2ps_le16(b + 12);
    if (u2ps_host.dev.id_vendor == TEST_VID && test_pid(u2ps_host.dev.id_product))
      u2ps_host.td_pid = u2ps_host.dev.id_product;
    memcpy(u2ps_host.string_ids, b + 14, 3);
    u2ps_host.config_count = b[17];
    u2ps_host.config_index = 0;
    read_config_head();
    return GO_ON;
  case STEP_CONFIG9: {
    if (!ok)
      return control_end(result);
    uint16_t total = len >= 4u ? u2ps_le16(b + 2) : 0u;
    if (len < U2PS_CONFIG_LEN || b[1] != config_type() || total < U2PS_CONFIG_LEN)
      return U2PS_HOST_END_DESCRIPTOR;
    if (total > U2PS_HOST_EP0_BUF)
      return U2PS_HOST_END_TOO_LARGE;
    get_descriptor(STEP_CONFIG, ADDRESS, config_type(), u2ps_host.config_index, 0, total);
    return GO_ON;
  }
  case STEP_CONFIG: {
    if (!ok)
      return control_end(result);
    /* The test device: configuration 0, whatever it holds, within the
     * budget */
    if (u2ps_host.td_pid != 0u) {
      if (len < U2PS_CONFIG_LEN || b[1] != U2PS_DESC_CONFIGURATION)
        return U2PS_HOST_END_DESCRIPTOR;
      if (!within_budget(b))
        return U2PS_HOST_END_POWER;
      u2ps_host.config_value = b[5];
      control(STEP_SET_CONFIG, ADDRESS, 0, U2PS_SET_CONFIGURATION, b[5], 0, 0);
      return GO_ON;
    }
    /* Of the type requested: a configuration, or an other-speed one */
    u2ps_host_refusal_t r = len >= 2u && b[1] != config_type()
                              ? U2PS_HOST_REFUSAL_NOT_CONFIG
                              : u2ps_host_walk(b, len, &u2ps_host.walk);
    if (r == U2PS_HOST_REFUSAL_NONE) {
      /* At full and low speed the device is identified: it plays at high
       * speed only */
      if (u2ps_host.speed != U2PS_SPEED_HIGH)
        return U2PS_HOST_END_SPEED;
      if (within_budget(b)) {
        u2ps_host.config_value = b[5];
        read_strings();
        return GO_ON;
      }
      u2ps_host.power_refused = true;
    }
    if (u2ps_host.config_index == 0u && !u2ps_host.other_speed)
      u2ps_host.refusal = r;
    if (++u2ps_host.config_index < u2ps_host.config_count) {
      read_config_head();
      return GO_ON;
    }
    /* None plays at full or low speed: a high-speed-capable device tells
     * its other-speed configurations in its device_qualifier (9.6.2) */
    if (u2ps_host.speed != U2PS_SPEED_HIGH && !u2ps_host.other_speed) {
      get_descriptor(STEP_QUALIFIER, ADDRESS, U2PS_DESC_DEVICE_QUALIFIER, 0, 0,
                     U2PS_QUALIFIER_LEN);
      return GO_ON;
    }
    return u2ps_host.power_refused ? U2PS_HOST_END_POWER : U2PS_HOST_END_NOT_PLAYABLE;
  }
  case STEP_QUALIFIER:
    /* A request error: a full-speed-only device (9.6.2). A malformed
     * qualifier, or one without other-speed configurations, is as none. */
    if (!ok && !stall)
      return control_end(result);
    if (!ok || len < U2PS_QUALIFIER_LEN || b[0] < U2PS_QUALIFIER_LEN ||
        b[1] != U2PS_DESC_DEVICE_QUALIFIER || b[8] == 0u)
      return U2PS_HOST_END_NOT_PLAYABLE;
    u2ps_host.other_speed = true;
    u2ps_host.config_count = b[8];
    u2ps_host.config_index = 0;
    read_config_head();
    return GO_ON;
  case STEP_LANGID:
    /* No LANGID, or a STALL: no strings, which are optional */
    if (!ok && !stall)
      return control_end(result);
    if (!ok || len < 4u || b[0] < 4u || b[1] != U2PS_DESC_STRING) {
      next_string(3);
      return GO_ON;
    }
    u2ps_host.langid = u2ps_le16(b + 2);
    next_string(0);
    return GO_ON;
  case STEP_STRING: {
    uint8_t k = u2ps_host.string_at;
    if (!ok && !stall)
      return control_end(result);
    u2ps_host.has_string[k] = ok && keep_string(b, len, u2ps_host.strings[k]);
    next_string((uint8_t)(k + 1u));
    return GO_ON;
  }
  case STEP_SET_CONFIG:
    if (!ok)
      return control_end(result);
    if (u2ps_host.td_pid != 0u) {
      test_device();
      return GO_ON;
    }
    clock_request(STEP_RANGE, U2PS_REQ_RANGE, U2PS_CS_SAM_FREQ_CONTROL,
                  (uint16_t)(2u + 12u * U2PS_HOST_SUB_RANGE_MAX));
    return GO_ON;
  case STEP_RANGE:
    /* The Sampling Frequency Control must support CUR and RANGE (UAC2
     * 5.2.5.1.1) */
    if (stall)
      return U2PS_HOST_END_CLOCK;
    if (!ok)
      return control_end(result);
    u2ps_host.dev.num_sub_ranges =
      u2ps_host_range(b, len, u2ps_host.sub_ranges, U2PS_HOST_SUB_RANGE_MAX);
    if (u2ps_host.dev.num_sub_ranges == 0u)
      return U2PS_HOST_END_CLOCK;
    clock_request(STEP_CUR, U2PS_REQ_CUR, U2PS_CS_SAM_FREQ_CONTROL, 4);
    return GO_ON;
  case STEP_CUR:
    if (stall)
      return U2PS_HOST_END_CLOCK;
    if (!ok)
      return control_end(result);
    if (len != 4u)
      return U2PS_HOST_END_CLOCK;
    u2ps_host.dev.sampling_frequency = u2ps_le32(b);
    if (u2ps_host.walk.clock_valid_readable) {
      clock_request(STEP_VALID, U2PS_REQ_CUR, U2PS_CS_CLOCK_VALID_CONTROL, 1);
      return GO_ON;
    }
    configured();
    return GO_ON;
  case STEP_VALID:
  case STEP_S_VALID:
    /* Optional: a STALL leaves the validity unknown */
    if (!ok && !stall)
      return control_end(result);
    u2ps_host.dev.clock_validity = !ok || len < 1u ? U2PS_CLOCK_VALIDITY_UNKNOWN
                                   : b[0] != 0u   ? U2PS_CLOCK_VALIDITY_VALID
                                                  : U2PS_CLOCK_VALIDITY_INVALID;
    if (u2ps_host.step == STEP_VALID)
      configured();
    else if (u2ps_host.cancel)
      return start_cancelled(false);
    else
      select_alternate_setting();
    return GO_ON;
  /* A stream's start: the frequency, read back, the clock's validity, the
   * alternate setting; its stop: alternate setting 0 */
  case STEP_S_SET_FREQ:
    if (u2ps_host.cancel && (ok || stall))
      return start_cancelled(false);
    if (stall)
      return start_refused(U2PS_HOST_STOP_REQUEST_SET_SAMPLING_FREQUENCY);
    if (!ok)
      return control_end(result);
    clock_request(STEP_S_GET_FREQ, U2PS_REQ_CUR, U2PS_CS_SAM_FREQ_CONTROL, 4);
    return GO_ON;
  case STEP_S_GET_FREQ:
    if (u2ps_host.cancel && (stall || (ok && (len != 4u || u2ps_le32(b) == 0u))))
      return start_cancelled(false);
    if (stall)
      return start_refused(U2PS_HOST_STOP_REQUEST_GET_SAMPLING_FREQUENCY);
    if (ok && (len != 4u || u2ps_le32(b) == 0u))
      return start_refused(U2PS_HOST_STOP_REQUEST_SAMPLING_FREQUENCY_VALUE);
    if (!ok)
      return control_end(result);
    u2ps_host.dev.sampling_frequency = u2ps_le32(b);
    if (u2ps_host.cancel)
      return start_cancelled(false);
    if (u2ps_host.walk.clock_valid_readable)
      clock_request(STEP_S_VALID, U2PS_REQ_CUR, U2PS_CS_CLOCK_VALID_CONTROL, 1);
    else
      select_alternate_setting();
    return GO_ON;
  case STEP_S_ALT:
    if (u2ps_host.cancel && (ok || stall))
      return start_cancelled(ok);
    if (stall)
      return start_refused(U2PS_HOST_STOP_REQUEST_SET_INTERFACE);
    if (!ok)
      return control_end(result);
    stream_open();
    return GO_ON;
  case STEP_S_ALT0:
    if (!ok && !stall)
      return control_end(result);
    u2ps_host.state = ST_CONFIGURED;
    stream_stopped(u2ps_host.stop_why, U2PS_HOST_STOP_REQUEST_NONE);
    return GO_ON;
  /* The device has answered after a resume */
  case STEP_GET_STATUS:
    if (!ok)
      return control_end(result);
    u2ps_host.state = ST_CONFIGURED;
    u2ps_app_host_on_resumed(u2ps_host.remote);
    return GO_ON;
  /* The test device's requests: the session stays after them */
  case STEP_TD_SETUP:
    if (!ok)
      return control_end(result);
    wait_from_now(ST_TD_WAIT);
    return GO_ON;
  case STEP_TD_DESC:
  case STEP_TD_DATA:
    if (!ok)
      return control_end(result);
    u2ps_host.state = ST_TD;
    return GO_ON;
  }
  return GO_ON;
}

/* ---- Calls --------------------------------------------------------------------- */

u2ps_host_err_t u2ps_host_init(void)
{
  U2PS_HOST_CHECK_THREAD();
  /* DMA memory the port's DMA does not reach: refused before anything
   * changes, at every init (an app may have switched a RAM's clock off) */
  if (!u2ps_hport_dma_check(setup_buf, sizeof setup_buf) ||
      !u2ps_hport_dma_check(ctrl_buf, sizeof ctrl_buf))
    return U2PS_HOST_ERR_DMA_MEMORY;
  bool again = u2ps_host.port_up;
  /* A re-init: the session ends, in thread with the vectors masked, and
   * the controller resets with them masked */
  if (again) {
    mask();
    session_end(U2PS_HOST_END_REINIT, false, ST_IDLE);
  }
  /* Stream numbers go on counting, so an old one never names a new stream */
  uint32_t stream = u2ps_host.stream;
  memset(&u2ps_host, 0, sizeof u2ps_host);
  u2ps_host.stream = stream;
  u2ps_host_stream_reset();
  u2ps_host_err_t e = u2ps_hport_init();
#if U2PS_CONTRACT_CHECKS
  /* Where the port's DMA reaches a packet, for the check of each queue */
  if (e == U2PS_HOST_OK)
    u2ps_host_stream_ranges();
#endif
  /* After the port's init, whose controller reset ends a control transfer
   * still in flight: the memory is the stack's alone, and its section needs
   * no zeroing at startup */
  memset(setup_buf, 0, sizeof setup_buf);
  memset(ctrl_buf, 0, sizeof ctrl_buf);
  if (again) {
    u2ps_host_in_upcall = false;
    U2PS_HOST_MASK(false);
  }
  u2ps_host.port_up = e == U2PS_HOST_OK;
  return e;
}

void u2ps_host_connect(bool on)
{
  U2PS_HOST_CHECK_THREAD();
  if (!u2ps_host.port_up || u2ps_host.state == ST_TEST_MODE) {
#if U2PS_CONTRACT_CHECKS
    if (on && !u2ps_host.port_up)
      U2PS_HOST_CONTRACT(U2PS_CONTRACT_HOST_CONNECT_BEFORE_INIT);
#endif
    return;
  }
  mask();
  u2ps_host.allowed = on;
  if (on) {
    maybe_start();
  } else {
    session_end(U2PS_HOST_END_DETACH, true, ST_IDLE);
  }
  unmask();
}

void u2ps_host_vbus(bool present)
{
  U2PS_HOST_CHECK_THREAD();
  if (present == vbus_on)
    return;
  vbus_on = present;
  if (!u2ps_host.port_up || u2ps_host.state == ST_TEST_MODE)
    return;
  mask();
  if (present) {
    maybe_start();
  } else {
    session_end(U2PS_HOST_END_VBUS, true, ST_IDLE);
  }
  unmask();
}

#if U2PS_CONTRACT_CHECKS
/* The stack's one tick period, in ms (u2ps_host.h) */
#define TICK_PERIOD 10u

/* The tick's period while a wait or a control transfer is timed: one that
 * this tick measures from an earlier one */
static void check_tick(uint32_t now_ms)
{
  static bool known;
  static uint32_t last;
  state_t s = u2ps_host.state;
  bool timed = u2ps_host.port_up &&
               (((s == ST_DEBOUNCE || s == ST_RECOVERY || s == ST_ADDRESS_WAIT ||
                  s == ST_TD_WAIT || s == ST_RESUME_K || s == ST_RESUME_RCY ||
                  (s == ST_SUSPENDED && u2ps_host.td_pid != 0u)) &&
                 u2ps_host.wait_known) ||
                s == ST_RESET || s == ST_RESUME_END ||
                (s == ST_CONTROL && u2ps_host.ctl_known && !u2ps_host.aborted));
  if (known && timed && now_ms - last > TICK_PERIOD)
    U2PS_HOST_CONTRACT(U2PS_CONTRACT_HOST_TICK_LATE);
  known = true;
  last = now_ms;
}
#endif

/* The port is asked to end the resume: out of it, the device recovers
 * (or, for the test device, the sequence is done); still in it
 * T_RESUME_END after the first ask, the session ends */
static void resume_poll(void)
{
  if (u2ps_hport_resume(false)) {
    if (u2ps_host.td_pid != 0u)
      u2ps_host.state = ST_TD;
    else
      wait_from_tick(ST_RESUME_RCY);
  } else if (u2ps_host.now - u2ps_host.resume_first >= T_RESUME_END) {
    fail(U2PS_HOST_END_ERROR);
  }
}

void u2ps_host_tick(uint32_t now_ms)
{
  U2PS_HOST_CHECK_THREAD();
#if U2PS_CONTRACT_CHECKS
  check_tick(now_ms);
#endif
  /* Only a wait or a control transfer needs the tick; the masked switch
   * reads the state again */
  state_t s = u2ps_host.state;
  bool timed = s == ST_DEBOUNCE || s == ST_RESET || s == ST_RECOVERY || s == ST_ADDRESS_WAIT ||
               s == ST_CONTROL || s == ST_TD_WAIT || s == ST_RESUME_K || s == ST_RESUME_END ||
               s == ST_RESUME_RCY || (s == ST_SUSPENDED && u2ps_host.td_pid != 0u);
  if (!u2ps_host.port_up || !timed)
    return;
  mask();
  u2ps_host.now = now_ms;
  switch (u2ps_host.state) {
  case ST_DEBOUNCE:
    if (waited(T_ATTDB)) {
      /* The session begins with the reset, and keeps nothing of the last
       * session's device */
      u2ps_host.session = true;
      memset(&u2ps_host.dev, 0, sizeof u2ps_host.dev);
      memset(u2ps_host.has_string, 0, sizeof u2ps_host.has_string);
      u2ps_host.refusal = U2PS_HOST_REFUSAL_NONE;
      u2ps_host.td_pid = 0;
      u2ps_host.power_refused = false;
      u2ps_host.other_speed = false;
      u2ps_host.remote = false;
      u2ps_host.state = ST_RESET;
      u2ps_host.wait_known = true;
      u2ps_host.wait_start = now_ms;
      u2ps_hport_reset(true);
    }
    break;
  case ST_RESET:
    /* More than T_DRSTR counts, as in waited() */
    if (now_ms - u2ps_host.wait_start > T_DRSTR) {
      u2ps_host.state = ST_RESET_END;
      u2ps_hport_reset(false);
    }
    break;
  case ST_RECOVERY:
    /* 64 bytes at high speed; the first 8 at full and low speed */
    if (waited(T_RSTRCY))
      get_descriptor(STEP_DEVICE8, 0, U2PS_DESC_DEVICE, 0, 0, u2ps_host.mps0);
    break;
  case ST_ADDRESS_WAIT:
    if (waited(T_SETADR))
      get_descriptor(STEP_DEVICE, ADDRESS, U2PS_DESC_DEVICE, 0, 0, U2PS_DEVICE_LEN);
    break;
  case ST_CONTROL:
    if (!u2ps_host.ctl_known) {
      u2ps_host.ctl_known = true;
      u2ps_host.ctl_start = now_ms;
    } else if (!u2ps_host.aborted && now_ms - u2ps_host.ctl_start >= U2PS_HOST_CONTROL_MAX) {
      /* on_control follows, and ends the session */
      u2ps_host.aborted = true;
      u2ps_host.control_timeouts++;
      u2ps_hport_control_abort();
    }
    break;
  /* The test device's waits: 0x0106 suspends, 0x0107 reads the device
   * descriptor, 0x0108 sends the data stage */
  case ST_TD_WAIT:
    if (!waited(T_TEST))
      break;
    if (u2ps_host.td_pid == 0x0106u) {
      u2ps_hport_suspend();
      wait_from_tick(ST_SUSPENDED);
    } else if (u2ps_host.td_pid == 0x0107u) {
      get_descriptor(STEP_TD_DESC, ADDRESS, U2PS_DESC_DEVICE, 0, 0, U2PS_DEVICE_LEN);
    } else {
      in_flight(STEP_TD_DATA);
      u2ps_hport_control_data(ctrl_buf);
    }
    break;
  case ST_SUSPENDED:
    if (waited(T_TEST)) {
      (void)u2ps_hport_resume(true);
      wait_from_tick(ST_RESUME_K);
    }
    break;
  case ST_RESUME_K:
    if (waited(T_DRSMDN)) {
      u2ps_host.state = ST_RESUME_END;
      u2ps_host.resume_first = now_ms;
      resume_poll();
    }
    break;
  case ST_RESUME_END:
    resume_poll();
    break;
  case ST_RESUME_RCY:
    if (waited(T_RSMRCY))
      control(STEP_GET_STATUS, ADDRESS, U2PS_REQ_DIR_IN, U2PS_GET_STATUS, 0, 0, 2);
    break;
  default:
    break;
  }
  unmask();
}

uint32_t u2ps_host_stream_start(uint8_t alternate_setting, uint32_t sampling_frequency)
{
  U2PS_HOST_CHECK_THREAD();
  if (!u2ps_host.port_up)
    return 0;
  mask();
  const u2ps_host_walk_t *w = &u2ps_host.walk;
  uint8_t i = 0;
  while (i < w->alt_count && w->alts[i].alternate_setting != alternate_setting)
    i++;
  bool ok = u2ps_host.state == ST_CONFIGURED && u2ps_host.ss == SS_IDLE && i < w->alt_count &&
            w->alts[i].skip == U2PS_HOST_SKIP_NONE;
  if (ok) {
    u2ps_host.stream++;
    u2ps_host.ss = SS_STARTING;
    u2ps_host.cancel = false;
    u2ps_host.alt = i;
    /* The frequency before SET_INTERFACE: a u2ps device starts its stream
     * at the clock's frequency */
    if (w->clock_programmable)
      set_frequency(sampling_frequency);
    else
      clock_request(STEP_S_GET_FREQ, U2PS_REQ_CUR, U2PS_CS_SAM_FREQ_CONTROL, 4);
  }
  uint32_t number = ok ? u2ps_host.stream : 0u;
#if U2PS_CONTRACT_CHECKS
  if (ok)
    u2ps_host_last_stream = number;
#endif
  unmask();
  return number;
}

void u2ps_host_stream_stop(uint32_t stream)
{
  U2PS_HOST_CHECK_THREAD();
  if (!u2ps_host.port_up)
    return;
  mask();
  /* The close is asynchronous: on_closed goes on. A start ends at the
   * completion of its request in flight. */
  if (stream == u2ps_host.stream && u2ps_host.ss == SS_OPEN) {
    u2ps_host.ss = SS_CLOSING;
    u2ps_host_stream_closing();
    u2ps_hport_stream_close();
  } else if (stream == u2ps_host.stream && u2ps_host.ss == SS_STARTING) {
    u2ps_host.cancel = true;
  }
  unmask();
}

bool u2ps_host_test_mode(uint8_t selector)
{
  U2PS_HOST_CHECK_THREAD();
  if (!u2ps_host.port_up || u2ps_host.state == ST_TEST_MODE || selector < U2PS_TEST_J ||
      selector > U2PS_TEST_FORCE_ENABLE)
    return false;
  mask();
  session_end(U2PS_HOST_END_TEST_MODE, false, ST_TEST_MODE);
  u2ps_hport_disable();
  u2ps_hport_test_mode(selector);
  unmask();
  return true;
}

/* A resume runs: the K, its end, the recovery or GET_STATUS */
static bool resuming(void)
{
  state_t s = u2ps_host.state;
  return s == ST_RESUME_K || s == ST_RESUME_END || s == ST_RESUME_RCY ||
         (s == ST_CONTROL && u2ps_host.step == STEP_GET_STATUS);
}

bool u2ps_host_suspend(void)
{
  U2PS_HOST_CHECK_THREAD();
  if (!u2ps_host.port_up)
    return false;
  mask();
  bool ok = false;
  bool app_session = u2ps_host.session && u2ps_host.td_pid == 0u;
  if (app_session && (u2ps_host.state == ST_SUSPENDED || resuming())) {
#if U2PS_CONTRACT_CHECKS
    if (!u2ps_host.remote)
      U2PS_HOST_CONTRACT(U2PS_CONTRACT_HOST_SUSPEND_SUSPENDED);
#endif
  } else if (u2ps_host.ss != SS_IDLE) {
    /* A stream needs SOFs */
#if U2PS_CONTRACT_CHECKS
    U2PS_HOST_CONTRACT(U2PS_CONTRACT_HOST_SUSPEND_STREAM);
#endif
  } else if (u2ps_host.state == ST_CONFIGURED) {
    u2ps_hport_suspend();
    u2ps_host.state = ST_SUSPENDED;
    u2ps_host.remote = false;
    ok = true;
  }
  unmask();
  return ok;
}

bool u2ps_host_resume(void)
{
  U2PS_HOST_CHECK_THREAD();
  if (!u2ps_host.port_up)
    return false;
  mask();
  bool ok = u2ps_host.state == ST_SUSPENDED && u2ps_host.td_pid == 0u;
  if (ok) {
    (void)u2ps_hport_resume(true);
    u2ps_host.remote = false;
    wait_from_now(ST_RESUME_K);
  }
#if U2PS_CONTRACT_CHECKS
  else if (u2ps_host.session && u2ps_host.td_pid == 0u && !u2ps_host.remote)
    U2PS_HOST_CONTRACT(U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED);
#endif
  unmask();
  return ok;
}

void u2ps_host_stats(u2ps_host_stats_t *stats)
{
  U2PS_HOST_CHECK_THREAD();
  memset(stats, 0, sizeof *stats);
  mask();
  if (u2ps_host.port_up) {
    const u2ps_hport_stats_t *p = u2ps_hport_stats();
    stats->sent = p->sent;
    stats->late_completions = p->late_completions;
    stats->late = p->late;
    stats->no_buffer = p->no_buffer;
    stats->corrupted = p->corrupted;
    stats->fb_polls = p->fb_polls;
    stats->fb_missed = p->fb_missed;
    stats->control_retries = p->control_retries;
    stats->errors = p->errors;
  }
  stats->control_timeouts = u2ps_host.control_timeouts;
  stats->wakeups = u2ps_host.wakeups;
  u2ps_host_stream_stats(stats);
  unmask();
}

/* ---- Upcalls ------------------------------------------------------------------- */

void u2ps_host_on_connect(void)
{
  if (u2ps_host.state == ST_TEST_MODE)
    return;
  u2ps_host.connected = true;
  /* A connect while the debounce runs restarts it */
  if (u2ps_host.state == ST_DEBOUNCE)
    u2ps_host.state = ST_IDLE;
  maybe_start();
}

void u2ps_host_on_disconnect(void)
{
  u2ps_host.connected = false;
  if (u2ps_host.state == ST_TEST_MODE)
    return;
  /* The port has stopped everything itself */
  u2ps_host_in_upcall = true;
  session_end(U2PS_HOST_END_DISCONNECT, false, ST_IDLE);
  u2ps_host_in_upcall = false;
}

void u2ps_host_on_enabled(u2ps_speed_t speed)
{
  if (u2ps_host.state == ST_RESET) {
    /* A segment ended before T_DRSTR (ChipIdea): another one */
    u2ps_hport_reset(true);
  } else if (u2ps_host.state == ST_RESET_END) {
    /* Full and low speed: identified, unless the build refuses them */
    if (speed != U2PS_SPEED_HIGH && U2PS_HOST_REFUSE_FS) {
      u2ps_host_in_upcall = true;
      fail(U2PS_HOST_END_SPEED);
      u2ps_host_in_upcall = false;
      return;
    }
    u2ps_host.speed = speed;
    u2ps_host.mps0 = speed == U2PS_SPEED_HIGH ? EP0_MPS_HS : EP0_MPS_FIRST;
    wait_from_now(ST_RECOVERY);
  }
}

/* A remote wakeup: while the app's suspend lasts, the resume runs from the
 * K the port drives; at any other time it is only counted */
void u2ps_host_on_wakeup(void)
{
  u2ps_host.wakeups++;
  if (u2ps_host.state == ST_SUSPENDED && u2ps_host.td_pid == 0u) {
    u2ps_host.remote = true;
    wait_from_now(ST_RESUME_K);
  }
}

void u2ps_host_on_disabled(void)
{
  u2ps_host_in_upcall = true;
  if (u2ps_host.session)
    session_end(U2PS_HOST_END_DISABLED, false, ST_ENDED);
  u2ps_host_in_upcall = false;
}

void u2ps_host_on_control(u2ps_hport_control_t result, uint16_t len)
{
  if (u2ps_host.state != ST_CONTROL)
    return;
  u2ps_host_in_upcall = true;
  if (u2ps_host.aborted) {
    fail(U2PS_HOST_END_TIMEOUT);
  } else {
    if (len > U2PS_HOST_EP0_BUF)
      len = U2PS_HOST_EP0_BUF;
    u2ps_host_end_t why = step_done(result, len);
    if (why != GO_ON)
      fail(why);
  }
  u2ps_host_in_upcall = false;
}

void u2ps_host_on_closed(void)
{
  if (u2ps_host.ss != SS_CLOSING)
    return;
  u2ps_host_in_upcall = true;
  u2ps_host_stream_end();
  unselect(U2PS_HOST_STOP_APP);
  u2ps_host_in_upcall = false;
}

/* ---- Contract checks (u2ps_types.h, u2ps_contract_t) -------------------------- */

#if U2PS_CONTRACT_CHECKS
bool u2ps_host_masked;
bool u2ps_host_in_callback;
uint32_t u2ps_host_last_stream;

void u2ps_host_mask(bool masked)
{
  if (masked) {
    u2ps_host_masked = true;
    u2ps_hport_irq_mask(true);
  } else {
    u2ps_hport_irq_mask(false);
    u2ps_host_masked = false;
  }
}

/* In the port's interrupt the vectors are not masked, and an upcall runs
 * only the stack's code and the app's callbacks; in thread a callback runs
 * under U2PS_HOST_CALLBACK */
bool u2ps_host_callback(void)
{
  return u2ps_host_in_callback || (u2ps_host_in_upcall && !u2ps_host_masked);
}

void u2ps_host_check_thread(void)
{
  if (u2ps_host_callback())
    U2PS_HOST_CONTRACT(U2PS_CONTRACT_HOST_CALL_IN_CALLBACK);
  else if (u2ps_host_masked)
    U2PS_HOST_CONTRACT(U2PS_CONTRACT_HOST_CALL_NESTED);
}
#endif
