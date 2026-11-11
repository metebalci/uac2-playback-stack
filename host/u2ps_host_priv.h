/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_host_priv.h: the descriptor walk and the RANGE reader, shared by the
 * files of the host stack. Not part of the app API.
 *
 * References: USB 2.0 Specification, Revision 2.0; UAC2 is the USB Device
 * Class Definition for Audio Devices, Release 2.0; Formats is its Audio Data
 * Formats, Release 2.0; Terminal Types is its Terminal Types, Release 2.0.
 */
#ifndef U2PS_HOST_PRIV_H
#define U2PS_HOST_PRIV_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_host.h"

/* The endpoints of a kept alternate setting, for the stream */
typedef struct {
  uint8_t ep_out;      /* bEndpointAddress of the data endpoint */
  uint8_t ep_fb;       /* of the feedback endpoint the stack follows; 0 for none */
  uint8_t fb_interval; /* its bInterval */
  uint16_t fb_mps;     /* its wMaxPacketSize bits 10:0 */
} u2ps_host_alt_ep_t;

/* Entities and links the first pass keeps, per audio function */
#define U2PS_WALK_ENTITY_MAX 32u
#define U2PS_WALK_LINK_MAX   16u

typedef struct {
  uint8_t subtype; /* bDescriptorSubtype (UAC2 Table A-9) */
  uint8_t id;      /* bTerminalID, bUnitID or bClockID */
  uint8_t a;       /* Input Terminal: bCSourceID; Output Terminal, Feature Unit: bSourceID;
                      Clock Source: bmControls; Clock Selector: bNrInPins */
  uint8_t b;       /* Clock Selector: its first input */
  uint16_t type;   /* Input and Output Terminal: wTerminalType */
} u2ps_walk_entity_t;

/* The walk's result and its scratch memory, the caller's: the walk keeps no
 * state of its own */
typedef struct {
  /* The playable audio function found (U2PS_HOST_REFUSAL_NONE), or the
   * first one tried */
  uint8_t itf_ac;              /* its AudioControl interface */
  uint8_t itf_as;              /* the AudioStreaming interface the stack plays to */
  uint8_t clock;               /* the Clock Source: the target of the clock's requests */
  bool clock_programmable;     /* Clock Frequency Control 0b11 */
  bool clock_valid_readable;   /* Clock Validity Control not 0b00 */
  uint8_t terminal;            /* the USB streaming Input Terminal */
  uint16_t terminal_type;      /* u2ps_host_device_t.terminal_type */
  uint8_t alt_count;           /* alternate settings kept, in ascending order */
  uint8_t alt_dropped;         /* streaming alternate settings beyond U2PS_HOST_ALT_MAX */
  u2ps_host_alternate_setting_t alts[U2PS_HOST_ALT_MAX];
  u2ps_host_alt_ep_t eps[U2PS_HOST_ALT_MAX];

  /* Scratch of the first pass */
  bool ac_found;
  uint8_t ac_protocol;
  bool header;
  uint16_t bcd_adc;
  uint8_t n_entities, n_links;
  u2ps_walk_entity_t entities[U2PS_WALK_ENTITY_MAX];
  uint8_t link_itf[U2PS_WALK_LINK_MAX]; /* an AS interface and its bTerminalLink */
  uint8_t link_terminal[U2PS_WALK_LINK_MAX];
  uint8_t alt_seen[32]; /* bit per alternate setting number seen */
} u2ps_host_walk_t;

/* Walks a configuration descriptor, or an other-speed configuration (USB
 * 2.0 9.6.4), of len bytes (the bytes read; its wTotalLength may say
 * more) for a playable audio function. Fills *w;
 * returns U2PS_HOST_REFUSAL_NONE, or why the configuration has none. */
u2ps_host_refusal_t u2ps_host_walk(const uint8_t *desc, uint16_t len, u2ps_host_walk_t *w);

