/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_testkit_dev.h: the PC test kit's device side (CMake target
 * u2ps_testkit_device): a DAC app against the device stack. The kit plays
 * the device port and the host: the host's control transfers, packet by
 * packet (USB 2.0 8.5.3), its bus events, and the stream's packets and
 * holes, each as the port's interrupt. See u2ps_testkit.h for what the
 * kit records.
 *
 * A test, in the app's order:
 *   u2ps_testkit_dev_reset();                    first, for every scenario
 *   app_init();                                  the app's u2ps_dev_init(),
 *                                                u2ps_dev_connect(true)
 *   u2ps_testkit_dev_vbus(true);                 VBUS mode app: the board's
 *                                                VBUS poll
 *   u2ps_testkit_dev_enumerate(U2PS_SPEED_HIGH); reset, address, configure
 *   u2ps_testkit_dev_set_sampling_frequency(96000);
 *   u2ps_testkit_dev_set_interface(2);           on_stream_start
 *   u2ps_testkit_dev_packet(576, 8);             on_packet
 *   u2ps_testkit_dev_hole(3, U2PS_MISS_LATE);    on_hole
 *   u2ps_testkit_dev_set_interface(0);           on_stream_stop
 *   return u2ps_testkit_result();
 *
 * The fake port: its constants are u2ps_dport_cfg.h of the kit
 * (U2PS_DPORT_ALIGN 4, U2PS_DPORT_STRIDE(mps) mps rounded up to 4,
 * U2PS_DPORT_ISO_OUT_MIN 2, U2PS_DPORT_ISO_OUT_RESERVE 1, exact
 * microframes). It keeps a stream's class memory as a ring of the buffers
 * the app lent: each packet goes into the next buffer and stays held until
 * the app releases it. It keeps one buffer armed, as DWC2 does, so the app
 * holds at most count - 1: a packet that comes while it does is a NO_BUFFER
 * hole of its own microframe, reported before the next packet; after the
 * stream's last packet, such holes (with the microframes without a packet
 * between them, NO_PACKET) come before on_stream_stop, or at each tick of
 * a long gap, when u2ps_dev_tick() asks for them (u2ps_dev.h,
 * u2ps_app_on_hole). Its DMA reaches all memory. It ends a stream's close
 * (on_closed) at the kit's next step: right after the control transfer
 * that began it, or before the next injection.
 */
#ifndef U2PS_TESTKIT_DEV_H
#define U2PS_TESTKIT_DEV_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_dev.h"
#include "u2ps_dport_cfg.h"
#include "u2ps_testkit.h"

/* How a control transfer ended, as the host saw it */
typedef enum {
  U2PS_TESTKIT_CONTROL_OK,    /* every stage completed */
  U2PS_TESTKIT_CONTROL_STALL, /* a request error: the device stalled */
  /* EP0 still NAKs: the answer is pending (U2PS_REQ_DEFER, a deferred
   * stream start); after the app's u2ps_dev_reply() or
   * u2ps_dev_stream_open(), u2ps_testkit_dev_control_resume() finishes it */
  U2PS_TESTKIT_CONTROL_NAK,
  /* A protocol fault of the device, or the kit used out of order; the kit
   * prints the reason */
  U2PS_TESTKIT_CONTROL_ERROR
} u2ps_testkit_control_t;

/* Starts a scenario: the fake port's state (EP0, address, stream, its log
 * of port calls), the trace, the time (0) and the scripted host's record
 * of the device are reset; the record of violations is kept. The stack
 * itself is not reset: the app's u2ps_dev_init() does that. VBUS stays as
 * it was; it is present at the start of the process. */
void u2ps_testkit_dev_reset(void);

/* VBUS on the cable comes (true) or goes (false). In VBUS mode app this
 * is also the app's VBUS poll: the kit reports the level with
 * u2ps_dev_vbus(), from the thread. In mode bus the stack sees nothing. */
