/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_testkit_loopback.h: the PC test kit's loopback (CMake target
 * u2ps_testkit_loopback, which needs both stacks): a player app against
 * the host stack, whose bus leads to the u2ps device stack instead of the
 * scripted device. Control transfers go to the device stack; each packet
 * the host sends arrives there in the microframe of its on_sent, with its
 * bytes; each feedback poll reads the device stack's feedback value. The
 * device stack runs the kit's own DAC app, so the test links only the
 * player app. Everything of u2ps_testkit_host.h applies, except the calls
 * that script the device (configuration, sampling frequencies, feedback,
 * refusals), which the device stack replaces.
 *
 * The kit's DAC app: it accepts every sampling frequency and every start,
 * lends 8 buffers of the least stride, releases each packet in its
 * on_packet, after u2ps_testkit_host_received() counts it and the hook of
 * u2ps_testkit_host_on_packet() sees it. Its callbacks are in the trace
 * too (on_stream_start, on_packet, on_hole, ...), and the device stack
 * ticks at every millisecond. It defines the device stack's callbacks, so
 * a test of the loopback has no DAC app of its own.
 *
 * A test:
 *   u2ps_testkit_loopback_reset(NULL);   first, for every scenario
 *   app_init();                          the player's u2ps_host_init(), ...
 *   u2ps_testkit_host_connect();
 *   u2ps_testkit_host_run_ms(500);       host_on_device
 *   ...                                  as with u2ps_testkit_host.h
 */
#ifndef U2PS_TESTKIT_LOOPBACK_H
#define U2PS_TESTKIT_LOOPBACK_H

#include <stdbool.h>

#include "u2ps_dev.h"
#include "u2ps_testkit_host.h"

/* Starts a scenario: u2ps_testkit_host_reset(), then the device side: the
 * fake device port's state, VBUS present, the kit's DAC app's
 * u2ps_dev_init() with desc (NULL: the default description, below) and
 * u2ps_dev_connect(true), and a bus reset at high speed. desc and what it
 * points to must stay valid and unchanged. Returns false, and prints the
 * reason, if u2ps_dev_init() failed.
 *
 * The default description: VID 0x1234, PID 0x5678, bcdDevice 0x0100,
 * strings "u2ps", "u2ps testkit DAC", "0001"; bus-powered, 100 mA; I/O
 * box, speaker, 2 channels (front left, right); the formats 16/2, 24/4
 * and 32/4 (bits/bytes) PCM in alternate settings 1 to 3; the sampling
 * frequencies 44.1, 48, 88.2, 96, 176.4, 192, 352.8, 384, 705.6 and
 * 768 kHz. */
bool u2ps_testkit_loopback_reset(const u2ps_dev_description_t *desc);

#endif /* U2PS_TESTKIT_LOOPBACK_H */
