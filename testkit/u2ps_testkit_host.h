/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_testkit_host.h: the PC test kit's host side (CMake target
 * u2ps_testkit_host): a player app against the host stack. The kit plays
 * the host port and a scripted UAC2 device behind it, and steps the bus
 * one microframe at a time. See u2ps_testkit.h for what the kit records.
 *
 * A test, in the app's order:
 *   u2ps_testkit_host_reset();           first, for every scenario
 *   app_init();                          the app's u2ps_host_init(),
 *                                        u2ps_host_vbus(true),
 *                                        u2ps_host_connect(true)
 *   u2ps_testkit_host_connect();         the device is plugged in
 *   u2ps_testkit_host_run_ms(500);       enumeration: host_on_device
 *   u2ps_host_stream_start(2, 96000);    the app's thread
 *   u2ps_testkit_host_run_ms(10);        host_on_stream_start, host_on_sent
 *   u2ps_testkit_host_inject_late(2);    host_on_hole(2, LATE)
 *   return u2ps_testkit_result();
 *
 * Time and the bus: the kit's time advances only in the run calls. Each
 * microframe the port sends one queued packet, in queue order; every
 * eighth microframe a millisecond passes: the port's completions due,
 * then the poll (u2ps_testkit_set_poll) or the kit's u2ps_host_tick().
 * The fake port's constants are u2ps_hport_cfg.h of the kit
 * (U2PS_HPORT_ALIGN 4, exact microframes). As the DWC2 host port, the fake
 * port opens no stream whose feedback endpoint has a bInterval above 13
 * (USB 2.0 allows up to 16): the start then ends with
 * U2PS_HOST_STOP_OPEN. Its DMA reaches all of memory, so the contract
 * check of each packet's memory (U2PS_CONTRACT_HOST_QUEUE_DATA) finds
 * nothing on the PC; the chip's rule applies on the board.
 *
 * The scripted device (the default, after the reset): device descriptor
 * VID 0x1234, PID 0x5678, bcdDevice 0x0100, strings "Maker", "Player
 * Test", "SN-0001"; one configuration: AudioControl interface 0 (Clock
 * Source 4, host programmable), AudioStreaming interface 1 whose
 * alternate settings 1 to 4 carry 2 channels of 16/2, 24/4, 32/4 and 24/3
 * (bits/bytes), each with an asynchronous data endpoint and an explicit
 * feedback endpoint; the sampling frequencies 44.1, 48, 88.2, 96, 176.4,
 * 192, 352.8, 384, 705.6 and 768 kHz; 48 kHz at the start. Its feedback
 * endpoint answers the nominal rate of its sampling frequency. It
 * enables at high speed, and its port ends a resume at once (as DWC2).
 */
#ifndef U2PS_TESTKIT_HOST_H
#define U2PS_TESTKIT_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_host.h"
#include "u2ps_hport_cfg.h"
#include "u2ps_testkit.h"

/* What the device received of the stream */
typedef struct {
  uint32_t packets;          /* packets that crossed the bus intact */
  uint64_t bytes;            /* their bytes */
  uint32_t corrupted;        /* packets that crossed it damaged */
  uint16_t last_len;         /* the last intact packet's length */
  u2ps_uframe_t last_uframe; /* and its microframe */
} u2ps_testkit_received_t;

/* Starts a scenario: the fake port's state, the trace, the time (0), the
 * scripted device (the default, not connected) and what it received are
 * reset; the record of violations is kept. The stack itself is not reset:
 * the app's u2ps_host_init() does that. */
void u2ps_testkit_host_reset(void);

/* The device is plugged into the root port, or leaves it, as the port's
 * interrupt */
void u2ps_testkit_host_connect(void);
void u2ps_testkit_host_disconnect(void);
/* The device signals a remote wakeup, a K, as the port's interrupt: while
 * the port is suspended the stack resumes it, and host_on_resumed(true)
 * follows; otherwise the stack only counts it (wakeups) */
void u2ps_testkit_host_wakeup(void);
/* The port is suspended (u2ps_host_suspend, or the test device's
 * suspend), a resume not yet ended */
bool u2ps_testkit_host_suspended(void);
/* The test mode selector the root port is in (1..5), or 0 for none */
uint8_t u2ps_testkit_host_test_mode(void);

/* The kit's time: ms milliseconds, or uframes microframes */
void u2ps_testkit_host_run_ms(uint32_t ms);
void u2ps_testkit_host_run_uframes(uint32_t uframes);
/* Runs until done() returns true, at most max_ms; true if it did */
bool u2ps_testkit_host_run_until(bool (*done)(void), uint32_t max_ms);

/* Holes of the bus, each for the next packet: it goes out 'uframes'
 * microframes late, which the stack reports as one LATE hole before its
 * on_sent; or it does not cross the bus intact: a CORRUPTED hole of 1,
 * then its on_sent. A NO_BUFFER hole needs no injection: it comes when the
 * app's queue runs empty while the stream runs. */
void u2ps_testkit_host_inject_late(uint16_t uframes);
void u2ps_testkit_host_inject_corrupted(void);
/* Packets in the port's queue, not yet sent */
unsigned u2ps_testkit_host_queued(void);

/* The scripted device. Changes take effect at the next enumeration,
 * except those of the requests themselves. */
/* The speed its next reset ends at: U2PS_SPEED_HIGH (the default),
 * U2PS_SPEED_FULL or U2PS_SPEED_LOW. At full speed its bMaxPacketSize0 is
 * 64, at low speed 8. */
void u2ps_testkit_host_speed(u2ps_speed_t speed);
/* Its idVendor, idProduct and bcdDevice: VID 0x1A0A with a PID of the
 * test device (u2ps_host.h) makes it the test device */
void u2ps_testkit_host_device_ids(uint16_t id_vendor, uint16_t id_product, uint16_t bcd_device);
/* Its configuration descriptor (at most 65535 bytes) instead of the
 * default */
void u2ps_testkit_host_configuration(const uint8_t *desc, uint16_t len);
/* Its sampling frequencies: RANGE of n discrete sub-ranges */
void u2ps_testkit_host_sampling_frequencies(const uint32_t *sampling_frequencies, unsigned n);
/* What its feedback endpoint answers: a 16.16 value of audio slots per
 * microframe (UAC2 5.12.4.2); 0: the nominal rate of its sampling
 * frequency (the default) */
void u2ps_testkit_host_feedback(uint32_t value);
/* It stalls SET_INTERFACE to this alternate setting (-1: none, the
 * default), or SET CUR of its sampling frequency (false: none) */
void u2ps_testkit_host_refuse_alternate_setting(int alternate_setting);
void u2ps_testkit_host_refuse_sampling_frequency(bool refuse);
/* Its sampling frequency, as the host last set it */
uint32_t u2ps_testkit_host_sampling_frequency(void);
/* The alternate setting the host last selected */
uint8_t u2ps_testkit_host_alternate_setting(void);
/* What it received of the stream since the reset (in a loopback: what the
 * kit's DAC app received from the device stack) */
const u2ps_testkit_received_t *u2ps_testkit_host_received(void);
/* Called with each packet it receives intact, after its count (NULL: none,
 * the default) */
void u2ps_testkit_host_on_packet(void (*on_packet)(const uint8_t *data, uint16_t len,
                                                   u2ps_uframe_t uframe));

#endif /* U2PS_TESTKIT_HOST_H */
