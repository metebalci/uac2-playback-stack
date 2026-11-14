/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * main.c: the u2ps test app on the NUCLEO-U5A5ZJ-Q. A UAC2 sink with 2
 * channels, five formats (PCM 16/2, 24/4, 32/4, 24/3: bit
 * resolution/subslot bytes; then IEEE_FLOAT 32/4 as alternate setting 5)
 * and ten rates from 44.1 to 768 kHz, as VID 0x16C0 PID 0x27E0 (the shared
 * pair for audio class devices in Objective Development's
 * USB-IDs-for-free.txt), whose rules ask for a serial number that starts
 * with the distributor's domain: U2PS_EXAMPLE_SERIAL_PREFIX (CMake option)
 * and the chip's 96-bit unique ID in hex. It counts what it receives and
 * releases every
 * packet at once.
 *
 * VBUS mode app (U2PS_VBUS=app): the main loop reads VBUS on PC2
 * (board_vbus()), takes a new level once it has held for 50 ms (the first
 * reading at once), and reports the level with u2ps_dev_vbus() on every
 * pass. The pull-up is on while the attach is allowed and VBUS is present.
 *
 * VCP lines (115200 8N1), written from the main loop only. Each line is
 * one event: its name, a fixed first word, then key=value pairs separated
 * by single spaces, always in the order listed here; a later version may
 * add keys at the end of a line, and never renames or reorders one. A
 * value is a decimal unsigned integer unless stated otherwise; hex values
 * start with 0x; a word holds only a-z, 0-9, '_' and ','; a/b/c/d is a
 * list of decimals separated by '/'; only build and fix are quoted ("...",
 * they hold spaces). Units: cycles are DWT core cycles (160 MHz), us, ms,
 * Hz, bytes and uframes (microframes) as named; a/b/c/d hole lists count
 * by u2ps_miss_t: NO_PACKET/LATE/NO_BUFFER/CORRUPTED.
 * Events the app reports by itself:
 *   boot app=u2ps-test build="..." chip=stm32u5a5 dev=0x... rev=0x...
 *     [test=inject|inject,old_rule|stall|inject,stall|inject,old_rule,stall]
 *     clocks=ok|failed vbus_adc=ok|failed
 *     at start (dev and rev: the chip's IDCODE fields)
 *   init result=ok|failed [reason=description|clock|power|security|phy|
 *     reset|mode|memory|port|dma_memory code=<u2ps_dev_err_t>
 *     [rule=<u2ps_dev_description_error_t, lower case without its prefix>]
 *     [port_reason=ok|clock|power|tzen|phy|reset|mode|fifo]]
 *     desc=a|b|bad cycles=... us=... [fix="..."]
 *     after each u2ps_dev_init(): its result, for reason=description the
 *     rule broken (u2ps_dev_description_error, e.g.
 *     rule=initial_sampling_frequency), for a port's reason the port's own
 *     (u2ps_dwc2_dcd_init_error), the description (a, b or bad, below),
 *     the call's time; fix only for reason=security and dma_memory
 *   reset, suspend, resume (no keys) at on_reset, on_suspend, on_resume
 *   configured speed=high|full|none at on_configured(true)
 *   unconfigured why=reset|set_configuration|detach|vbus|reinit at
 *     on_configured(false)
 *   vbus_change present=0|1 at each change the stack reports (on_vbus);
 *     at a loss after the stream's stop and the configuration's end, in
 *     the order the callbacks came
 *   rate requested=<Hz> rate=<Hz> at the host's SET CUR of the clock: the
 *     value the host sent and the one the stack chose
 *   fault fault=open|close|start code=<u2ps_fault_t>
 *     [cause=count|mem_null|stride_short|stride_align|mem_align|not_dma|
 *     stream] at on_fault and at u2ps_dev_tick()'s faults; cause, for
 *     open only, is u2ps_dwc2_dcd_open_error()
 *   stream_start n=... alt=... bits=... subslot=<bytes> rate=<Hz>
 *     mps=<bytes> format=pcm|ieee_float at on_stream_start
 *   stream_open n=... result=done|not_pending|refused|fault ms=...
 *     cycles=... mask_cycles=... when the main loop opens a deferred
 *     start ("defer"): the ms since on_stream_start, the call's cycles and
 *     the longest time it held the USB vector masked (none in a build with
 *     U2PS_LTO=ON, as mask_max below)
 *   stream_stats <counts> <counters> <load> once a second while streaming
 *   stream_stop <counts> <counters> at on_stream_stop, the stream's totals
 *   stall n=... stalls=... holding=0|1 limit=... held_max=... over=...
 *     packets=... holes=a/b/c/d hole_uframes=a/b/c/d tail=a/b/c/d
 *     tail_uframes=a/b/c/d pauses=... pause_uframes=... pauses_long=...
 *     [out_run=n/avg/max out_stall=n/avg/max] after each stream_stop, only
 *     in a build with -DU2PS_EXAMPLE_STALL=ON (app_stall.h): the stalls
 *     begun, whether one was in progress at the stop, the packets the app
 *     may hold (count - U2PS_DPORT_ISO_OUT_RESERVE) and the most it held,
 *     packets delivered while it held that many, the stream's counts, and
 *     the tail, the holes after its last packet; in an injection build
 *     also the OUT passes (cycles) between stalls (from 50 ms after one
 *     ends to the next) and while the app held every packet it may hold
 *   events_lost count=... events the callbacks could not queue
 *   contract violation=<u2ps_contract_t, lower case without its prefix>
 *     code=<u2ps_contract_t> count=... only in a build with
 *     -DU2PS_CONTRACT_CHECKS=ON: the violations of each kind the stack
 *     reported (u2ps_app_on_contract) since the last such line
 * with
 *   <counts>: n=<stream> ms=<since its start> packets=... bytes=...
 *     holes=a/b/c/d hole_uframes=a/b/c/d pauses=... pause_uframes=...
 *     pauses_long=...
 *   <counters>: late_completions=... late=<uframes> no_buffer=<uframes>
 *     corrupted=... fb_recoveries=... errors=... fallback_over=...
 *     release_errors=... uframe_back=... (u2ps_dev_stats_t), then the
 *     port's iisoixfr=... eopf=... fb_idle=... fb_forced=... fb_arm_busy=...
 *     isoodrp=... pid_drops=... close_drops=... close_polled=... ahberr=...
 *     lost_ack=... b2bstup=... ep0_dropped=... ep0_undone=...
 *     ep0_overrun=... timeouts=... (u2ps_dwc2_dcd_diag_t), then the app's
 *     canary_hits=... tail_checked=... tail_written=... tail_last=0x...
 *     (below) and vcp_dropped=... (lines the VCP dropped)
 *   <load>: the interrupt load since the last line: isr_max=<cycles>
 *     (the longest pass of the USB handler), isr_pct=<percent, two
 *     decimals> (the handler's share of the time), isr_n=<passes> and
 *     mask_max=<cycles> (the longest time the stack held the USB vector
 *     masked from thread; none in a build with U2PS_LTO=ON, where the
 *     link's --wrap of u2ps_dport_irq_mask() does not reach the calls
 *     inside the link-time optimised libraries)
 * Answers to the commands, each with the command's name first:
 *   version app=u2ps-test build="..." chip=... dev=... rev=... [test=...]
 *   reconnect, disconnect, connect (no keys): u2ps_dev_connect(false),
 *     then (true) 100 ms later; (false); (true)
 *   reinit a|b|bad: u2ps_dev_init() again, which detaches first if
 *     attached, answered by the callbacks' lines and then an init line;
 *     the device stays detached until "connect"
 *   speed speed=high|full|none (u2ps_dev_speed)
 *   stats <counters> reply_cycles=... reply_cycles_max=... <load>: the
 *     counters at any time, and the cycles of the last and the longest
 *     deferred reply
 *   vbus present=0|1 level=0|1 mv=... adc=ok|failed: the level the stack
 *     knows, the debounced level, the reading in mV and whether the ADC
 *     runs
 *   diag follow=0|1 window=0|1 out_follow=0|1 out_follows=...
 *     out_follow_late=... fb_opens=... fb_open_uframe=... fb_open_odd=0|1
 *     fb_flips=... fb_switches=... fb_polled=0|1 fb_first_q=...
 *     fb_first_odd=0|1 fb_first_after=<uframes> iiso_hist=8 values
 *     separated by '/' close_cycles=... close_max=... in_setups=...
 *     in_setup_ittxfe=... lost_ack=... b2bstup=... out_gap_follows=...
 *     incompisoout=... incompisoout_other=... out_gap_cleared=...
 *     incomp_nak=... out_gap_own=... eonum_mismatch=...: the
 *     port's switches, its feedback start record, IISOIXFR by microframe,
 *     close times, EP0 counters and the data endpoint's gap follows and
 *     INCOMPISOOUT indications (u2ps_dwc2_dcd.h)
 *   clock core_hz=... frames=... hs=0|1 system_core_clock=<Hz>, or clock
 *     error=no_sof: the core clock counted in DWT cycles over 1 s of the
 *     host's SOFs
 *   mode follow=0|1 window=0|1 out=0|1, to "mode [follow 0|1] [window 0|1]
 *     [out 0|1]": the port's switches for comparisons (the feedback
 *     parity's follow, the IISOIXFR window, the data endpoint parity's
 *     follow), from the next stream
 *   fb value=0x..., to "fb <hex>|off": a fixed feedback value from now on,
 *     or the nominal one again
 *   defer ms=..., to "defer <ms>|off": on_stream_start defers each start
 *     and the main loop opens it that many ms later (a stream_open line);
 *     0 (off, the default) opens at once
 *   canary tail_probe=on|off, to "canary on|off" (the tail probe below)
 *   help commands=<the commands, separated by ','>
 *   error reason=arguments command=<name> for a command's bad arguments;
 *     error reason=unknown_command for any other line
 * In injection builds also: inject every=... us=... injected=... skipped=...
 * timeouts=... d_min_ns=... d_max_ns=..., to "inject <every> <us>|off";
 * ack every=..., to "ack <every>|off"; passes <kind>=<n>/<avg cycles>/<max
 * cycles> for each kind of pass, to "passes [clear]" (u2ps_dwc2_dcd.h;
 * incomp, added in 0.1.1, last); and:
 *   probe every=... probes=... nak_first=... nak_never=... nak_at_xfrc=...
 *     timeouts=... nak_cycles_max=..., to "probe [<every>|off]": the
 *     NAKSTS probe (u2ps_dwc2_dcd.h)
 *
 * Descriptions: a (at boot) has initial_sampling_frequency 0, so the
 * clock starts at rates[0], 44.1 kHz; b differs only in bcdDevice 0x0101
 * and initial_sampling_frequency 48 kHz, so the host can tell which runs
 * (a re-init keeps the clock's sampling frequency, though, as both list
 * it); bad has an initial_sampling_frequency that is not one of rates[],
 * which u2ps_dev_init() refuses without a change.
 *
 * Class memory canaries: a word after each buffer, which no packet may
 * reach, is checked at every packet (canary_hits). With the tail probe on,
 * each buffer is filled with a pattern before it goes back to the port,
 * and after a packet whose length is not a multiple of 4 the bytes up to
 * the next word are checked: tail_written counts packets whose DMA wrote
 * past the packet's end, of tail_checked; tail_last is the last such word.
 *
 * Vendor requests to the AudioControl interface, for bench checks of the
 * control pipe: bRequest 0x01 IN, answered from thread after 2 ms
 * (deferred reply); 0x02 OUT with data, accepted; 0x03 IN, answered at
 * once. The IN answers are wLength bytes counting up from 0.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_class_mem.h"
#include "board.h"
#include "stm32u5xx.h"
#include "u2ps_dport.h"
#include "u2ps_dev.h"
#include "u2ps_dport_cfg.h"
#include "u2ps_dwc2_dcd.h"

#ifndef U2PS_TEST_INJECT
#define U2PS_TEST_INJECT 0
#endif
#ifndef U2PS_EXAMPLE_STALL
#define U2PS_EXAMPLE_STALL 0
#endif
#if U2PS_EXAMPLE_STALL
#include "app_stall.h"
#endif

/* The USB vector's priority: the highest that runs code */
#define PRIO_USB 1u

/* ---- The description -------------------------------------------------------- */

static const u2ps_format_t formats[] = {{2, 16, U2PS_AUDIO_DATA_FORMAT_PCM},
                                        {4, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
                                        {4, 32, U2PS_AUDIO_DATA_FORMAT_PCM},
                                        {3, 24, U2PS_AUDIO_DATA_FORMAT_PCM},
                                        {4, 32, U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT}};
static const uint32_t rates[] = {44100,  48000,  88200,  96000,  176400,
                                 192000, 352800, 384000, 705600, 768000};

/* The serial number: U2PS_EXAMPLE_SERIAL_PREFIX and the unique ID as 24 hex
 * digits, made in main() before u2ps_dev_init() */
#ifndef U2PS_EXAMPLE_SERIAL_PREFIX
#error "U2PS_EXAMPLE_SERIAL_PREFIX is not set: configure the example with CMake, which sets it (option U2PS_EXAMPLE_SERIAL_PREFIX)"
#endif
static char serial[sizeof U2PS_EXAMPLE_SERIAL_PREFIX + 24];

static const u2ps_dev_description_t desc = {
  .id_vendor = 0x16C0,
  .id_product = 0x27E0,
  .bcd_device = 0x0100,
  .manufacturer = "u2ps",
  .product = "u2ps test",
  .serial_number = serial,
  .self_powered = false,
  .max_power_ma = 100,
  .category = 0x08,       /* I/O box */
  .terminal_type = 0x0301, /* speaker */
  .nr_channels = 2,
  .channel_config = 0x3,  /* front left, front right */
  .formats = formats,
  .format_count = sizeof formats / sizeof formats[0],
  .sampling_frequencies = rates,
  .sampling_frequency_count = sizeof rates / sizeof rates[0],
};

/* Descriptions b and bad (above), copies of desc made at start; each stays
 * unchanged once made, as u2ps_dev_init() requires */
static u2ps_dev_description_t desc_b, desc_bad;

/* Class memory: 8 buffers of the largest packet, each followed by a
 * canary word (app_class_mem.h) */
#define BUFFERS 8u
#define CANARY 0xC3A55A3Cu
#define TAIL_FILL 0xA5u
#define STRIDE_MAX APP_STRIDE(U2PS_ISO_OUT_MPS_MAX)
static _Alignas(U2PS_DPORT_ALIGN) uint8_t class_mem[BUFFERS * STRIDE_MAX];
static uint16_t cur_stride, cur_xfr; /* the stream's stride, and its buffers' transfer size */
static volatile bool tail_probe;
static volatile uint32_t fb_nominal; /* the stream's nominal feedback, for "fb off" */
static volatile uint32_t canary_hits, tail_checked, tail_written, tail_last;

/* ---- Counters and events, written in the USB interrupt ---------------------- */

typedef struct {
  uint32_t stream;
  uint32_t packets, bytes;
  uint32_t holes[4], hole_uframes[4]; /* by u2ps_miss_t */
  uint32_t pauses, pause_uframes, pauses_long;
  uint32_t start_ms;
} counts_t;

typedef enum {
  EV_START, EV_STOP, EV_RESET, EV_SUSPEND, EV_RESUME, EV_FAULT, EV_RATE, EV_CONFIGURED,
  EV_UNCONFIGURED, EV_VBUS
} ev_type_t;

typedef struct {
  uint8_t type;
  u2ps_stream_info_t info; /* EV_START */
  counts_t counts;         /* EV_STOP */
#if U2PS_EXAMPLE_STALL
  app_stall_t stall;       /* EV_STOP */
#endif
  uint32_t value;          /* EV_FAULT: the fault; EV_RATE: the rate; EV_CONFIGURED: the speed;
                              EV_UNCONFIGURED: why (u2ps_config_end_t); EV_VBUS: present */
  uint32_t requested;      /* EV_RATE: the value the host sent; EV_FAULT: the port's cause */
} event_t;

#define EVENTS 16u
static event_t events[EVENTS];
/* head: the callbacks (the USB interrupt, or thread with the USB vector
 * masked); tail: the main loop */
static volatile uint32_t ev_head, ev_tail;
static volatile uint32_t ev_lost;

static volatile counts_t run;
static volatile bool streaming;
#if U2PS_EXAMPLE_STALL
/* The stall mode's state: the callbacks, and the main loop with the
 * interrupts off */
static app_stall_t stall;
#endif

/* The deferred vendor request */
static volatile bool deferred;
static volatile uint32_t deferred_id, deferred_ms;
static uint8_t reply[U2PS_EP0_BUF];
/* DWT cycles of u2ps_dev_reply() for it: the last and the longest */
static uint32_t reply_cycles, reply_cycles_max;

/* The interrupt load of the current window: the USB handler's passes, their
 * cycles and the longest; and the longest masked region from thread */
static volatile uint32_t isr_n, isr_sum, isr_max, mask_max;
#if U2PS_EXAMPLE_MASK_TIMED
static uint32_t mask_t0;
/* The longest masked region since the main loop cleared it: around
 * u2ps_dev_stream_open(), that call's */
static uint32_t mask_open_max;
#endif
static uint32_t window_t0;

static event_t *ev_new(ev_type_t type)
{
  uint32_t head = ev_head;
  if ((head + 1u) % EVENTS == ev_tail) {
    ev_lost++;
    return NULL;
  }
  events[head].type = (uint8_t)type;
  return &events[head];
}

static void ev_put(void)
{
  ev_head = (ev_head + 1u) % EVENTS;
}

static void ev_simple(ev_type_t type, uint32_t value)
{
  event_t *e = ev_new(type);
  if (e != NULL) {
    e->value = value;
    ev_put();
  }
}

static void counts_copy(counts_t *to, const volatile counts_t *from)
{
  to->stream = from->stream;
  to->packets = from->packets;
  to->bytes = from->bytes;
  for (unsigned i = 0; i < 4u; i++) {
    to->holes[i] = from->holes[i];
    to->hole_uframes[i] = from->hole_uframes[i];
  }
  to->pauses = from->pauses;
  to->pause_uframes = from->pause_uframes;
  to->pauses_long = from->pauses_long;
  to->start_ms = from->start_ms;
}

/* ---- App callbacks (USB interrupt) -------------------------------------------- */

int u2ps_app_on_request(const u2ps_request_t *req)
{
  const u2ps_setup_t *s = &req->setup;
  bool in = (s->bmRequestType & 0x80u) != 0u;

  if (s->bRequest == 0x01u && in) {
    deferred_id = req->id;
    deferred_ms = board_millis();
    deferred = true;
    return U2PS_REQ_DEFER;
  }
  if (s->bRequest == 0x02u && !in)
    return 0;
  if (s->bRequest == 0x03u && in) {
    for (uint16_t i = 0; i < s->wLength && i < req->size; i++)
      req->buf[i] = (uint8_t)i;
    return s->wLength;
  }
  return U2PS_REQ_STALL;
}

int u2ps_app_on_sampling_frequency(uint32_t requested, uint32_t chosen, uint32_t id)
{
  (void)id;
  event_t *e = ev_new(EV_RATE);
  if (e != NULL) {
    e->value = chosen;
    e->requested = requested;
    ev_put();
  }
  return 0;
}

void u2ps_app_on_configured(bool configured, u2ps_config_end_t why)
{
  if (configured)
    ev_simple(EV_CONFIGURED, (uint32_t)u2ps_dev_speed());
  else
    ev_simple(EV_UNCONFIGURED, (uint32_t)why);
}

/* In the main loop's u2ps_dev_vbus(), with the USB vector masked */
void u2ps_app_on_vbus(bool present)
{
  ev_simple(EV_VBUS, present ? 1u : 0u);
}

static uint32_t get32(const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* A buffer before it goes (back) to the port: its canary, and with the
 * tail probe the pattern over the transfer size */
static void buffer_prepare(uint8_t *buf)
{
  if (tail_probe) {
    for (uint16_t i = 0; i < cur_xfr; i++)
      buf[i] = TAIL_FILL;
  }
  put32(buf + cur_stride - APP_CANARY_BYTES, CANARY);
}

static void buffer_check(const uint8_t *buf, uint16_t len)
{
  if (get32(buf + cur_stride - APP_CANARY_BYTES) != CANARY)
    canary_hits++;
  if (tail_probe && len % 4u != 0u) {
    uint16_t end = (uint16_t)((len + 3u) & ~3u);
    bool written = false;
    for (uint16_t i = len; i < end; i++)
      written = written || buf[i] != TAIL_FILL;
    tail_checked++;
    if (written) {
      tail_written++;
      tail_last = get32(buf + end - 4u);
    }
  }
}

/* The "defer" command: a deferred start, opened by the main loop */
static volatile uint32_t defer_ms; /* 0: open at once */
static volatile bool defer_pending;
static volatile uint32_t defer_stream, defer_since;
static u2ps_stream_cfg_t defer_cfg;

u2ps_stream_start_t u2ps_app_on_stream_start(const u2ps_stream_info_t *info,
                                             u2ps_stream_cfg_t *cfg)
{
  counts_t zero = {0};
  zero.stream = info->stream;
  zero.start_ms = board_millis();
  run = zero;
  cur_stride = APP_STRIDE(info->max_packet_size);
  cur_xfr = (uint16_t)((info->max_packet_size + 3u) & ~3u);
  fb_nominal = cfg->fb_value;
  canary_hits = tail_checked = tail_written = tail_last = 0;
  for (unsigned b = 0; b < BUFFERS; b++)
    buffer_prepare(class_mem + b * cur_stride);
  cfg->mem = class_mem;
  cfg->stride = cur_stride;
  cfg->count = BUFFERS;
#if U2PS_EXAMPLE_STALL
  app_stall_start(&stall, BUFFERS);
#endif
  streaming = true;
  event_t *e = ev_new(EV_START);
  if (e != NULL) {
    e->info = *info;
    ev_put();
  }
  if (defer_ms != 0u) {
    defer_cfg = *cfg;
    defer_stream = info->stream;
    defer_since = board_millis();
    defer_pending = true;
    return U2PS_STREAM_DEFER;
  }
  return U2PS_STREAM_OPEN;
}

void u2ps_app_on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  (void)uframe;
  run.packets++;
  run.bytes += len;
  /* The buffer is the app's until released; data lies in class_mem */
  uint8_t *buf = class_mem + (data - class_mem);
  buffer_check(buf, len);
  buffer_prepare(buf);
#if U2PS_EXAMPLE_STALL
  /* Held until the stall ends; the buffer is ready to go back already */
  if (app_stall_packet(&stall))
    return;
#endif
  u2ps_dev_release(run.stream, 1);
}

void u2ps_app_on_hole(uint16_t uframes, u2ps_miss_t why)
{
  if ((unsigned)why < 4u) {
    run.holes[why]++;
    run.hole_uframes[why] += uframes;
  }
#if U2PS_EXAMPLE_STALL
  app_stall_hole(&stall, uframes, why);
#endif
}

void u2ps_app_on_pause(uint16_t uframes)
{
  run.pauses++;
  if (uframes == U2PS_PAUSE_LONG)
    run.pauses_long++;
  else
    run.pause_uframes += uframes;
}

void u2ps_app_on_stream_stop(void)
{
  streaming = false;
  event_t *e = ev_new(EV_STOP);
  if (e != NULL) {
    counts_copy(&e->counts, &run);
#if U2PS_EXAMPLE_STALL
    e->stall = stall;
#endif
    ev_put();
  }
}

void u2ps_app_on_fault(u2ps_fault_t fault)
{
  event_t *e = ev_new(EV_FAULT);
  if (e != NULL) {
    e->value = (uint32_t)fault;
    e->requested = (uint32_t)u2ps_dwc2_dcd_open_error();
    ev_put();
  }
}

void u2ps_app_on_reset(void)
{
  deferred = false;
  ev_simple(EV_RESET, 0);
}

void u2ps_app_on_suspend(void)
{
  ev_simple(EV_SUSPEND, 0);
}

void u2ps_app_on_resume(void)
{
  ev_simple(EV_RESUME, 0);
}

#if U2PS_CONTRACT_CHECKS
/* Built with -DU2PS_CONTRACT_CHECKS=ON: the contract violations the stack
 * reports, counted per u2ps_contract_t. The hook runs in the USB interrupt
 * or in thread, so it counts with the interrupts off; the main loop prints
 * the counts (contract lines). */
#define CONTRACT_CODES (U2PS_CONTRACT_HOST_QUEUE_UNSIZED + 1)
static volatile uint32_t contracts[CONTRACT_CODES];

void u2ps_app_on_contract(u2ps_contract_t violation)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if ((unsigned)violation < CONTRACT_CODES)
    contracts[violation]++;
  __set_PRIMASK(primask);
}
#endif

/* The USB vector, with its load measured in DWT cycles */
void OTG_HS_IRQHandler(void);
void OTG_HS_IRQHandler(void)
{
  uint32_t t0 = board_cycles();
  u2ps_dwc2_dcd_irq();
  uint32_t dt = board_cycles() - t0;
  isr_n++;
  isr_sum += dt;
  if (dt > isr_max)
    isr_max = dt;
}

#if U2PS_EXAMPLE_MASK_TIMED
/* The device stack's masked regions from thread: the link wraps the port's
 * u2ps_dport_irq_mask() (-Wl,--wrap), so the time between the mask and
 * the unmask is measured; the stack never nests them. Not with U2PS_LTO=ON
 * (CMakeLists.txt): there LTO resolves the stack's calls to the port before
 * the linker could wrap them. */
void __real_u2ps_dport_irq_mask(bool masked);
void __wrap_u2ps_dport_irq_mask(bool masked);
void __wrap_u2ps_dport_irq_mask(bool masked)
{
  if (masked) {
    __real_u2ps_dport_irq_mask(true);
    mask_t0 = board_cycles();
  } else {
    uint32_t dt = board_cycles() - mask_t0;
    if (dt > mask_max)
      mask_max = dt;
    if (dt > mask_open_max)
      mask_open_max = dt;
    __real_u2ps_dport_irq_mask(false);
  }
}
#endif

/* ---- Lines --------------------------------------------------------------------- */

typedef struct {
  char s[1024];
  unsigned n;
} line_t;

static void put_str(line_t *l, const char *s)
{
  while (*s != '\0' && l->n + 1u < sizeof l->s)
    l->s[l->n++] = *s++;
  l->s[l->n] = '\0';
}

static void put_u32(line_t *l, uint32_t v)
{
  char d[11];
  unsigned i = 0;
  do {
    d[i++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v != 0u);
  while (i > 0u && l->n + 1u < sizeof l->s)
    l->s[l->n++] = d[--i];
  l->s[l->n] = '\0';
}

static void put_hex(line_t *l, uint32_t v, unsigned digits)
{
  put_str(l, "0x");
  while (digits-- > 0u && l->n + 1u < sizeof l->s)
    l->s[l->n++] = "0123456789abcdef"[(v >> (4u * digits)) & 0xFu];
  l->s[l->n] = '\0';
}

static void put_kv(line_t *l, const char *key, uint32_t v)
{
  put_str(l, " ");
  put_str(l, key);
  put_str(l, "=");
  put_u32(l, v);
}

/* key=word: a value of letters, digits, '_' and ',' */
static void put_kw(line_t *l, const char *key, const char *word)
{
  put_str(l, " ");
  put_str(l, key);
  put_str(l, "=");
  put_str(l, word);
}

/* a/b/c/d of the four reasons: NO_PACKET, LATE, NO_BUFFER, CORRUPTED */
static void put_quad(line_t *l, const char *key, const uint32_t v[4])
{
  put_str(l, " ");
  put_str(l, key);
  put_str(l, "=");
  for (unsigned i = 0; i < 4u; i++) {
    if (i != 0u)
      put_str(l, "/");
    put_u32(l, v[i]);
  }
}

static void send(line_t *l)
{
  put_str(l, "\r\n");
  board_vcp_write(l->s);
}

static void put_counts(line_t *l, const counts_t *c, uint32_t now)
{
  put_kv(l, "n", c->stream);
  put_kv(l, "ms", now - c->start_ms);
  put_kv(l, "packets", c->packets);
  put_kv(l, "bytes", c->bytes);
  put_quad(l, "holes", c->holes);
  put_quad(l, "hole_uframes", c->hole_uframes);
  put_kv(l, "pauses", c->pauses);
  put_kv(l, "pause_uframes", c->pause_uframes);
  put_kv(l, "pauses_long", c->pauses_long);
}

static void put_stats(line_t *l)
{
  u2ps_dev_stats_t st;
  u2ps_dwc2_dcd_diag_t dg;
  u2ps_dev_stats(&st);
  u2ps_dwc2_dcd_diag(&dg);
  put_kv(l, "late_completions", st.late_completions);
  put_kv(l, "late", st.late);
  put_kv(l, "no_buffer", st.no_buffer);
  put_kv(l, "corrupted", st.corrupted);
  put_kv(l, "fb_recoveries", st.fb_recoveries);
  put_kv(l, "errors", st.errors);
  put_kv(l, "fallback_over", st.fallback_over);
  put_kv(l, "release_errors", st.release_errors);
  put_kv(l, "uframe_back", st.uframe_back);
  put_kv(l, "iisoixfr", dg.iisoixfr);
  put_kv(l, "eopf", dg.eopf);
  put_kv(l, "fb_idle", dg.fb_idle_recoveries);
  put_kv(l, "fb_forced", dg.fb_forced);
  put_kv(l, "fb_arm_busy", dg.fb_arm_busy);
  put_kv(l, "isoodrp", dg.isoodrp);
  put_kv(l, "pid_drops", dg.pid_drops);
  put_kv(l, "close_drops", dg.close_drops);
  put_kv(l, "close_polled", dg.close_polled);
  put_kv(l, "ahberr", dg.ahberr);
  put_kv(l, "lost_ack", dg.lost_ack);
  put_kv(l, "b2bstup", dg.b2bstup);
  put_kv(l, "ep0_dropped", dg.ep0_dropped);
  put_kv(l, "ep0_undone", dg.ep0_undone);
  put_kv(l, "ep0_overrun", dg.ep0_overrun);
  put_kv(l, "timeouts", dg.timeouts);
  put_kv(l, "canary_hits", canary_hits);
  put_kv(l, "tail_checked", tail_checked);
  put_kv(l, "tail_written", tail_written);
  put_str(l, " tail_last=");
  put_hex(l, tail_last, 8);
  put_kv(l, "vcp_dropped", board_vcp_dropped());
}

/* The interrupt load since the last call, then a new window */
static void put_load(line_t *l)
{
  __disable_irq();
  uint32_t n = isr_n, sum = isr_sum, max = isr_max, mmax = mask_max;
  uint32_t now = board_cycles();
  isr_n = isr_sum = isr_max = mask_max = 0;
  uint32_t span = now - window_t0;
  window_t0 = now;
  __enable_irq();
  /* Hundredths of a percent */
  uint32_t pct = span != 0u ? (uint32_t)(((uint64_t)sum * 10000u) / span) : 0u;
  put_kv(l, "isr_max", max);
  put_str(l, " isr_pct=");
  put_u32(l, pct / 100u);
  put_str(l, pct % 100u < 10u ? ".0" : ".");
  put_u32(l, pct % 100u);
  put_kv(l, "isr_n", n);
#if U2PS_EXAMPLE_MASK_TIMED
  put_kv(l, "mask_max", mmax);
#else
  (void)mmax;
  put_kw(l, "mask_max", "none");
#endif
}

static void put_diag(line_t *l)
{
  u2ps_dwc2_dcd_diag_t dg;
  u2ps_dwc2_fb_start_t fs;
  u2ps_dwc2_dcd_mode_t m;
  u2ps_dwc2_dcd_diag(&dg);
  u2ps_dwc2_dcd_fb_start(&fs);
  u2ps_dwc2_dcd_mode_get(&m);
  put_str(l, "diag");
  put_kv(l, "follow", m.fb_follow ? 1u : 0u);
  put_kv(l, "window", m.iiso_window ? 1u : 0u);
  put_kv(l, "out_follow", m.out_follow ? 1u : 0u);
  put_kv(l, "out_follows", dg.out_follows);
  put_kv(l, "out_follow_late", dg.out_follow_late);
  put_kv(l, "fb_opens", fs.opens);
  put_kv(l, "fb_open_uframe", fs.open_uframe);
  put_kv(l, "fb_open_odd", fs.open_odd ? 1u : 0u);
  put_kv(l, "fb_flips", fs.flips);
  put_kv(l, "fb_switches", fs.switches);
  put_kv(l, "fb_polled", fs.polled ? 1u : 0u);
  put_kv(l, "fb_first_q", fs.first_q);
  put_kv(l, "fb_first_odd", fs.first_odd ? 1u : 0u);
  put_kv(l, "fb_first_after", fs.first_after);
  put_str(l, " iiso_hist=");
  for (unsigned i = 0; i < 8u; i++) {
    if (i != 0u)
      put_str(l, "/");
    put_u32(l, dg.iiso_hist[i]);
  }
  put_kv(l, "close_cycles", dg.close_cycles);
  put_kv(l, "close_max", dg.close_max);
  put_kv(l, "in_setups", dg.in_setups);
  put_kv(l, "in_setup_ittxfe", dg.in_setup_ittxfe);
  put_kv(l, "lost_ack", dg.lost_ack);
  put_kv(l, "b2bstup", dg.b2bstup);
  put_kv(l, "out_gap_follows", dg.out_gap_follows);
  put_kv(l, "incompisoout", dg.incompisoout);
  put_kv(l, "incompisoout_other", dg.incompisoout_other);
  put_kv(l, "out_gap_cleared", dg.out_gap_cleared);
  put_kv(l, "incomp_nak", dg.incomp_nak);
  put_kv(l, "out_gap_own", dg.out_gap_own);
  put_kv(l, "eonum_mismatch", dg.eonum_mismatch);
}

#if U2PS_TEST_INJECT
static void put_inject(line_t *l)
{
  u2ps_dwc2_inject_t st;
  u2ps_dwc2_inject_get(&st);
  uint32_t mhz = board_cycles_per_us();
  put_str(l, "inject");
  put_kv(l, "every", st.every);
  put_kv(l, "us", st.us);
  put_kv(l, "injected", st.injected);
  put_kv(l, "skipped", st.skipped);
  put_kv(l, "timeouts", st.timeouts);
  put_kv(l, "d_min_ns", (uint32_t)(((uint64_t)st.d_min * 1000u) / mhz));
  put_kv(l, "d_max_ns", (uint32_t)(((uint64_t)st.d_max * 1000u) / mhz));
}

static void put_passes(line_t *l, bool clear)
{
  static const char *const names[U2PS_DWC2_PASS_KINDS] = U2PS_DWC2_PASS_NAMES;
  u2ps_dwc2_passes_t p;
  u2ps_dwc2_passes_get(&p, clear);
  put_str(l, "passes");
  /* In the kinds' order, but incomp, added in 0.1.1, last */
  for (unsigned i = 0; i < U2PS_DWC2_PASS_KINDS; i++) {
    unsigned k = i == U2PS_DWC2_PASS_KINDS - 1u ? U2PS_DWC2_PASS_INCOMP
                 : i >= U2PS_DWC2_PASS_INCOMP   ? i + 1u
                                                : i;
    put_str(l, " ");
    put_str(l, names[k]);
    put_str(l, "=");
    put_u32(l, p.n[k]);
    put_str(l, "/");
    put_u32(l, p.n[k] != 0u ? (uint32_t)(p.sum[k] / p.n[k]) : 0u);
    put_str(l, "/");
    put_u32(l, p.max[k]);
  }
}
#endif

static void version(line_t *l)
{
  uint32_t id = board_idcode();
  put_str(l, "version app=u2ps-test build=\"" __DATE__ " " __TIME__ "\" chip=stm32u5a5 dev=");
  put_hex(l, id & 0xFFFu, 3);
  put_str(l, " rev=");
  put_hex(l, id >> 16, 4);
#if defined(U2PS_TEST_OLD_RULE) && U2PS_TEST_OLD_RULE
  put_str(l, " test=inject,old_rule");
#elif U2PS_TEST_INJECT
  put_str(l, " test=inject");
#endif
#if U2PS_EXAMPLE_STALL
  put_str(l, U2PS_TEST_INJECT ? ",stall" : " test=stall");
#endif
}

/* The core clock against the host's: DWT cycles over 8000 microframes (or
 * 1000 frames at full speed) of SOFs, that is 1 s of the host's clock.
 * Blocks the main loop for that second; gives up after 3 s without SOFs. */
static void clock_check(line_t *l)
{
  bool hs;
  uint32_t f0 = board_usb_frame(&hs);
  uint32_t mod = hs ? 0x4000u : 0x800u;
  uint32_t n = hs ? 8000u : 1000u;
  uint32_t start = board_millis();
  uint32_t c0 = 0;
  bool ok = false;
  while (board_millis() - start < 3000u) {
    if (board_usb_frame(&hs) != f0) {
      c0 = board_cycles();
      f0 = board_usb_frame(&hs);
      ok = true;
      break;
    }
  }
  uint32_t end = (f0 + n) % mod;
  while (ok && board_usb_frame(&hs) != end) {
    if (board_millis() - start >= 3000u)
      ok = false;
  }
  uint32_t c = board_cycles() - c0;
  put_str(l, "clock");
  if (!ok) {
    put_kw(l, "error", "no_sof");
    return;
  }
  put_kv(l, "core_hz", c);
  put_kv(l, "frames", n);
  put_kv(l, "hs", hs ? 1u : 0u);
  put_kv(l, "system_core_clock", SystemCoreClock);
}

#if U2PS_EXAMPLE_STALL
#if U2PS_TEST_INJECT
/* The OUT passes between stalls and while the app holds every packet it
 * may hold: n, cycles summed, the longest */
typedef struct {
  uint32_t n, max;
  uint64_t sum;
} out_prof_t;

static enum { PROF_SETTLE, PROF_RUN, PROF_FILL, PROF_STALL } prof_phase;
static uint32_t prof_since, prof_stream;
static out_prof_t prof_run, prof_stall;

/* The pass profile since the last call, added to 'to' if not NULL */
static void prof_take(out_prof_t *to)
{
  u2ps_dwc2_passes_t p;
  u2ps_dwc2_passes_get(&p, true);
  if (to != NULL) {
    to->n += p.n[U2PS_DWC2_PASS_OUT];
    to->sum += p.sum[U2PS_DWC2_PASS_OUT];
    if (p.max[U2PS_DWC2_PASS_OUT] > to->max)
      to->max = p.max[U2PS_DWC2_PASS_OUT];
  }
}

static void put_prof(line_t *l, const char *key, const out_prof_t *p)
{
  put_str(l, " ");
  put_str(l, key);
  put_str(l, "=");
  put_u32(l, p->n);
  put_str(l, "/");
  put_u32(l, p->n != 0u ? (uint32_t)(p->sum / p->n) : 0u);
  put_str(l, "/");
  put_u32(l, p->max);
}
#endif

/* Main loop: begins a stall when one is due, and at its end releases what
 * the app held */
static void stall_poll(uint32_t now)
{
  if (!streaming)
    return;
  bool due = app_stall_due(now - run.start_ms);
  __disable_irq();
  bool began = false;
  uint32_t n = 0, stream = run.stream;
  bool full = stall.holding && stall.held >= stall.limit;
  if (due && !stall.holding) {
    app_stall_begin(&stall);
    began = true;
  } else if (!due && stall.holding) {
    n = app_stall_end(&stall);
  }
  __enable_irq();
#if U2PS_TEST_INJECT
  if (stream != prof_stream) {
    prof_stream = stream;
    prof_run = prof_stall = (out_prof_t){0};
    prof_phase = PROF_SETTLE;
    prof_since = now;
  }
  if (began) {
    prof_take(prof_phase == PROF_RUN ? &prof_run : NULL);
    prof_phase = PROF_FILL;
  } else if (prof_phase == PROF_FILL && full) {
    prof_take(NULL);
    prof_phase = PROF_STALL;
  } else if (!due && (prof_phase == PROF_FILL || prof_phase == PROF_STALL)) {
    prof_take(prof_phase == PROF_STALL ? &prof_stall : NULL);
    prof_phase = PROF_SETTLE;
    prof_since = now;
  } else if (prof_phase == PROF_SETTLE && now - prof_since >= 50u) {
    prof_take(NULL);
    prof_phase = PROF_RUN;
  }
#else
  (void)began;
  (void)full;
#endif
  if (n != 0u)
    u2ps_dev_release(stream, (uint16_t)n);
}

static void put_stall(line_t *l, const counts_t *c, const app_stall_t *s)
{
  put_str(l, "stall");
  put_kv(l, "n", c->stream);
  put_kv(l, "stalls", s->stalls);
  put_kv(l, "holding", s->holding ? 1u : 0u);
  put_kv(l, "limit", s->limit);
  put_kv(l, "held_max", s->held_max);
  put_kv(l, "over", s->over);
  put_kv(l, "packets", c->packets);
  put_quad(l, "holes", c->holes);
  put_quad(l, "hole_uframes", c->hole_uframes);
  put_quad(l, "tail", s->tail);
  put_quad(l, "tail_uframes", s->tail_uframes);
  put_kv(l, "pauses", c->pauses);
  put_kv(l, "pause_uframes", c->pause_uframes);
  put_kv(l, "pauses_long", c->pauses_long);
#if U2PS_TEST_INJECT
  put_prof(l, "out_run", &prof_run);
  put_prof(l, "out_stall", &prof_stall);
#endif
}
#endif

/* ---- Main loop ------------------------------------------------------------------ */

static bool str_eq(const char *a, const char *b)
{
  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/* The next word of a command line, at most n - 1 characters; returns the
 * rest */
static const char *word(const char *p, char *w, unsigned n)
{
  unsigned i = 0;
  while (*p == ' ')
    p++;
  while (*p != '\0' && *p != ' ') {
    if (i + 1u < n)
      w[i++] = *p;
    p++;
  }
  w[i] = '\0';
  return p;
}

/* A decimal or (with 0x) hexadecimal number */
static bool number(const char *w, uint32_t *v)
{
  uint32_t x = 0;
  unsigned base = 10u;
  if (w[0] == '0' && (w[1] == 'x' || w[1] == 'X')) {
    base = 16u;
    w += 2;
  }
  if (*w == '\0')
    return false;
  for (; *w != '\0'; w++) {
    unsigned dg;
    if (*w >= '0' && *w <= '9')
      dg = (unsigned)(*w - '0');
    else if (base == 16u && *w >= 'a' && *w <= 'f')
      dg = (unsigned)(*w - 'a' + 10);
    else if (base == 16u && *w >= 'A' && *w <= 'F')
      dg = (unsigned)(*w - 'A' + 10);
    else
      return false;
    x = x * base + dg;
  }
  *v = x;
  return true;
}

/* "mode [follow 0|1] [window 0|1] [out 0|1]" */
static void mode_command(const char *p, line_t *l)
{
  u2ps_dwc2_dcd_mode_t m;
  char w[16], v[16];
  u2ps_dwc2_dcd_mode_get(&m);
  for (;;) {
    p = word(p, w, sizeof w);
    if (w[0] == '\0')
      break;
    p = word(p, v, sizeof v);
    bool on = str_eq(v, "1");
    if ((!on && !str_eq(v, "0")) ||
        (!str_eq(w, "follow") && !str_eq(w, "window") && !str_eq(w, "out"))) {
      put_str(l, "error reason=arguments command=mode");
      return;
    }
    if (str_eq(w, "follow"))
      m.fb_follow = on;
    else if (str_eq(w, "window"))
      m.iiso_window = on;
    else
      m.out_follow = on;
  }
  u2ps_dwc2_dcd_mode(&m);
  put_str(l, "mode");
  put_kv(l, "follow", m.fb_follow ? 1u : 0u);
  put_kv(l, "window", m.iiso_window ? 1u : 0u);
  put_kv(l, "out", m.out_follow ? 1u : 0u);
}

#if U2PS_TEST_INJECT
static void put_probe(line_t *l)
{
  u2ps_dwc2_probe_t st;
  u2ps_dwc2_probe_get(&st);
  put_str(l, "probe");
  put_kv(l, "every", st.every);
  put_kv(l, "probes", st.probes);
  put_kv(l, "nak_first", st.nak_first);
  put_kv(l, "nak_never", st.nak_never);
  put_kv(l, "nak_at_xfrc", st.nak_at_xfrc);
  put_kv(l, "timeouts", st.timeouts);
  put_kv(l, "nak_cycles_max", st.nak_cycles_max);
}

/* "inject <every> <us>", "inject off", "inject" */
static void inject_command(const char *p, line_t *l)
{
  char w[16], v[16];
  p = word(p, w, sizeof w);
  if (str_eq(w, "off")) {
    u2ps_dwc2_inject(0, 0);
  } else if (w[0] != '\0') {
    uint32_t every, us;
    (void)word(p, v, sizeof v);
    if (!number(w, &every) || !number(v, &us) || every == 0u || us > 1000u) {
      put_str(l, "error reason=arguments command=inject");
      return;
    }
    u2ps_dwc2_inject(every, us);
  }
  put_inject(l);
}
#endif

/* VBUS, debounced: a new reading counts once it has held for
 * VBUS_DEBOUNCE_MS; the first reading counts at once, so that a device
 * powered from the bus attaches soon after power-up (u2ps_dev.h, TSIGATT) */
#define VBUS_DEBOUNCE_MS 50u
static bool vbus_level, vbus_last, vbus_read;
static uint32_t vbus_since;

static bool vbus_poll(uint32_t now)
{
  bool v = board_vbus();
  if (!vbus_read) {
    vbus_read = true;
    vbus_level = vbus_last = v;
    vbus_since = now;
  } else if (v != vbus_last) {
    vbus_last = v;
    vbus_since = now;
  } else if (v != vbus_level && now - vbus_since >= VBUS_DEBOUNCE_MS) {
    vbus_level = v;
  }
  return vbus_level;
}

static const char *speed_name(u2ps_speed_t speed)
{
  return speed == U2PS_SPEED_HIGH ? "high" : speed == U2PS_SPEED_FULL ? "full" : "none";
}

/* u2ps_dev_init() with description 'd' named 'name', timed; the line
 * tells its result */
static u2ps_dev_err_t usb_init(const u2ps_dev_description_t *d, const char *name, line_t *l)
{
  /* u2ps_dev_err_t, from the device API alone */
  static const char *const reasons[] = {"ok",  "description", "clock", "power",  "security",
                                        "phy", "reset",       "mode",  "memory", "port",
                                        "dma_memory", "timer"};
  _Static_assert(sizeof reasons / sizeof reasons[0] == U2PS_DEV_ERR_TIMER + 1,
                 "one name per u2ps_dev_err_t value");
  uint32_t c0 = board_cycles();
  u2ps_dev_err_t err = u2ps_dev_init(d);
  uint32_t c = board_cycles() - c0;
  /* u2ps_dwc2_err_t, the port's own reason */
  static const char *const port_reasons[] = {"ok",    "clock", "power", "tzen",
                                             "phy",   "reset", "mode",  "fifo", "cycles"};
  _Static_assert(sizeof port_reasons / sizeof port_reasons[0] == U2PS_DWC2_ERR_CYCLES + 1,
                 "one name per u2ps_dwc2_err_t value");
  /* u2ps_dev_description_error_t, the rule a description broke */
  static const char *const rules[] = {
    "ok",                  "null",           "formats",
    "format_count",        "sampling_frequencies", "sampling_frequency_count",
    "nr_channels",         "max_power_ma",   "self_powered",
    "manufacturer",        "product",        "serial_number",
    "channel_config",      "terminal_type",  "sampling_frequency_order",
    "initial_sampling_frequency", "subslot_size", "bit_resolution",
    "audio_data_format",   "max_packet_size", "configuration_size"};
  _Static_assert(sizeof rules / sizeof rules[0] == U2PS_DESCRIPTION_CONFIGURATION_SIZE + 1,
                 "one name per u2ps_dev_description_error_t value");
  put_str(l, "init");
  if (err == U2PS_DEV_OK) {
    put_kw(l, "result", "ok");
  } else {
    put_kw(l, "result", "failed");
    put_kw(l, "reason",
           (unsigned)err < sizeof reasons / sizeof reasons[0] ? reasons[err] : "unknown");
    put_kv(l, "code", (uint32_t)err);
    if (err == U2PS_DEV_ERR_DESCRIPTION) {
      u2ps_dev_description_error_t r = u2ps_dev_description_error();
      put_kw(l, "rule", (unsigned)r < sizeof rules / sizeof rules[0] ? rules[r] : "unknown");
    } else {
      u2ps_dwc2_err_t pe = u2ps_dwc2_dcd_init_error();
      put_kw(l, "port_reason", (unsigned)pe < sizeof port_reasons / sizeof port_reasons[0]
                                 ? port_reasons[pe]
                                 : "unknown");
    }
  }
  put_kw(l, "desc", name);
  put_kv(l, "cycles", c);
  put_kv(l, "us", c / board_cycles_per_us());
  if (err == U2PS_DEV_ERR_SECURITY)
    put_str(l, " fix=\"TrustZone is enabled: the port needs option byte TZEN = 0\"");
  if (err == U2PS_DEV_ERR_DMA_MEMORY)
    put_str(l, " fix=\"place .bss.u2ps_dma in SRAM1, SRAM2, SRAM3 or SRAM5 with its clock and power on\"");
  return err;
}

/* A fault line: u2ps_fault_t by name and code; for U2PS_FAULT_OPEN the
 * port's cause (u2ps_dwc2_open_err_t), read when the fault came */
static void put_fault(line_t *l, u2ps_fault_t fault, uint32_t cause)
{
  static const char *const faults[] = {"none", "open", "close", "start"};
  static const char *const causes[] = {"ok",           "count",     "mem_null",
                                       "stride_short", "stride_align", "mem_align",
                                       "not_dma",      "stream"};
  put_str(l, "fault");
  put_kw(l, "fault", (unsigned)fault < 4u ? faults[fault] : "unknown");
  put_kv(l, "code", (uint32_t)fault);
  if (fault == U2PS_FAULT_OPEN)
    put_kw(l, "cause", cause < sizeof causes / sizeof causes[0] ? causes[cause] : "unknown");
}

/* u2ps_config_end_t */
static const char *const config_ends[] = {"none", "reset", "set_configuration", "detach", "vbus",
                                          "reinit"};

#if U2PS_CONTRACT_CHECKS
/* One contract line per kind of violation reported since the last call */
static void contracts_print(void)
{
  /* u2ps_contract_t */
  static const char *const names[] = {
    "none",
    "dev_call_in_callback",   "dev_call_nested",        "dev_connect_before_init",
    "dev_tick_late",          "dev_description_changed", "dev_release_count",
    "dev_release_stream",     "dev_reply_result",       "dev_reply_data",
    "dev_callback_result",    "host_call_in_callback",  "host_call_nested",
    "host_connect_before_init", "host_tick_late",       "host_queue_stream",
    "host_queue_data",        "host_queue_full",        "host_queue_unsized"};
  _Static_assert(sizeof names / sizeof names[0] == CONTRACT_CODES,
                 "one name per u2ps_contract_t value");
  for (unsigned i = 1; i < CONTRACT_CODES; i++) {
    __disable_irq();
    uint32_t n = contracts[i];
    contracts[i] = 0;
    __enable_irq();
    if (n == 0u)
      continue;
    line_t l = {.n = 0};
    put_str(&l, "contract");
    put_kw(&l, "violation", names[i]);
    put_kv(&l, "code", i);
    put_kv(&l, "count", n);
    send(&l);
  }
}
#endif

static void events_print(uint32_t now)
{
  while (ev_tail != ev_head) {
    event_t *e = &events[ev_tail];
    line_t l = {.n = 0};
    switch ((ev_type_t)e->type) {
    case EV_START:
      put_str(&l, "stream_start");
      put_kv(&l, "n", e->info.stream);
      put_kv(&l, "alt", e->info.alternate_setting);
      put_kv(&l, "bits", e->info.format.bit_resolution);
      put_kv(&l, "subslot", e->info.format.subslot_size);
      put_kv(&l, "rate", e->info.sampling_frequency);
      put_kv(&l, "mps", e->info.max_packet_size);
      put_kw(&l, "format",
             e->info.format.audio_data_format == U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT ? "ieee_float"
                                                                                   : "pcm");
      break;
    case EV_STOP:
      put_str(&l, "stream_stop");
      put_counts(&l, &e->counts, now);
      put_stats(&l);
#if U2PS_EXAMPLE_STALL
      send(&l);
      l.n = 0;
      put_stall(&l, &e->counts, &e->stall);
#endif
      break;
    case EV_RESET:
      put_str(&l, "reset");
      break;
    case EV_SUSPEND:
      put_str(&l, "suspend");
      break;
    case EV_RESUME:
      put_str(&l, "resume");
      break;
    case EV_FAULT:
      put_fault(&l, (u2ps_fault_t)e->value, e->requested);
      break;
    case EV_RATE:
      put_str(&l, "rate");
      put_kv(&l, "requested", e->requested);
      put_kv(&l, "rate", e->value);
      break;
    case EV_CONFIGURED:
      put_str(&l, "configured");
      put_kw(&l, "speed", speed_name((u2ps_speed_t)e->value));
      break;
    case EV_UNCONFIGURED:
      put_str(&l, "unconfigured");
      put_kw(&l, "why", e->value < sizeof config_ends / sizeof config_ends[0]
                          ? config_ends[e->value]
                          : "unknown");
      break;
    case EV_VBUS:
      put_str(&l, "vbus_change");
      put_kv(&l, "present", e->value);
      break;
    }
    send(&l);
    ev_tail = (ev_tail + 1u) % EVENTS;
  }
}

int main(void)
{
  bool clocks = board_init();
  /* The boot line carries the version line's fields */
  line_t l = {.n = 0};
  version(&l);
  line_t b = {.n = 0};
  put_str(&b, "boot");
  put_str(&b, l.s + sizeof "version" - 1u);
  put_kw(&b, "clocks", clocks ? "ok" : "failed");
  put_kw(&b, "vbus_adc", board_vbus_adc() ? "ok" : "failed");
  send(&b);

  {
    uint32_t uid[3];
    board_uid(uid);
    size_t n = sizeof U2PS_EXAMPLE_SERIAL_PREFIX - 1u;
    memcpy(serial, U2PS_EXAMPLE_SERIAL_PREFIX, n);
    for (unsigned w = 0; w < 3u; w++)
      for (int d = 7; d >= 0; d--)
        serial[n++] = "0123456789ABCDEF"[(uid[w] >> (4 * d)) & 0xFu];
    serial[n] = '\0';
  }
  desc_b = desc;
  desc_b.bcd_device = 0x0101;
  desc_b.initial_sampling_frequency = 48000;
  desc_bad = desc;
  desc_bad.initial_sampling_frequency = 50000;

  l.n = 0;
  bool usb = usb_init(&desc, "a", &l) == U2PS_DEV_OK;
  send(&l);
  if (usb) {
    NVIC_SetPriority(OTG_HS_IRQn, PRIO_USB);
    NVIC_EnableIRQ(OTG_HS_IRQn);
    u2ps_dev_connect(true);
  }

  uint32_t stats_ms = board_millis();
  uint32_t reconnect_ms = 0;
  bool reconnecting = false;
  for (;;) {
    uint32_t now = board_millis();
    /* The level on every pass; the stack acts on changes */
    u2ps_dev_vbus(vbus_poll(now));
    u2ps_fault_t fault = usb ? u2ps_dev_tick(now) : U2PS_FAULT_NONE;
    if (fault == U2PS_FAULT_CLOSE || fault == U2PS_FAULT_START) {
      l.n = 0;
      put_fault(&l, fault, 0);
      send(&l);
    }
    if (defer_pending && now - defer_since >= defer_ms) {
      static const char *const results[] = {"done", "not_pending", "refused", "fault"};
      defer_pending = false;
#if U2PS_EXAMPLE_MASK_TIMED
      mask_open_max = 0u;
#endif
      uint32_t c0 = board_cycles();
      u2ps_open_result_t r = u2ps_dev_stream_open(defer_stream, &defer_cfg);
      uint32_t c = board_cycles() - c0;
      l.n = 0;
      put_str(&l, "stream_open");
      put_kv(&l, "n", defer_stream);
      put_kw(&l, "result", (unsigned)r < 4u ? results[r] : "unknown");
      put_kv(&l, "ms", now - defer_since);
      put_kv(&l, "cycles", c);
#if U2PS_EXAMPLE_MASK_TIMED
      put_kv(&l, "mask_cycles", mask_open_max);
#else
      put_kw(&l, "mask_cycles", "none");
#endif
      send(&l);
    }
    events_print(now);
#if U2PS_EXAMPLE_STALL
    stall_poll(now);
#endif
#if U2PS_CONTRACT_CHECKS
    contracts_print();
#endif
    if (ev_lost != 0u) {
      l.n = 0;
      put_str(&l, "events_lost");
      put_kv(&l, "count", ev_lost);
      send(&l);
      ev_lost = 0;
    }
    if (streaming && now - stats_ms >= 1000u) {
      counts_t c;
      counts_copy(&c, &run);
      stats_ms = now;
      l.n = 0;
      put_str(&l, "stream_stats");
      put_counts(&l, &c, now);
      put_stats(&l);
      put_load(&l);
      send(&l);
    } else if (!streaming) {
      /* The first window of a stream starts with it */
      stats_ms = now;
      line_t discard = {.n = 0};
      put_load(&discard);
    }
    if (deferred && now - deferred_ms >= 2u) {
      deferred = false;
      for (unsigned i = 0; i < sizeof reply; i++)
        reply[i] = (uint8_t)i;
      uint32_t c0 = board_cycles();
      (void)u2ps_dev_reply(deferred_id, reply, (int)sizeof reply);
      reply_cycles = board_cycles() - c0;
      if (reply_cycles > reply_cycles_max)
        reply_cycles_max = reply_cycles;
    }
    if (reconnecting && now - reconnect_ms >= 100u) {
      reconnecting = false;
      u2ps_dev_connect(true);
    }
    board_led_green(streaming);
    board_led_blue(usb);

    const char *cmd = board_vcp_line();
    if (cmd != NULL) {
      char w[16];
      const char *rest = word(cmd, w, sizeof w);
      l.n = 0;
      if (str_eq(cmd, "version")) {
        version(&l);
      } else if (str_eq(cmd, "reconnect") && usb) {
        u2ps_dev_connect(false);
        reconnecting = true;
        reconnect_ms = now;
        put_str(&l, "reconnect");
      } else if (str_eq(cmd, "disconnect") && usb) {
        reconnecting = false;
        u2ps_dev_connect(false);
        put_str(&l, "disconnect");
      } else if (str_eq(cmd, "connect") && usb) {
        reconnecting = false;
        u2ps_dev_connect(true);
        put_str(&l, "connect");
      } else if (str_eq(w, "reinit") && usb) {
        char v[16];
        (void)word(rest, v, sizeof v);
        if (str_eq(v, "a") || str_eq(v, "b") || str_eq(v, "bad")) {
          reconnecting = false;
          /* The callbacks of a detach come first; then the result */
          (void)usb_init(str_eq(v, "a") ? &desc : str_eq(v, "b") ? &desc_b : &desc_bad, v, &l);
          events_print(now);
        } else {
          put_str(&l, "error reason=arguments command=reinit");
        }
      } else if (str_eq(cmd, "speed")) {
        put_str(&l, "speed");
        put_kw(&l, "speed", speed_name(u2ps_dev_speed()));
      } else if (str_eq(cmd, "stats")) {
        put_str(&l, "stats");
        put_stats(&l);
        put_kv(&l, "reply_cycles", reply_cycles);
        put_kv(&l, "reply_cycles_max", reply_cycles_max);
        put_load(&l);
      } else if (str_eq(cmd, "vbus")) {
        put_str(&l, "vbus");
        put_kv(&l, "present", u2ps_dev_vbus_present() ? 1u : 0u);
        put_kv(&l, "level", vbus_level ? 1u : 0u);
        put_kv(&l, "mv", board_vbus_mv());
        put_kw(&l, "adc", board_vbus_adc() ? "ok" : "failed");
      } else if (str_eq(cmd, "diag")) {
        put_diag(&l);
      } else if (str_eq(cmd, "clock")) {
        clock_check(&l);
      } else if (str_eq(w, "mode")) {
        mode_command(rest, &l);
      } else if (str_eq(w, "fb")) {
        char v[16];
        uint32_t value;
        (void)word(rest, v, sizeof v);
        if (str_eq(v, "off")) {
          u2ps_dev_fb_set(fb_nominal);
          put_str(&l, "fb value=");
          put_hex(&l, fb_nominal, 8);
        } else if (number(v, &value)) {
          u2ps_dev_fb_set(value);
          put_str(&l, "fb value=");
          put_hex(&l, value, 8);
        } else {
          put_str(&l, "error reason=arguments command=fb");
        }
      } else if (str_eq(w, "defer")) {
        char v[16];
        uint32_t ms;
        (void)word(rest, v, sizeof v);
        if (str_eq(v, "off") || (number(v, &ms) && ms != 0u)) {
          defer_ms = str_eq(v, "off") ? 0u : ms;
          put_str(&l, "defer");
          put_kv(&l, "ms", defer_ms);
        } else {
          put_str(&l, "error reason=arguments command=defer");
        }
      } else if (str_eq(w, "canary")) {
        char v[16];
        (void)word(rest, v, sizeof v);
        if (str_eq(v, "on") || str_eq(v, "off")) {
          tail_probe = str_eq(v, "on");
          put_str(&l, "canary");
          put_kw(&l, "tail_probe", tail_probe ? "on" : "off");
        } else {
          put_str(&l, "error reason=arguments command=canary");
        }
#if U2PS_TEST_INJECT
      } else if (str_eq(w, "inject")) {
        inject_command(rest, &l);
      } else if (str_eq(w, "ack")) {
        char v[16];
        uint32_t every = 0;
        (void)word(rest, v, sizeof v);
        if (str_eq(v, "off") || number(v, &every)) {
          u2ps_dwc2_inject_lost_ack(every);
          put_str(&l, "ack");
          put_kv(&l, "every", every);
        } else {
          put_str(&l, "error reason=arguments command=ack");
        }
      } else if (str_eq(w, "passes")) {
        char v[16];
        (void)word(rest, v, sizeof v);
        put_passes(&l, str_eq(v, "clear"));
      } else if (str_eq(w, "probe")) {
        char v[16];
        uint32_t every = 0;
        (void)word(rest, v, sizeof v);
        if (v[0] == '\0' || str_eq(v, "off") || number(v, &every)) {
          if (v[0] != '\0')
            u2ps_dwc2_probe(every);
          put_probe(&l);
        } else {
          put_str(&l, "error reason=arguments command=probe");
        }
#endif
      } else if (str_eq(cmd, "help")) {
        put_str(&l, "help commands=version,reconnect,disconnect,connect,reinit,speed,stats,vbus,"
                    "diag,clock,mode,fb,defer,canary,help");
#if U2PS_TEST_INJECT
        put_str(&l, ",inject,ack,passes,probe");
#endif
      } else {
        put_str(&l, "error reason=unknown_command");
      }
      send(&l);
    }
  }
}