/* RANGE layout 3 (UAC2 5.2.3.3, Table 5-7) of len bytes: wNumSubRanges,
 * then dMIN, dMAX, dRES each. Copies at most 'max' whole sub-ranges into
 * out and returns how many: wNumSubRanges, limited to the whole sub-ranges
 * in len and to max; 0 if there is none. */
uint16_t u2ps_host_range(const uint8_t *reply, uint16_t len, u2ps_host_sub_range_t *out,
                         uint16_t max);

/* ---- The stream's packet path (u2ps_host_stream.c) -------------------------
 * The control sequence of a start and a stop is in u2ps_host.c; the
 * accumulator, the queue, the feedback and the holes are here. */

/* Inside an upcall, or a masked region of a call from thread: a call of
 * any context must not mask the port's vectors again */
extern bool u2ps_host_in_upcall;

/* ---- Contract checks (U2PS_CONTRACT_CHECKS, u2ps_host.c) ----
 * Without the checks every macro below is the plain code, so that build
 * is the one without them. */
#if U2PS_CONTRACT_CHECKS
/* The port's vectors masked through U2PS_HOST_MASK(): the flag covers the
 * port's masked time from before irq_mask(true) to after irq_mask(false) */
extern bool u2ps_host_masked;
/* An app callback runs in thread, under a thread call (U2PS_HOST_CALLBACK) */
extern bool u2ps_host_in_callback;
/* The last stream number u2ps_host_stream_start() returned */
extern uint32_t u2ps_host_last_stream;
void u2ps_host_mask(bool masked);
/* At the entry of a thread call: inside a callback, or nested in another */
void u2ps_host_check_thread(void);
/* An app callback runs now: in the port's interrupt, or in thread under a
 * thread call */
bool u2ps_host_callback(void);
/* At a successful init: the port's DMA ranges (u2ps_hport_dma_ranges),
 * which u2ps_host_queue() checks each packet against (u2ps_host_stream.c) */
void u2ps_host_stream_ranges(void);
#define U2PS_HOST_CONTRACT(violation) u2ps_app_on_contract(violation)
#define U2PS_HOST_MASK(masked)        u2ps_host_mask(masked)
#define U2PS_HOST_CHECK_THREAD()      u2ps_host_check_thread()
/* An app callback that may run in thread with the vectors masked */
#define U2PS_HOST_CALLBACK(call)                                                              \
  do {                                                                                        \
    u2ps_host_in_callback = true;                                                             \
    call;                                                                                     \
    u2ps_host_in_callback = false;                                                            \
  } while (0)
#else
#define U2PS_HOST_MASK(masked)   u2ps_hport_irq_mask(masked)
#define U2PS_HOST_CHECK_THREAD() ((void)0)
#define U2PS_HOST_CALLBACK(call) call
#endif

/* The port has opened stream 'stream' of alternate setting *a at
 * 'sampling_frequency' Hz (the clock's, read back): the packet path starts,
 * at the nominal rate, with an empty accumulator; packets may be queued */
void u2ps_host_stream_begin(uint32_t stream, const u2ps_host_alternate_setting_t *a,
                            uint32_t sampling_frequency);
/* No packet may be queued from now on (u2ps_host_stream_stop() began the
 * close); the port's reports still come until on_closed */
void u2ps_host_stream_closing(void);
/* The stream has ended: on_closed, or the port stopped it at a session's
 * end; nothing more of it is reported to the app */
void u2ps_host_stream_end(void);
/* At u2ps_host_init(): no stream, the stack's counters zero */
void u2ps_host_stream_reset(void);
/* The packet path's counters into *s (u2ps_host_stats); no_buffer, the
 * port's, less the drained microframes */
void u2ps_host_stream_stats(u2ps_host_stats_t *s);

/* Little-endian fields of descriptors and requests */
static inline uint16_t u2ps_le16(const uint8_t *p)
{
  return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static inline uint32_t u2ps_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#endif /* U2PS_HOST_PRIV_H */
