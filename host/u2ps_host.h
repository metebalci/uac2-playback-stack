/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_host.h: the app API of the host stack, a UAC2 player. Unstable: it
 * may change in any release before 1.0.0.
 *
 * The stack enumerates the one device on the root port, finds its audio
 * function, configures it and reads its clock, and tells the app what it
 * can play (u2ps_app_host_on_device). The app then starts a stream at an
 * alternate setting and a sampling frequency, and queues packets of the
 * size the stack computes, each where it lies in the app's own DMA memory.
 *
 * Context: every app callback (u2ps_app_host_*) runs in the port's
 * interrupt, with one exception: the callbacks that u2ps_host_connect(false),
 * u2ps_host_vbus(false), u2ps_host_test_mode(), a re-init and a resume
 * that does not end make run in thread with the port's vectors masked.
 * Every other wait that u2ps_host_tick() ends continues with a port call,
 * whose completion comes as an upcall in the interrupt, or with none (the
 * port's suspend and resume, which have no completion).
 *
 * The app thread: u2ps_host_init(), u2ps_host_connect(), u2ps_host_vbus(),
 * u2ps_host_tick(), u2ps_host_test_mode(), u2ps_host_suspend(),
 * u2ps_host_resume(), u2ps_host_stream_start(), u2ps_host_stream_stop()
 * and u2ps_host_stats() all run in one thread context, never from an
 * interrupt and never from each other: each masks the port's vectors, and
 * the mask does not nest.
 *
 * Callbacks that also run in thread, with the vectors masked:
 *   callback                 in thread, called from
 *   on_stream_stop(END)      u2ps_host_connect(false), u2ps_host_vbus(false),
 *                            u2ps_host_test_mode(), u2ps_host_init() (a
 *                            re-init)
 *   on_end                   the same calls, and u2ps_host_tick() (a resume
 *                            that does not end: U2PS_HOST_END_ERROR)
 * Every other callback runs only in the port's interrupt, on_test_device
 * and on_resumed included. Inside any callback the app may call
 * u2ps_host_slots_next(), u2ps_host_queue() and u2ps_host_drain(); it
 * must not call the thread calls above: it defers them to its thread.
 *
 * The session. A session begins when the stack starts the root port's
 * reset, and ends with exactly one u2ps_app_host_on_end(), whose reason
 * tells why: a failure of the enumeration, a disconnect, the app's
 * u2ps_host_connect(false), a VBUS loss, a disabled port, test mode or a
 * re-init. A disconnect before the reset begins ends no session. After a
 * failure the stack leaves the device alone (the port is disabled): a new
 * connect of the device, or u2ps_host_connect(false) then (true), tries
 * again.
 *
 * Enumeration (USB 2.0 9.1.2), driven by u2ps_host_tick():
 * 1. 100 ms after the connect (TATTDB, 7.1.7.3), measured from the first
 *    tick after it; a disconnect restarts the wait at the next connect.
 * 2. Reset for 50 ms (TDRSTR, 7.1.7.5).
 * 3. The speed. u2ps streams at high speed only, but a host's port
 *    supports low, full and high speed (7.1.2.3): a full- or low-speed
 *    device is enumerated as far as step 7 to identify it, and then ends
 *    the session (below), unless it is the test device. Built with
 *    U2PS_HOST_REFUSE_FS=1, a documented deviation from 7.1.2.3, the
 *    session ends here with U2PS_HOST_END_SPEED instead.
 * 4. 10 ms of reset recovery (TRSTRCY, 9.2.6.2).
 * 5. GET_DESCRIPTOR(device) at address 0: 64 bytes at high speed, the
 *    first 8 at full and low speed (5.5.3). bMaxPacketSize0 must be 64 at
 *    high speed, 8, 16, 32 or 64 at full speed, 8 at low speed (5.5.3,
 *    9.6.1); every later control transfer uses it.
 * 6. SET_ADDRESS(1), then at least 2 ms (9.2.6.3).
 * 7. The device descriptor. A hub (bDeviceClass 0x09, 11.23.1) ends the
 *    session with U2PS_HOST_END_HUB. The test device goes on as below
 *    (Test device). Then each configuration in turn until one holds a
 *    playable audio function within the power budget; then the strings in
 *    the first LANGID. At full and low speed the stack stops at the first
 *    configuration with a playable audio function and ends the session
 *    with U2PS_HOST_END_SPEED. With none there, it reads the device
 *    qualifier (GET_DESCRIPTOR(DEVICE_QUALIFIER, 10), 9.6.2, p. 264): a
 *    STALL, a full-speed-only device's answer, or a malformed qualifier
 *    ends the session with U2PS_HOST_END_NOT_PLAYABLE; a valid one, each
 *    of its bNumConfigurations other-speed configurations in turn (9.6.4,
 *    pp. 266-267: a configuration's layout, read the same way), until one
 *    holds a playable audio function: U2PS_HOST_END_SPEED; with none,
 *    U2PS_HOST_END_NOT_PLAYABLE. Its refusal is the first full-speed
 *    configuration's.
 * 8. SET_CONFIGURATION.
 * 9. RANGE and CUR of the clock's Sampling Frequency Control (UAC2
 *    5.2.5.1.1), and CUR of its Clock Validity Control if that is readable.
 * 10. u2ps_app_host_on_device().
 * Every control transfer may take U2PS_HOST_CONTROL_MAX ms, counted from
 * the first tick after it began; then the stack aborts it and the session
 * ends with U2PS_HOST_END_TIMEOUT. A STALL of a string request leaves that
 * string NULL, and a STALL of the Clock Validity Control leaves the clock's
 * validity unknown; any other STALL ends the session. The stack does not
 * retry.
 *
 * Power (USB 2.0 9.2.5.1): host software selects no configuration that
 * needs more power than the port can supply. A configuration whose
 * bMaxPower x 2 mA (Table 9-10; bus current, whatever bmAttributes D6
 * says) exceeds U2PS_HOST_IA_VBUS_RATED_MA is not selected. If no
 * configuration is playable within the budget but one was playable apart
 * from its power, the session ends with U2PS_HOST_END_POWER.
 *
 * The test device. USB-IF's test fixtures for an Embedded Host enumerate
 * as the test device, VID 0x1A0A, whose PID selects a test (OTG and EH
 * Supplement 2.0, 6.6.6; Embedded Host Electrical Test Procedure). The
 * stack recognises it at step 7, at any speed, for these PIDs only; every
 * other PID of VID 0x1A0A goes on as any device. It reads configuration 0
 * whole, checks its power as above (over the budget: U2PS_HOST_END_POWER,
 * no test), sends SET_CONFIGURATION(bConfigurationValue) and calls
 * u2ps_app_host_on_test_device(), then:
 *   PID     test                          then
 *   0x0101  TEST_SE0_NAK                  on_end(U2PS_HOST_END_TEST_MODE),
 *   0x0102  TEST_J                        and the root port in test mode
 *   0x0103  TEST_K                        3, 1, 2 or 4 (Table 9-7), as
 *   0x0104  TEST_PACKET                   u2ps_host_test_mode() does
 *   0x0106  HS_HOST_PORT_SUSPEND_RESUME   SOFs for 15 s; suspend for 15 s;
 *                                         resume (TDRSMDN); SOFs
 *   0x0107  SINGLE_STEP_GET_DEV_DESC      SOFs for 15 s; one
 *                                         GET_DESCRIPTOR(device, 18)
 *   0x0108  SINGLE_STEP_SET_FEATURE       the SETUP of GET_DESCRIPTOR(device,
 *                                         18); SOFs for 15 s; its data and
 *                                         status stages
 *   0x0200  TTST_CONFIG                   stays configured, SOFs continue
 * Each 15 s wait lasts more than 15,000 ms and less than 15,002 ms plus
 * one tick period, from the completion of SET_CONFIGURATION (0x0106,
 * 0x0107) or of the SETUP (0x0108); a failed request ends the session
 * with the usual reason. The session of 0x0106, 0x0107, 0x0108 and 0x0200 stays until a
 * disconnect, u2ps_host_connect(false), a VBUS loss, test mode or a
 * re-init; meanwhile u2ps_host_stream_start() and u2ps_host_suspend() are
 * refused. A test mode holds the port until a re-init or a power cycle:
 * offer the user a re-init.
 *
 * Suspend and resume (USB 2.0 7.1.7.6, 7.1.7.7), the app's: with a
 * configured device and no stream, u2ps_host_suspend() stops the SOFs.
 * u2ps_host_resume() drives the resume K for 20 ms (TDRSMDN) until the
 * port is out of resume, waits 10 ms (TRSMRCY) from then, and reads
 * GET_STATUS(device) (9.4.5); u2ps_app_host_on_resumed() follows its
 * completion. A remote wakeup of the device while suspended, a K, resumes
 * the same way, without the app's call: the stack never enables remote
 * wakeup (SET_FEATURE(DEVICE_REMOTE_WAKEUP)), but a device's K is
 * handled. A resume whose port is still in resume 50 ms after the stack
 * first asked it to end ends the session with U2PS_HOST_END_ERROR. The
 * device keeps its address and configuration while suspended (9.1.1.6).
 *
 * The audio function (UAC2 4.6, 4.7): an Interface Association Descriptor
 * of the audio class bounds it, since entity IDs are unique only within a
 * function; in a configuration with several, the first playable one counts.
 * A configuration without one is refused unless the library is built with
 * U2PS_HOST_ACCEPT_NO_IAD=1, a deviation from UAC2 4.6, and then only if it
 * has exactly one AudioControl interface. Within the function, the stack
 * plays to the USB streaming Input Terminal (Terminal Types 2.1), through
 * the AudioStreaming interface linked to it, on the clock of that terminal:
 * a Clock Source, or a Clock Selector with one input that is one. Each of
 * the interface's alternate settings is kept with its format, and skipped,
 * with a reason, only if the stack cannot play it (u2ps_host_skip_t).
 *
 * VBUS (VBUS mode app, extended to the host). Neither controller can supply
 * VBUS, so the board supplies it, with its over-current protection (USB 2.0
 * 7.2.1, 7.2.1.2.1), and the app senses it and reports the level with
 * u2ps_host_vbus() on every poll. While VBUS is absent the stack does not
 * enumerate, and a loss ends a session at once.
 *
 * References: USB 2.0 Specification, Revision 2.0; UAC2 is the USB Device
 * Class Definition for Audio Devices, Release 2.0; Formats is its Audio Data
 * Formats, Release 2.0.
 */
#ifndef U2PS_HOST_H
#define U2PS_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "u2ps_types.h"

/* Build constants. The host library's CMake options set them as public
 * compile definitions, so the library and the app see the same values:
 * Each numeric one stops the build outside its range, with a message that
 * names it and the range.
 * - U2PS_HOST_EP0_BUF (1024): bytes of the control buffer, a multiple of
 *   64, 256 to 65472, and at least the RANGE of U2PS_HOST_SUB_RANGE_MAX,
 *   2 + 12 * U2PS_HOST_SUB_RANGE_MAX bytes (386, so 448, with its default
 *   32). A multiple of 64, because a control IN data stage reserves whole
 *   packets of endpoint 0's size, up to 64 bytes. A configuration
 *   descriptor longer than this ends the session with
 *   U2PS_HOST_END_TOO_LARGE.
 * - U2PS_HOST_ALT_MAX (16; 1 to 255): the most streaming alternate settings kept per
 *   audio function, the lowest-numbered ones; the others are not kept.
 * - U2PS_HOST_SUB_RANGE_MAX (32; 1 to (U2PS_HOST_EP0_BUF - 2) / 12, 85
 *   with the default U2PS_HOST_EP0_BUF): the most sub-ranges of the
 *   clock's RANGE kept.
 * - U2PS_HOST_QUEUE_MAX (16; 1 to 255): the most packets queued at once.
 * - U2PS_HOST_DEBT_MAX (8; 1 to 8000): the most microframes' worth of audio slots a
 *   hole may leave owed; the excess is forgiven and counted in
 *   slots_forgiven.
 * - U2PS_HOST_CONTROL_MAX (5000; 1 to 65535): milliseconds a control transfer may take
 *   (USB 2.0 9.2.6.1 allows a device 5 s).
 * - U2PS_HOST_ACCEPT_NO_IAD (0): 1 accepts a configuration without an
 *   audio IAD if it has exactly one AudioControl interface (above).
 * - U2PS_HOST_STRINGS (1): 0 reads no strings; the device's strings are
 *   then NULL.
 * - U2PS_HOST_IA_VBUS_RATED_MA (no default; 8 to 5,000): the current in mA
 *   the board's VBUS supply of the root port is rated for, IA_VBUS_RATED of
 *   the OTG and EH Compliance Plan (Table 6-3, PI8): the power budget
 *   (Power, above). The CMake option has no default, so every host
 *   integration sets it; a value outside 8..5000 stops the build.
 * - U2PS_HOST_REFUSE_FS (0): 1 ends the session of a full- or low-speed
 *   device at once with U2PS_HOST_END_SPEED, before any request, and so
 *   the test device runs at high speed only: a deviation from USB 2.0
 *   7.1.2.3 (Enumeration, step 3).
 * - U2PS_CONTRACT_CHECKS (0): 1 checks at run time the rules of this
 *   header that the stack can see, and reports each violation through
 *   u2ps_app_on_contract() (CMake option U2PS_CONTRACT_CHECKS, which sets
 *   the device library alike). For integration and tests; without it the
 *   checks compile to nothing. */
#if !defined(U2PS_HOST_EP0_BUF) || !defined(U2PS_HOST_ALT_MAX) ||               \
    !defined(U2PS_HOST_SUB_RANGE_MAX) || !defined(U2PS_HOST_QUEUE_MAX) ||       \
    !defined(U2PS_HOST_DEBT_MAX) || !defined(U2PS_HOST_CONTROL_MAX) ||          \
    !defined(U2PS_HOST_ACCEPT_NO_IAD) || !defined(U2PS_HOST_STRINGS) ||         \
    !defined(U2PS_HOST_IA_VBUS_RATED_MA) || !defined(U2PS_HOST_REFUSE_FS) ||    \
    !defined(U2PS_CONTRACT_CHECKS)
#error "u2ps_host.h: the host library's build constants (U2PS_HOST_EP0_BUF and the others listed above) are missing: link the CMake target u2ps_host, whose public compile definitions set them, rather than only adding host/ to the include path"
#endif

/* ---- The device ------------------------------------------------------------ */

/* One sub-range of the clock's RANGE: dMIN, dMAX, dRES in Hz (UAC2 5.2.1,
 * Table 5-7, the 4-byte layout 5.2.3.3). A discrete frequency has dMIN =
 * dMAX and dRES 0. */
typedef struct {
  uint32_t min, max, res;
} u2ps_host_sub_range_t;

/* bmAttributes D3..2 of the data endpoint (USB 2.0 9.6.6, Table 9-13) */
typedef enum {
  U2PS_SYNCHRONIZATION_TYPE_NONE = 0, /* No Synchronization: played at the nominal rate */
  U2PS_SYNCHRONIZATION_TYPE_ASYNCHRONOUS = 1,
  U2PS_SYNCHRONIZATION_TYPE_ADAPTIVE = 2,
  U2PS_SYNCHRONIZATION_TYPE_SYNCHRONOUS = 3
} u2ps_synchronization_type_t;

/* Why the stack cannot play an alternate setting it kept. The app cannot
 * change the device: it starts a stream on an alternate setting whose skip
 * is U2PS_HOST_SKIP_NONE; a skip only says why another one is left out. */
typedef enum {
  U2PS_HOST_SKIP_NONE = 0,          /* playable */
  /* Not Type I PCM or IEEE_FLOAT (bmFormats D0 or D2, Formats A.2.1; e.g.
   * native DSD, D31), IEEE_FLOAT other than 32 bits in 4-byte subslots
   * (Formats 2.3.1.7.3), or no channel */
  U2PS_HOST_SKIP_FORMAT,
  U2PS_HOST_SKIP_HIGH_BANDWIDTH,    /* more than one transaction per microframe (USB 2.0 5.9.2) */
  U2PS_HOST_SKIP_INTERVAL,          /* a data endpoint not served every microframe (bInterval != 1) */
  U2PS_HOST_SKIP_IMPLICIT_FEEDBACK, /* asynchronous without an explicit feedback endpoint */
  U2PS_HOST_SKIP_MAX_PACKETS_ONLY,  /* EP_GENERAL bmAttributes D7 (UAC2 Table 4-34) */
  U2PS_HOST_SKIP_PACKET_SIZE        /* wMaxPacketSize over 1,024 bytes (USB 2.0 5.6.3) */
} u2ps_host_skip_t;

/* The clock's validity: CUR of its Clock Validity Control (UAC2 5.2.5.1.2) */
typedef enum {
  U2PS_CLOCK_VALIDITY_UNKNOWN = 0, /* the Control is not readable, or it stalled */
  U2PS_CLOCK_VALIDITY_VALID,       /* CUR true: the clock is stable and usable */
  U2PS_CLOCK_VALIDITY_INVALID      /* CUR false */
} u2ps_clock_validity_t;

/* A streaming alternate setting of the audio function */
typedef struct {
  uint8_t alternate_setting; /* bAlternateSetting */
  u2ps_format_t format;      /* bSubslotSize, bBitResolution and the Audio Data Format */
  uint8_t nr_channels;       /* bNrChannels */
  uint32_t channel_config;   /* bmChannelConfig */
  uint16_t max_packet_size;  /* wMaxPacketSize bits 10:0 */
  /* Of the data endpoint, as declared */
  u2ps_synchronization_type_t synchronization_type;
  /* The stack follows the feedback endpoint (an asynchronous endpoint with
   * explicit feedback, USB 2.0 9.6.6). false: it plays at the nominal rate,
   * and ignores a feedback endpoint the alternate setting declares. */
  bool explicit_feedback;
  u2ps_host_skip_t skip; /* U2PS_HOST_SKIP_NONE: playable; otherwise why not */
} u2ps_host_alternate_setting_t;

/* The configured device and its audio function, given at
 * u2ps_app_host_on_device(). It and everything it points to are the
 * stack's, valid until u2ps_app_host_on_end(). */
typedef struct {
  uint16_t id_vendor, id_product, bcd_device; /* idVendor, idProduct, bcdDevice */
  /* The strings of iManufacturer, iProduct and iSerialNumber in the first
   * LANGID, as ASCII: each UTF-16 code unit outside 0x20..0x7E becomes
   * '?'. NULL if the device has none, it stalled the request, or the
   * library reads no strings (U2PS_HOST_STRINGS 0). */
  const char *manufacturer, *product, *serial_number;
  /* wTerminalType of the Output Terminal the USB streaming Input Terminal
   * feeds directly or through Feature Units only (UAC2 Tables 4-10, 4-13);
   * 0 if none */
  uint16_t terminal_type;
  /* The clock's Clock Frequency Control is host programmable (Clock Source
   * bmControls D1..0 = 0b11, UAC2 Table 4-6) */
  bool sampling_frequency_host_programmable;
  uint32_t sampling_frequency; /* the clock's, GET CUR after SET_CONFIGURATION */
  /* The clock's validity, read after sampling_frequency, and again at each
   * u2ps_host_stream_start(), which updates it here */
  u2ps_clock_validity_t clock_validity;
  /* The sub-ranges of the clock's RANGE in sub_ranges[]: wNumSubRanges, but
   * no more than the reply held whole, nor U2PS_HOST_SUB_RANGE_MAX; at
   * least 1 */
  uint16_t num_sub_ranges;
  const u2ps_host_sub_range_t *sub_ranges;
  /* The kept streaming alternate settings, at most U2PS_HOST_ALT_MAX, in
   * ascending alternate_setting; at least one is playable */
  uint8_t alternate_setting_count;
  const u2ps_host_alternate_setting_t *alternate_settings;
} u2ps_host_device_t;

/* Why a session ended (u2ps_app_host_on_end): each value names its cause
 * and what to change. After a failure the stack leaves the device alone
 * until it reconnects, or until u2ps_host_connect(false) then (true). */
typedef enum {
  U2PS_HOST_END_DISCONNECT = 1, /* the device disconnected */
  U2PS_HOST_END_DETACH,         /* the app's u2ps_host_connect(false) */
  /* The app's u2ps_host_vbus(false): if VBUS was not meant to go, check
   * the board's VBUS switch, its over-current flag and the app's reading */
  U2PS_HOST_END_VBUS,
  /* The port disabled itself (port babble): a device or signal fault;
   * check the cable, then reconnect the device. On DWC2 the port first
   * resets its core inside the OTG_HS handler: one pass of about 26 ms,
   * at most 66 ms, in which thread code and every interrupt at or below
   * the USB priority wait. Give audio DMA, watchdog feeds and safety polls
   * a higher priority or a 66 ms margin. */
  U2PS_HOST_END_DISABLED,
  /* The app's u2ps_host_test_mode(), or the test device's PID 0x0101 to
   * 0x0104 (u2ps_app_host_on_test_device came first): the port stays in
   * test mode until u2ps_host_init() or a power cycle; offer a re-init */
  U2PS_HOST_END_TEST_MODE,
  U2PS_HOST_END_REINIT,         /* the app's u2ps_host_init() */
  /* The device enabled at full or low speed, and one of its
   * configurations, or of the other-speed configurations its device
   * qualifier announces (USB 2.0 9.6.2, 9.6.4), holds a playable audio
   * function (with U2PS_HOST_REFUSE_FS=1: at once, with no request): u2ps
   * streams at high speed only; connect a high-speed device directly,
   * with a high-speed cable */
  U2PS_HOST_END_SPEED,
  /* bMaxPacketSize0 is not one the speed allows: 64 at high speed, 8, 16,
   * 32 or 64 at full speed, 8 at low speed (USB 2.0 5.5.3, 9.6.1): the
   * device is not compliant; nothing to change in the app */
  U2PS_HOST_END_EP0_MPS,
  /* The device descriptor or a configuration's header is malformed: the
   * device is not compliant; nothing to change in the app */
  U2PS_HOST_END_DESCRIPTOR,
  /* A configuration descriptor is longer than U2PS_HOST_EP0_BUF: build
   * the library with a larger -DU2PS_HOST_EP0_BUF (a multiple of 64, at
   * least the descriptor's wTotalLength) */
  U2PS_HOST_END_TOO_LARGE,
  /* No configuration holds a playable audio function: the refusal reason
   * that on_end gives tells why (u2ps_host_refusal_t, with its fix). At
   * full or low speed no other-speed configuration does either, or the
   * device has no device qualifier (a full-speed-only device), and the
   * reason is that of the first configuration the device gives at that
   * speed. */
  U2PS_HOST_END_NOT_PLAYABLE,
  /* The clock's RANGE or CUR stalled or is malformed: the device's clock
   * is not UAC2-compliant (5.2.5.1.1); nothing to change in the app */
  U2PS_HOST_END_CLOCK,
  /* The device stalled another request it must answer: the device is not
   * compliant, or it is not ready; reconnect it */
  U2PS_HOST_END_STALL,
  /* Transaction errors beyond the port's retries: the signal (cable, hub)
   * or a device that stopped answering; reconnect it */
  U2PS_HOST_END_ERROR,
  /* A control transfer took longer than U2PS_HOST_CONTROL_MAX ms: the
   * device stopped answering; a device known to be slow needs a larger
   * -DU2PS_HOST_CONTROL_MAX (USB 2.0 9.2.6.1 allows 5 s, the default) */
  U2PS_HOST_END_TIMEOUT,
  /* The device is a hub (bDeviceClass 0x09, USB 2.0 11.23.1): the host
   * stack drives no hubs; connect the device directly */
  U2PS_HOST_END_HUB,
  /* Every configuration that is playable, or the test device's, needs
   * more than U2PS_HOST_IA_VBUS_RATED_MA (bMaxPower x 2 mA, USB 2.0
   * 9.2.5.1): the device needs more current than the port supplies; power
   * it on its own, or, if the board's VBUS supply is rated higher, build
   * with that rating */
  U2PS_HOST_END_POWER
} u2ps_host_end_t;

/* Why a configuration holds no playable audio function: the refusal reason
 * that comes with U2PS_HOST_END_NOT_PLAYABLE, that of the first
 * configuration (and in it, of the first audio function); NONE with every
 * other end. Each names what the device's descriptors lack: the app
 * cannot change them, so the device cannot be played, except with
 * U2PS_HOST_REFUSAL_NO_IAD, which the build option U2PS_HOST_ACCEPT_NO_IAD
 * changes. */
typedef enum {
  U2PS_HOST_REFUSAL_NONE = 0,     /* a playable audio function */
  U2PS_HOST_REFUSAL_CUT,          /* it ends early: wTotalLength beyond the bytes read, or a
                                     descriptor beyond wTotalLength */
  U2PS_HOST_REFUSAL_LENGTH,       /* a descriptor with bLength below 2, or too short for its type */
  U2PS_HOST_REFUSAL_NOT_CONFIG,   /* not a configuration descriptor */
  U2PS_HOST_REFUSAL_NOT_AUDIO,    /* no AudioControl interface (in the function) */
  U2PS_HOST_REFUSAL_NO_IAD,       /* AudioControl interfaces but no audio IAD (UAC2 4.6), and
                                     U2PS_HOST_ACCEPT_NO_IAD off or more than one of them:
                                     with one, -DU2PS_HOST_ACCEPT_NO_IAD=ON accepts it (a
                                     deviation from UAC2 4.6) */
  U2PS_HOST_REFUSAL_UAC1,         /* an audio function of USB Audio Class 1.0 */
  U2PS_HOST_REFUSAL_VERSION,      /* no class-specific AC header, or bcdADC other than 2.00 */
  U2PS_HOST_REFUSAL_NO_TERMINAL,  /* no USB streaming Input Terminal */
  U2PS_HOST_REFUSAL_NO_STREAMING, /* no AudioStreaming interface linked to it, or none with
                                     alternate setting 0 without endpoints */
  U2PS_HOST_REFUSAL_CLOCK,        /* the terminal's clock is no Clock Source, nor a Clock
                                     Selector with one input that is one */
  U2PS_HOST_REFUSAL_NO_ALT,       /* no streaming alternate setting besides 0 */
  U2PS_HOST_REFUSAL_ALT_REPEATED, /* one alternate setting twice */
  U2PS_HOST_REFUSAL_FORMAT,       /* an alternate setting without its AS general or format type
                                     descriptor, or a Type I format whose subslot and resolution
                                     do not fit together */
  U2PS_HOST_REFUSAL_ENDPOINT,     /* endpoints other than one isochronous OUT data endpoint and,
                                     for explicit feedback, one isochronous IN feedback endpoint
                                     of at least 4 bytes */
  U2PS_HOST_REFUSAL_ALL_SKIPPED   /* every kept alternate setting is skipped */
} u2ps_host_refusal_t;

/* ---- The stream ------------------------------------------------------------ */

/* What a started stream plays, at u2ps_app_host_on_stream_start() */
typedef struct {
  uint32_t stream;             /* the stream's number, counting from 1; never 0 */
  uint8_t alternate_setting;   /* bAlternateSetting */
  u2ps_format_t format;
  uint8_t nr_channels;
  uint32_t sampling_frequency; /* the clock's, read back after the stack set it */
  uint16_t max_packet_size;    /* no packet is longer */
  u2ps_synchronization_type_t synchronization_type;
  bool explicit_feedback;      /* the rate follows the device's feedback */
  /* The nominal rate, sampling_frequency / 8000 audio slots per
   * microframe, as 16.16 rounded to the nearest, for logs; the stack's
   * accumulator counts it exactly */
  uint32_t nominal_rate;
  /* The clock's validity, read during the start (U2PS_CLOCK_VALIDITY_UNKNOWN
   * if its Control is not readable or stalled); the device's clock_validity
   * holds the same */
  u2ps_clock_validity_t clock_validity;
} u2ps_host_stream_info_t;

/* Why a stream stopped (u2ps_app_host_on_stream_stop): each value names
 * its cause and what to change */
typedef enum {
  U2PS_HOST_STOP_APP = 1, /* the app's u2ps_host_stream_stop(), of a stream or of its start */
  U2PS_HOST_STOP_END,     /* the session ended; u2ps_app_host_on_end() follows with why */
  /* The device stalled a request of the start (SET CUR of the sampling
   * frequency, its GET CUR, or SET_INTERFACE), or read back a sampling
   * frequency of 0 or not 4 bytes long; the device stays configured.
   * on_stream_stop's 'request' names which (u2ps_host_stop_request_t).
   * Start again with a sampling frequency of the device's sub_ranges[] and
   * an alternate setting with skip U2PS_HOST_SKIP_NONE; if those were
   * used, the device refuses the pair. */
  U2PS_HOST_STOP_REFUSED,
  /* The port refused the stream (u2ps_hport_stream_open: it does not fit
   * the port, whose header names its limits): start another alternate
   * setting. On the DWC2 host port also a feedback endpoint with bInterval
   * 14..16: the port supports 1..13, since it counts microframes modulo
   * 2^14 with signed distances, which a poll period of 2^13 microframes or
   * more exceeds (u2ps_dwc2_hcd.h); such a device cannot be played there */
  U2PS_HOST_STOP_OPEN
} u2ps_host_stop_t;

/* Which request of a start the device refused: the cause detail that comes
 * with U2PS_HOST_STOP_REFUSED (u2ps_app_host_on_stream_stop); NONE with
 * every other reason. The device stays configured; the app cannot change
 * the device, only what it asks for. A STALL of the Clock Validity
 * Control's CUR refuses nothing: the start goes on with the clock's
 * validity unknown. */
typedef enum {
  U2PS_HOST_STOP_REQUEST_NONE = 0, /* not U2PS_HOST_STOP_REFUSED */
  /* SET CUR of the clock's Sampling Frequency Control stalled: the device
   * refuses that frequency; start with one of its sub_ranges[] */
  U2PS_HOST_STOP_REQUEST_SET_SAMPLING_FREQUENCY,
  /* GET CUR of it stalled: the device does not report its clock's
   * frequency (UAC2 5.2.5.1.1 requires it); it cannot be played */
  U2PS_HOST_STOP_REQUEST_GET_SAMPLING_FREQUENCY,
  /* GET CUR of it read back 0 Hz, or not 4 bytes: the device's clock does
   * not run at a frequency it can report; start again, or with another
   * frequency of its sub_ranges[] */
  U2PS_HOST_STOP_REQUEST_SAMPLING_FREQUENCY_VALUE,
  /* SET_INTERFACE to the alternate setting stalled: the device refuses
   * that alternate setting, at least at this frequency; start another
   * one, or another frequency */
  U2PS_HOST_STOP_REQUEST_SET_INTERFACE
} u2ps_host_stop_request_t;

/* The counters: the port's (u2ps_hport_stats_t) and the stack's own, since
 * u2ps_host_init(). The port reports a drained microframe as NO_BUFFER;
 * no_buffer here leaves those out. */
typedef struct {
  uint32_t sent, late_completions, late, no_buffer, corrupted, fb_polls, fb_missed,
    control_retries, errors;
  uint32_t drained;          /* drained microframes (u2ps_host_drain) */
  uint32_t fb_zero_length;   /* feedback polls that brought a zero-length packet */
  uint32_t fb_rejected;      /* feedback values of another length, top bits set or out of the window */
  uint32_t slots_clamped;    /* audio slots beyond max_packet_size, dropped */
  uint32_t slots_forgiven;   /* audio slots owed beyond U2PS_HOST_DEBT_MAX, dropped */
  uint32_t control_timeouts; /* control transfers aborted after U2PS_HOST_CONTROL_MAX */
  uint32_t queue_errors;     /* u2ps_host_queue() calls refused */
  uint32_t uframe_back;      /* packets reported sent not after the last one's microframe */
  uint32_t wakeups;          /* remote wakeups the port reported (u2ps_host_on_wakeup) */
} u2ps_host_stats_t;

/* ---- Calls ------------------------------------------------------------------- */

/* DMA memory. The stack's SETUP packet and control buffer and the port's
 * own memory (port memory) lie in section .bss.u2ps_dma (U2PS_DMA_SECTION,
 * u2ps_hport.h), which the app's linker script places in RAM the port's
 * DMA reaches, as for the device stack (u2ps_dev.h, DMA memory): on DWC2
 * the rule of u2ps_dwc2_dma_ok() (port/dwc2/common/u2ps_dwc2.h), on the
 * STM32U5 with each SRAM clocked and powered, as at reset; the packets the
 * app queues are its own DMA memory. u2ps_host_init() checks every byte of
 * the section's memory first, at every init, and the stack and the port
 * zero it there, so the section may be NOLOAD and the startup code need
 * not zero it. */

/* App thread (Context, above): checks the DMA memory, resets the stack and
 * starts the port (u2ps_hport_init). The stack waits for
 * u2ps_host_connect(true) and VBUS. Returns U2PS_HOST_OK,
 * U2PS_HOST_ERR_DMA_MEMORY if the stack's or the port's DMA memory lies
 * where the port's DMA does not reach it (then nothing changes, not even
 * at a re-init), or the port's reason why the controller did not start.
 * Called again it re-inits: a session ends with U2PS_HOST_END_REINIT, the
 * controller resets, the session permission ends and test mode ends; the
 * VBUS level is kept. Each init zeroes the stack's DMA memory. */
u2ps_host_err_t u2ps_host_init(void);

/* App thread, the one of u2ps_host_init() and tick: allow (true) or end
 * (false) sessions. A session starts once
 * a device is connected, VBUS is present and sessions are allowed; false
 * ends a session with U2PS_HOST_END_DETACH and disables the port. Refused,
 * with no effect, in test mode and before a successful u2ps_host_init(). */
void u2ps_host_connect(bool on);

/* App, thread (never from an interrupt): the VBUS level the app senses,
 * called with it on every poll; the stack acts on changes only. A loss
 * ends a session with U2PS_HOST_END_VBUS. Until the first call VBUS is
 * absent; calls before u2ps_host_init() count. */
void u2ps_host_vbus(bool present);

/* App thread: the time in milliseconds, called at least every 10 ms, the
 * one tick period of both stacks. It runs the waits of the enumeration and
 * the control timeouts, each measured from the first tick after the wait
 * began. Each wait is a lower bound (TATTDB, TDRSTR, TRSTRCY, 9.2.6.3,
 * TDRSMDN, TRSMRCY): it ends at the first tick that reads more than its
 * value in milliseconds since its start, so it lasts more than its value
 * at any phase of the counter, and a longer period only lengthens it, by
 * up to one period, and so each timeout. */
void u2ps_host_tick(uint32_t now_ms);

/* App, thread: puts the root port into test mode 'selector', 1..5 (USB 2.0
 * Table 9-7, 7.1.20): a session ends with U2PS_HOST_END_TEST_MODE, the port
 * is disabled, then u2ps_hport_test_mode(selector). Returns false, changing
 * nothing, for another selector, in test mode, or before a successful
 * u2ps_host_init(). Only u2ps_host_init() or a power cycle ends test mode;
 * until then u2ps_host_connect() is refused and no session starts. */
bool u2ps_host_test_mode(uint8_t selector);

/* App, thread: suspends the root port (Suspend and resume, above): the
 * port stops its SOFs, and the device suspends 3 ms later (USB 2.0
 * 7.1.7.6). Returns true, or false, changing nothing:
 * (1) before a successful u2ps_host_init() (call it first);
 * (2) without a configured device: before u2ps_app_host_on_device(), and
 *     in the test device's session (wait for on_device);
 * (3) while a stream starts, runs or stops (stop it, and suspend after its
 *     on_stream_stop): contract U2PS_CONTRACT_HOST_SUSPEND_STREAM;
 * (4) while already suspended or resuming (wait for on_resumed): contract
 *     U2PS_CONTRACT_HOST_SUSPEND_SUSPENDED;
 * (5) in test mode (re-init with u2ps_host_init()).
 * While suspended, u2ps_host_stream_start() returns 0; a disconnect, a
 * VBUS loss, u2ps_host_connect(false), test mode and a re-init end the
 * session as at any time. */
bool u2ps_host_suspend(void);

/* App, thread: resumes the port that u2ps_host_suspend() suspended:
 * u2ps_app_host_on_resumed(false) follows once the device answers
 * GET_STATUS (Suspend and resume, above). Returns true, or false,
 * changing nothing, (6) when the port is not suspended, or a resume runs
 * (wait for on_resumed): contract U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED,
 * except after the session ended or for a resume that the device's remote
 * wakeup began. A STALL, an error or a timeout of GET_STATUS ends the
 * session with U2PS_HOST_END_STALL, ERROR or TIMEOUT. */
bool u2ps_host_resume(void);

/* App, thread: plays alternate setting 'alternate_setting' at
 * 'sampling_frequency' Hz. SET CUR of the clock's frequency (only if it is
 * host programmable), GET CUR to read it back, CUR of its Clock Validity
 * Control once if readable, SET_INTERFACE, then the port opens the stream,
 * and u2ps_app_host_on_stream_start() follows. The frequency read back
 * becomes the device's sampling_frequency, and the validity read its
 * clock_validity (u2ps_host_device_t) and the stream's
 * (u2ps_host_stream_info_t). Returns the new stream's number, the one
 * on_stream_start will give, so that the start can be stopped while it
 * runs (u2ps_host_stream_stop); 0, changing nothing, without a configured
 * device (wait for u2ps_app_host_on_device(); also while suspended or
 * resuming, and in the test device's session), while a stream starts,
 * runs or stops (stop it, and wait for its on_stream_stop), or for an
 * alternate setting not kept or not playable (pick one of the device's
 * alternate_settings[] with skip U2PS_HOST_SKIP_NONE). Every start accepted
 * gets exactly one u2ps_app_host_on_stream_stop(), with or without an
 * on_stream_start before it. A STALL of a request of the start gives
 * U2PS_HOST_STOP_REFUSED, with the request in on_stream_stop's
 * 'request', and leaves the device configured; a transaction error or a
 * timeout ends the session. */
uint32_t u2ps_host_stream_start(uint8_t alternate_setting, uint32_t sampling_frequency);

/* Any context: the number of audio slots the next packet of stream 'stream'
 * carries; latched until that packet is queued; 0 for a stream that is not
 * running. The size follows the rate: with explicit feedback the last
 * valid feedback value, a 16.16 count of slots per microframe (USB 2.0
 * 5.12.4.2) of length 4, its top 4 bits zero, within +-12.5 % of nominal;
 * until the first, and without explicit feedback, the nominal rate
 * sampling_frequency / 8000, exactly. From the stream's first packet on,
 * every microframe on the bus adds the rate to an accumulator: each
 * packet's own, and each LATE or NO_BUFFER one; drained microframes add
 * nothing. A packet takes INT of the accumulator, at most INT(rate) + 1
 * slots (Formats 2.3.1.1), so a hole's debt is repaid by packets of
 * INT(rate) + 1. The debt stays within U2PS_HOST_DEBT_MAX microframes'
 * worth: the whole slots beyond it are dropped and counted in
 * slots_forgiven. A packet of more slots than max_packet_size carries is
 * cut to fit; the cut slots are dropped and counted in slots_clamped.
 * A known limit of the DWC2 host port: a run of 8,192 microframes
 * (1.024 s) or more with the queue dry counts modulo 16,384 microframes,
 * and as none when that remainder is 8,192 or more, so its NO_BUFFER
 * microframes add less, or nothing, to the accumulator (u2ps_hport.h,
 * u2ps_host_on_missed()). */
uint16_t u2ps_host_slots_next(uint32_t stream);

/* In an app callback, or from thread: queues the next packet of stream
 * 'stream', exactly u2ps_host_slots_next() audio slots of nr_channels *
 * subslot_size bytes at data. The packet lies in DMA memory, starts on
 * U2PS_HPORT_ALIGN (u2ps_hport_cfg.h) and stays untouched until its
 * u2ps_app_host_on_sent(); a contiguous stream stays aligned only if every
 * packet's length is a multiple of U2PS_HPORT_ALIGN. Returns false, and
 * counts in queue_errors, for one of three causes, which the app tells
 * apart itself: a stream that is not running (u2ps_host_slots_next()
 * returns 0: queue only between on_stream_start and on_stream_stop, with
 * their number); a full queue (U2PS_HOST_QUEUE_MAX packets not yet
 * reported by on_sent: wait for the next on_sent); or data NULL or not on
 * U2PS_HPORT_ALIGN (place the packet on it). */
bool u2ps_host_queue(uint32_t stream, const uint8_t *data);

/* Any context: the app stops queueing on purpose. The queued packets go
 * out; the empty microframes until the next u2ps_host_queue() are drained
 * microframes, counted in drained, not holes, and the stream's rate does
 * not accumulate in them. If the queue had already run empty before the
 * drain, that run of empty microframes is drained too: the port reports
 * it as one. On the DWC2 host port a run of 8,192 microframes (1.024 s)
 * or more counts modulo 16,384 microframes, and as none when that
 * remainder is 8,192 or more (u2ps_hport.h, u2ps_host_on_missed()). */
void u2ps_host_drain(uint32_t stream);

/* App, thread: stops stream 'stream', the number u2ps_host_stream_start()
 * returned and on_stream_start gives. A stream that runs: no packet may be
 * queued from now on, the port closes (packets not yet reported sent
 * return at its end), then SET_INTERFACE 0, then
 * u2ps_app_host_on_stream_stop(U2PS_HOST_STOP_APP). A start in progress is
 * cancelled: the request in flight completes, no further request of the
 * start follows, the port's stream never opens and no on_stream_start
 * comes; if that request was the start's SET_INTERFACE and the device
 * accepted it, SET_INTERFACE 0 follows; then
 * u2ps_app_host_on_stream_stop(U2PS_HOST_STOP_APP), exactly once. A
 * cancelled start's GET CUR and Clock Validity reads that completed still
 * update the device's sampling_frequency and clock_validity; a cancel
 * right after SET CUR leaves sampling_frequency at the last value read.
 * If the request in flight fails with a transaction error or a timeout,
 * the session ends as without the stop (U2PS_HOST_STOP_END). Nothing for
 * another number, and nothing more for a stream already stopping. */
void u2ps_host_stream_stop(uint32_t stream);

/* App, thread: a copy of the counters, taken with the vectors masked */
void u2ps_host_stats(u2ps_host_stats_t *stats);

/* ---- App callbacks: the app defines them ---------------------------------- */

/* The device is configured and its audio function can play */
void u2ps_app_host_on_device(const u2ps_host_device_t *dev);

/* The device is the test device (VID 0x1A0A) with PID 'id_product' and
 * bcdDevice 'bcd_device', configured; the test follows (The test device,
 * above). For a product that supports sessions, bcd_device bit 0 set with
 * PID 0x0200 means otg_vbus_off: after a disconnect from this session the
 * app turns VBUS off within 5 s (TTST_VBOFF, OTG and EH Compliance Plan
 * 6.7.5.1.1). An app that logs it says "USB test mode" for 0x0101 to
 * 0x0104. */
void u2ps_app_host_on_test_device(uint16_t id_product, uint16_t bcd_device);

/* The port is out of suspend and the device has answered GET_STATUS:
 * streams may start again. remote_wakeup: the device's remote wakeup
 * began the resume, not u2ps_host_resume(). */
void u2ps_app_host_on_resumed(bool remote_wakeup);

/* The session has ended, for reason 'why' (u2ps_host_end_t); with
 * U2PS_HOST_END_NOT_PLAYABLE, 'refusal' tells why the device had nothing to
 * play, otherwise it is U2PS_HOST_REFUSAL_NONE */
void u2ps_app_host_on_end(u2ps_host_end_t why, u2ps_host_refusal_t refusal);

/* The stream has started: the app queues its packets from now on */
void u2ps_app_host_on_stream_start(const u2ps_host_stream_info_t *info);

/* The oldest queued packet has gone out in microframe 'uframe'; its memory
 * returns */
void u2ps_app_host_on_sent(u2ps_uframe_t uframe);

/* A hole: 'uframes' microframes without a packet for reason 'why', LATE or
 * NO_BUFFER, or a CORRUPTED packet; immediately before the on_sent that
 * ends it */
void u2ps_app_host_on_hole(uint16_t uframes, u2ps_miss_t why);

/* The stream has stopped, or its start failed, for reason 'why'; every
 * queued packet's memory is the app's again. With U2PS_HOST_STOP_REFUSED,
 * 'request' tells which request of the start the device refused
 * (u2ps_host_stop_request_t, with its fix); otherwise it is
 * U2PS_HOST_STOP_REQUEST_NONE. */
void u2ps_app_host_on_stream_stop(u2ps_host_stop_t why, u2ps_host_stop_request_t request);

#if U2PS_CONTRACT_CHECKS
/* Optional: declared, called and needed only in a build with
 * U2PS_CONTRACT_CHECKS=1 (the device library declares the same). The app
 * has broken rule 'violation' of this header (u2ps_contract_t, in
 * u2ps_types.h, names each rule and its fix); called once per occurrence,
 * after which the stack goes on as a build without the checks. It runs
 * where the stack found the violation: in the port's interrupt, inside a
 * callback, or in the app's thread, possibly with the vectors masked. So
 * it stays short and calls nothing of the stack: it counts, logs to memory
 * or stops at a breakpoint. While integrating, build with the checks and
 * treat any call as a bug of the app. */
void u2ps_app_on_contract(u2ps_contract_t violation);
#endif

#endif /* U2PS_HOST_H */