void u2ps_testkit_dev_vbus(bool present);
/* The pull-up is on: the device is attached */
bool u2ps_testkit_dev_attached(void);

/* Bus events, each as the port's interrupt */
void u2ps_testkit_dev_bus_reset(u2ps_speed_t speed); /* reset, ending at speed */
void u2ps_testkit_dev_suspend(void);
void u2ps_testkit_dev_resume(void);

/* One control transfer of the host: SETUP, the data stage packet by packet
 * (the host stops at wLength bytes or at a short packet), the status
 * stage. Device-to-host: the data lands in data, its length in *actual.
 * Host-to-device: data holds wLength bytes to send. data and actual may be
 * NULL without a data stage. */
u2ps_testkit_control_t u2ps_testkit_dev_control(const u2ps_setup_t *setup, void *data,
                                                uint16_t *actual);
/* Finishes a transfer that ended U2PS_TESTKIT_CONTROL_NAK, from where it
 * waited, once the app has answered */
u2ps_testkit_control_t u2ps_testkit_dev_control_resume(void *data, uint16_t *actual);

/* Enumerates the device as a host does: a bus reset at speed, then GET
 * device descriptor, SET_ADDRESS 1, GET device descriptor, configuration
 * descriptor (9 bytes, then wTotalLength), the strings (LANGID and each
 * string index the device descriptor names), SET_CONFIGURATION 1. Stops
 * at the first transfer that does not end OK, prints which, and returns
 * its result. The device must be attached. */
u2ps_testkit_control_t u2ps_testkit_dev_enumerate(u2ps_speed_t speed);
/* The configuration descriptor the last enumeration read, with its
 * length in *len; NULL (and 0) before one */
const uint8_t *u2ps_testkit_dev_configuration(uint16_t *len);

/* SET CUR and GET CUR of the Sampling Frequency Control of the clock that
 * the enumeration found (UAC2 5.2.5.1.1), channel 0 */
u2ps_testkit_control_t u2ps_testkit_dev_set_sampling_frequency(uint32_t sampling_frequency);
u2ps_testkit_control_t u2ps_testkit_dev_get_sampling_frequency(uint32_t *sampling_frequency);
/* SET_INTERFACE of the AudioStreaming interface (1) to alternate_setting:
 * 0 stops the stream, 1 to format_count starts one (on_stream_start). A
 * deferred start ends U2PS_TESTKIT_CONTROL_NAK until the app's
 * u2ps_dev_stream_open() and u2ps_testkit_dev_control_resume(). */
u2ps_testkit_control_t u2ps_testkit_dev_set_interface(uint8_t alternate_setting);

/* The stream, each as the port's interrupt. A packet of len bytes, at most
 * max_packet_size, received in microframe uframe: into the next buffer of
 * the class memory, filled with the bytes of a running counter
 * (u2ps_testkit_dev_packet) or with data, then on_packet. */
void u2ps_testkit_dev_packet(uint16_t len, u2ps_uframe_t uframe);
void u2ps_testkit_dev_packet_bytes(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe);
/* A hole of the port: uframes microframes without a packet, for why (any
 * u2ps_miss_t); the stack reports it as on_hole, or NO_PACKET beyond
 * U2PS_GAP_PAUSE as on_pause */
void u2ps_testkit_dev_hole(uint16_t uframes, u2ps_miss_t why);

/* ms milliseconds of the kit's time: at each, a close due ends, then the
 * poll (u2ps_testkit_set_poll) runs, or the kit calls u2ps_dev_tick() */
void u2ps_testkit_dev_run_ms(uint32_t ms);

/* The value the feedback endpoint sends at its next poll (u2ps_dev_fb_set) */
uint32_t u2ps_testkit_dev_feedback(void);
/* Packets of the stream the app holds: given in on_packet, not released */
unsigned u2ps_testkit_dev_held(void);
/* The port has a stream open */
bool u2ps_testkit_dev_streaming(void);

#endif /* U2PS_TESTKIT_DEV_H */
