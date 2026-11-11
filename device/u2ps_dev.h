/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dev.h: the app API of the device stack, a UAC2 sink. Unstable: it
 * may change in any release before 1.0.0.
 *
 * The app describes its device and audio function once
 * (u2ps_dev_description_t). The stack builds every descriptor from that
 * description (device, both configurations, device_qualifier,
 * other_speed_configuration, strings), answers the standard requests and
 * the clock's requests, opens a stream when the host selects a streaming
 * alternate setting and closes it at alternate setting 0, and hands the app
 * each packet where the port put it.
 *
 * Context: every app callback (u2ps_app_*) runs in the port's interrupt
 * and must fit its interrupt budget, with the exceptions below, all in
 * thread with the port's vectors masked, so that no callback ever runs
 * while another does. A request that needs more time is answered later
 * from thread with u2ps_dev_reply(); EP0 NAKs meanwhile. The host allows
 * 50 ms for a request without data and 500 ms for the first data packet
 * (USB 2.0 9.2.6.4, 9.2.6.5).
 *
 * The app thread: u2ps_dev_init(), u2ps_dev_connect(), u2ps_dev_vbus(),
 * u2ps_dev_reply(), u2ps_dev_tick(), u2ps_dev_stream_open(),
 * u2ps_dev_stats() and u2ps_dev_release() outside a callback all run in
 * one thread context, never from an interrupt and never from each other:
 * each masks the port's vectors, and the mask does not nest.
 *
 * Callbacks that also run in thread, with the vectors masked:
 *   callback                 in thread, called from
 *   on_stream_stop           u2ps_dev_connect(false), u2ps_dev_init() (a
 *                            re-init), u2ps_dev_vbus() (a loss),
 *                            u2ps_dev_stream_open() (the port refused)
 *   on_configured(false)     u2ps_dev_connect(false), u2ps_dev_init(),
 *                            u2ps_dev_vbus() (a loss)
 *   on_hole, on_pause        u2ps_dev_connect(false), u2ps_dev_init(),
 *                            u2ps_dev_vbus() (a loss): the stream's tail,
 *                            before its on_stream_stop; u2ps_dev_tick()
 *                            (a long gap: the holes held back,
 *                            u2ps_app_on_hole())
 *   on_vbus                  u2ps_dev_vbus() (unmasked before the first
 *                            successful u2ps_dev_init())
 *   on_fault(OPEN)           u2ps_dev_stream_open()
 *   on_control (hook)        u2ps_dev_reply(), u2ps_dev_stream_open(),
 *                            u2ps_dev_tick() (the start bound)
 * Every other callback runs only in the port's interrupt. Inside any
 * callback, in the interrupt or in thread, the app may call
 * u2ps_dev_speed(), u2ps_dev_vbus_present(), u2ps_dev_fb_set() and
 * u2ps_dev_release() (which then does not mask); it must not call
 * u2ps_dev_init(), u2ps_dev_connect(), u2ps_dev_vbus(), u2ps_dev_reply(),
 * u2ps_dev_tick(), u2ps_dev_stream_open() or u2ps_dev_stats(): it defers
 * them to its thread.
 *
 * Order of the callbacks when a configuration ends at a bus reset, a
 * detach, a VBUS loss or a re-init: the holes of the stream's tail
 * (on_hole, on_pause; u2ps_app_on_hole()), on_stream_stop (if a stream had
 * started or its start was pending), then on_configured(false, why), then,
 * at a bus reset, on_reset, and at a VBUS loss on_vbus(false). At
 * SET_CONFIGURATION the stream's close is asynchronous (u2ps_dport.h):
 * on_configured(false) comes at once, and on_stream_stop when the close
 * has ended; a start still pending stops at once, before
 * on_configured(false).
 *
 * Stream start. When the host selects a streaming alternate setting
 * (SET_INTERFACE), the stack holds that request's status stage, EP0
 * NAKing as for a deferred reply, until the stream is open, so that the
 * device has finished the request before its status stage completes (USB
 * 2.0 9.2.6): at once if on_stream_start returns U2PS_STREAM_OPEN, after
 * the port's open in the same interrupt; at on_closed if the last stream's
 * close is still pending; at u2ps_dev_stream_open() if the app defers.
 * Alternate setting 0 is answered at once, whatever the close does. A
 * refused start stalls SET_INTERFACE, and the interface keeps alternate
 * setting 0 (UAC2 5.2.6.1.2). The hold is bounded: U2PS_START_MAX
 * milliseconds after the first u2ps_dev_tick() since the SETUP the status
 * completes anyway, the start stays pending and tick returns
 * U2PS_FAULT_START. USB 2.0 9.2.6.4 allows 50 ms for the request, so the
 * app calls u2ps_dev_tick() at least every 10 ms, the one tick period of
 * the stack: the limit rests on the bound plus two tick periods.
 * - An app that can lend its memory at once but is not ready to consume
 *   returns U2PS_STREAM_OPEN and releases nothing until it is ready: each
 *   packet the host sends meanwhile is one NO_BUFFER microframe;
 *   microframes without one stay NO_PACKET (a pause if long), and EP0 is
 *   not held.
 * - Linux selects each streaming alternate setting in turn while it
 *   enumerates the device, so the app sees one start and stop per
 *   alternate setting, and with U2PS_STREAM_DEFER each of them holds EP0
 *   for the app's delay.
 *
 * VBUS (U2PS_VBUS, the VBUS mode). The pull-up's supply must come from
 * VBUS or be controlled by it (USB 2.0 7.1.5.1), and when VBUS goes, the
 * device must remove it within 10 s (7.2.1). A device is not in the
 * Powered state without VBUS (9.1.1.2; Figure 9-1, "Power
 * Interruption"), so a VBUS loss ends its session as a detach does. The
 * stack puts the pull-up on only while the app allows the attach
 * (u2ps_dev_connect(true)) and VBUS is present, and takes it off when
 * either ends:
 * - Mode bus (U2PS_VBUS_BUS): a bus-powered device, whose pull-up takes its
 *   supply from VBUS. VBUS is present whenever the device runs, and
 *   u2ps_dev_init() refuses a self_powered description.
 * - Mode app (U2PS_VBUS_APP): the app reads VBUS and reports its level
 *   with u2ps_dev_vbus() on every poll; the debounce is the app's.
 * - Mode port (U2PS_VBUS_PORT), VBUS sensed by the port: not available
 *   yet.
 * A bus-powered device (mode bus, or mode app with a bus-powered
 * description) must signal attach within 100 ms of VBUS (TSIGATT, 7.1.7.3),
 * so it calls u2ps_dev_connect(true), and in mode app reports VBUS, within
 * 100 ms of power-up. Its other duties as a bus-powered device are the
 * app's: at most one unit load (100 mA) until configured (7.2.1), and in
 * suspend at most the suspend current, 0.5 mA averaged over 1 s, since the
 * stack never enables remote wakeup (7.2.3).
 *
 * Full speed: by default the device enumerates at full speed with the minimal
 * FS configuration (the audio function with its AudioControl interface
 * only, so nothing can stream), and the device_qualifier and
 * other_speed_configuration descriptors describe the other speed. Built with
 * U2PS_REFUSE_FS=1, it stalls every GET_DESCRIPTOR at full speed, so it
 * never enumerates there, and stalls device_qualifier and
 * other_speed_configuration at high speed.
 *
 * References: USB 2.0 Specification, Revision 2.0; UAC2 is the USB Device
 * Class Definition for Audio Devices, Release 2.0; Formats is its Audio Data
 * Formats, Release 2.0.
 */
#ifndef U2PS_DEV_H
#define U2PS_DEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "u2ps_types.h"

/* Build constants. The device library's CMake options set them as public
 * compile definitions, so the library and the app see the same values:
 * Each numeric one stops the build outside its range, with a message that
 * names it and the range.
 * - U2PS_EP0_BUF (512): bytes of the control buffer, which every EP0
 *   data stage uses; a multiple of 32, 64 to 65504 (wLength is 16 bits,
 *   USB 2.0 9.3.5). Every descriptor the
 *   stack builds must fit it. It also reaches the app in
 *   u2ps_request_t.size.
 * - U2PS_VBUS (no default): the VBUS mode (above), U2PS_VBUS_APP or
 *   U2PS_VBUS_BUS (CMake option U2PS_VBUS, app or bus).
 * - U2PS_REFUSE_FS (0): 1 refuses full speed (above).
 * - U2PS_CONTROL_HOOK (0): 1 calls u2ps_app_on_control() for every
 *   SETUP the stack answers (CMake option U2PS_CONTROL_HOOK).
 * - U2PS_GAP_PAUSE (400; 1 to 8191): more microframes than this without a packet,
 *   in which the host sent nothing (NO_PACKET), are a pause, not a hole
 *   (50 ms). Holes of the other reasons are holes however long. The
 *   microframe distance between packets is taken modulo 2^14 as a signed
 *   number (u2ps_uframe_dist), so a gap of 1.024 s or longer cannot be
 *   told from a step back, and the port's reports decide.
 * - U2PS_CLOSE_MAX (10; 1 to 65535): milliseconds a stream's close may take before
 *   u2ps_dev_tick() returns U2PS_FAULT_CLOSE.
 * - U2PS_START_MAX (30; 1 to 65535): milliseconds SET_INTERFACE's status stage is held
 *   for a stream start before it completes anyway and u2ps_dev_tick()
 *   returns U2PS_FAULT_START (Stream start, above).
 * - U2PS_ISO_OUT_MPS_MAX (1024; 1 to 1024): the largest wMaxPacketSize of a stream;
 *   u2ps_dev_init() refuses a description that needs more. The port sizes
 *   its receive memory from it.
 * - U2PS_ISO_OUT_COUNT_MAX (16; the port's U2PS_DPORT_ISO_OUT_MIN, 2 on
 *   DWC2 and 3 on ChipIdea, to 65535): the most buffers a stream may have; a
 *   stream lent more does not open (U2PS_FAULT_OPEN). The port sizes its
 *   buffer queue from it. The app holds at most count -
 *   U2PS_DPORT_ISO_OUT_RESERVE of them (u2ps_stream_cfg_t).
 * - U2PS_CONTRACT_CHECKS (0): 1 checks at run time the rules of this
 *   header that the stack can see, and reports each violation through
 *   u2ps_app_on_contract() (CMake option U2PS_CONTRACT_CHECKS, which sets
 *   the host library alike). For integration and tests; without it the
 *   checks compile to nothing. */
#if !defined(U2PS_EP0_BUF) || !defined(U2PS_REFUSE_FS) || !defined(U2PS_GAP_PAUSE) || \
    !defined(U2PS_CLOSE_MAX) || !defined(U2PS_START_MAX) || !defined(U2PS_ISO_OUT_MPS_MAX) || \
    !defined(U2PS_ISO_OUT_COUNT_MAX) || !defined(U2PS_CONTROL_HOOK) || !defined(U2PS_VBUS) || \
    !defined(U2PS_CONTRACT_CHECKS)
#error "u2ps_dev.h: the device library's build constants (U2PS_VBUS, U2PS_EP0_BUF and the others listed above) are missing: link the CMake target u2ps_device, whose public compile definitions set them, rather than only adding device/ to the include path"
#endif

/* The VBUS modes, the values of U2PS_VBUS */
#define U2PS_VBUS_PORT 1 /* the port senses VBUS: not available yet */
#define U2PS_VBUS_APP  2 /* the app reports VBUS with u2ps_dev_vbus() */
#define U2PS_VBUS_BUS  3 /* bus-powered: VBUS is present whenever the device runs */
#if U2PS_VBUS != U2PS_VBUS_APP && U2PS_VBUS != U2PS_VBUS_BUS
#error "u2ps_dev.h: U2PS_VBUS must be U2PS_VBUS_APP or U2PS_VBUS_BUS (no mode port yet): set the CMake option -DU2PS_VBUS=app or -DU2PS_VBUS=bus"
#endif

/* ---- The description ------------------------------------------------------- */

/* The format of a streaming alternate setting, u2ps_format_t, and its
 * Audio Data Format, u2ps_audio_data_format_t, are in u2ps_types.h: the
 * host stack shares them. */

/* The device and its audio function, a UAC2 sink: an AudioControl
 * interface (0) with a programmable internal clock, an Input Terminal (the
 * USB OUT endpoint) and an Output Terminal; and an AudioStreaming interface
 * (1) whose alternate settings 1..format_count each carry one format, with
 * an asynchronous isochronous OUT endpoint (EP1 OUT, every microframe) and
 * its explicit feedback endpoint (EP1 IN, every 8 microframes). The
 * description and everything it points to must stay valid and unchanged
 * after u2ps_dev_init(). */
typedef struct {
  uint16_t id_vendor;  /* idVendor */
  uint16_t id_product; /* idProduct */
  uint16_t bcd_device; /* bcdDevice */
  /* String descriptors 1, 2 and 3 (USB 2.0 9.6.7) in LANGID 0x0409, as
   * printable ASCII (0x20..0x7E), at most 126 characters; NULL for none.
   * With all three NULL the device has no strings. */
  const char *manufacturer;
  const char *product;
  const char *serial_number; /* iSerialNumber's string */
  bool self_powered;     /* bmAttributes D6, and what GET_STATUS reports */
  /* bMaxPower, 0..500 mA. bMaxPower counts 2 mA units, so an odd value is
   * rounded up to the next 2 mA (101 mA gives 51 units, 102 mA). */
  uint16_t max_power_ma;
  uint8_t category;      /* bCategory (UAC2 A.7), e.g. 0x08 I/O box */
  /* wTerminalType of the Output Terminal (USB Device Class Definition for
   * Terminal Types 2.0), e.g. 0x0301 speaker, 0x0602 digital audio
   * interface; not a USB terminal type nor one its tables mark input-only
   * (0x02xx, 0x0703, 0x0708..0x0710) */
  uint16_t terminal_type;
  uint8_t nr_channels;     /* bNrChannels, at least 1 */
  uint32_t channel_config; /* bmChannelConfig (UAC2 4.1), e.g. 0x3 front left and right */
  const u2ps_format_t *formats; /* alternate settings 1..format_count */
  uint8_t format_count;         /* at least 1 */
  /* The clock's sampling frequencies (UAC2 5.2.5.1.1) in Hz, strictly
   * ascending. The highest sizes every data endpoint: wMaxPacketSize =
   * (ceil(sampling frequency / 8000) + 1) * nr_channels * subslot_size,
   * at most 1024 (USB 2.0 5.6.3). */
  const uint32_t *sampling_frequencies;
  uint8_t sampling_frequency_count; /* at least 1 */
  /* The clock's sampling frequency from u2ps_dev_init() until the host
   * sets one, and what GET CUR returns meanwhile: one of
   * sampling_frequencies[], or 0 for sampling_frequencies[0]. A bus reset
   * keeps the one the host set; a re-init keeps it too if the new
   * description lists it (u2ps_dev_init). */
  uint32_t initial_sampling_frequency;
} u2ps_dev_description_t;

/* Which rule of u2ps_dev_description_t a description breaks, after
 * u2ps_dev_init() returned U2PS_DEV_ERR_DESCRIPTION
 * (u2ps_dev_description_error). The stack checks the rules in this order,
 * the format rules for one format after another, and names the first one
 * broken. Each value names its field and the fix. */
typedef enum {
  U2PS_DESCRIPTION_OK = 0,    /* no rule broken */
  U2PS_DESCRIPTION_NULL,      /* desc is NULL: pass the description */
  U2PS_DESCRIPTION_FORMATS,   /* formats is NULL: point it at the formats */
  U2PS_DESCRIPTION_FORMAT_COUNT, /* format_count is 0: give at least one format */
  U2PS_DESCRIPTION_SAMPLING_FREQUENCIES, /* sampling_frequencies is NULL: point it at the rates */
  /* sampling_frequency_count is 0, or more than (U2PS_EP0_BUF - 2) / 12
   * (42 at the default 512), so the RANGE reply does not fit the control
   * buffer: list 1 to that many rates, or build with a larger
   * -DU2PS_EP0_BUF */
  U2PS_DESCRIPTION_SAMPLING_FREQUENCY_COUNT,
  U2PS_DESCRIPTION_NR_CHANNELS,  /* nr_channels is 0: set at least 1 */
  U2PS_DESCRIPTION_MAX_POWER_MA, /* max_power_ma is above 500: set 0..500 */
  /* self_powered is true in VBUS mode bus (U2PS_VBUS_BUS), which is for a
   * bus-powered device only: set it false, or build with -DU2PS_VBUS=app */
  U2PS_DESCRIPTION_SELF_POWERED,
  /* The string is not printable ASCII (0x20..0x7E), or longer than 126
   * characters, or its descriptor, 2 + 2 * characters bytes, exceeds
   * U2PS_EP0_BUF: shorten it or replace the other characters */
  U2PS_DESCRIPTION_MANUFACTURER,
  U2PS_DESCRIPTION_PRODUCT,
  U2PS_DESCRIPTION_SERIAL_NUMBER,
  /* channel_config has more bits set than nr_channels, or one of D27..D31
   * (reserved, raw data): set one bit per channel's spatial location (UAC2
   * 4.1), or 0 */
  U2PS_DESCRIPTION_CHANNEL_CONFIG,
  /* terminal_type is a USB terminal type (0x01xx) or one the Terminal Types
   * tables mark input-only (0x02xx, 0x0703, 0x0708..0x0710): set an output
   * type, e.g. 0x0301 speaker or 0x0602 digital audio interface */
  U2PS_DESCRIPTION_TERMINAL_TYPE,
  /* sampling_frequencies[] holds 0 or is not strictly ascending: list
   * each rate once, above 0, in ascending order */
  U2PS_DESCRIPTION_SAMPLING_FREQUENCY_ORDER,
  /* initial_sampling_frequency is neither 0 nor one of
   * sampling_frequencies[]: set one of them, or 0 for the first */
  U2PS_DESCRIPTION_INITIAL_SAMPLING_FREQUENCY,
  /* A format's subslot_size is outside 1..4: set 1, 2, 3 or 4 bytes */
  U2PS_DESCRIPTION_SUBSLOT_SIZE,
  /* A format's bit_resolution is outside 1..8 * subslot_size: set at
   * most 8 bits per subslot byte */
  U2PS_DESCRIPTION_BIT_RESOLUTION,
  /* A format's audio_data_format is neither PCM nor IEEE_FLOAT, or
   * IEEE_FLOAT without subslot_size 4 and bit_resolution 32: use PCM, or
   * IEEE_FLOAT as 32 bits in 4 bytes */
  U2PS_DESCRIPTION_AUDIO_DATA_FORMAT,
  /* A format's wMaxPacketSize, (ceil(highest sampling frequency / 8000) +
   * 1) * nr_channels * subslot_size, exceeds U2PS_ISO_OUT_MPS_MAX: drop
   * the highest rates, use fewer channels or smaller subslots, or build
   * with a larger -DU2PS_ISO_OUT_MPS_MAX (at most 1024) */
  U2PS_DESCRIPTION_MAX_PACKET_SIZE,
  /* The high-speed configuration descriptor exceeds U2PS_EP0_BUF (8
   * formats fit the default 512): give fewer formats, or build with a
   * larger -DU2PS_EP0_BUF */
  U2PS_DESCRIPTION_CONFIGURATION_SIZE
} u2ps_dev_description_error_t;

/* ---- The configuration ------------------------------------------------------ */

/* Why the device left the Configured state (u2ps_app_on_configured) */
typedef enum {
  U2PS_CONFIG_END_NONE = 0,          /* none: the device entered it */
  /* A bus reset (USB 2.0 9.1.1.3): the host re-enumerates; a host also
   * resets a device that answered a request too late or not at all */
  U2PS_CONFIG_END_BUS_RESET,
  U2PS_CONFIG_END_SET_CONFIGURATION, /* SET_CONFIGURATION 0 (USB 2.0 9.4.7): the host's choice */
  U2PS_CONFIG_END_DETACH,            /* the app's u2ps_dev_connect(false) */
  /* VBUS loss (u2ps_dev_vbus): the cable was pulled or the host cut VBUS;
   * if neither, check the app's VBUS reading and its debounce */
  U2PS_CONFIG_END_VBUS,
  U2PS_CONFIG_END_REINIT             /* the app's u2ps_dev_init() while attached */
} u2ps_config_end_t;

/* ---- Control requests ------------------------------------------------------ */

/* A SETUP packet (USB 2.0 9.3, Table 9-2) */
typedef struct {
  uint8_t bmRequestType;
  uint8_t bRequest;
  uint16_t wValue;
  uint16_t wIndex;
  uint16_t wLength;
} u2ps_setup_t;

/* A vendor request to an interface of the current configuration */
typedef struct {
  u2ps_setup_t setup;
  /* The control buffer, 'size' bytes, lent for the callback only: once the
   * callback returns, it belongs to the stack again, also when the callback
   * defers. Device-to-host: the callback may write its answer here.
   * Host-to-device: it holds the received data. */
  uint8_t *buf;
  uint16_t size; /* bytes of buf (U2PS_EP0_BUF of the library) */
  uint16_t len;  /* host-to-device: bytes received, always wLength (a shorter
                    data stage is a request error the app never sees,
                    USB 2.0 9.3.5); otherwise 0 */
  uint32_t id;   /* names the request in u2ps_dev_reply() */
} u2ps_request_t;

/* Results of u2ps_app_on_request(), u2ps_app_on_sampling_frequency() and
 * u2ps_dev_reply(), and what u2ps_app_on_control() reports. A result
 * >= 0 is success: for a device-to-host request, the number of bytes to
 * send (the stack sends at most wLength); otherwise ignored. */
#define U2PS_REQ_STALL (-1) /* request error */
#define U2PS_REQ_DEFER (-2) /* answer later with u2ps_dev_reply() */

/* ---- The stream ------------------------------------------------------------ */

/* What the host selected, when a stream starts */
typedef struct {
  uint32_t stream;      /* the stream's number, counting from 1; never 0 */
  uint8_t alternate_setting;   /* bAlternateSetting, 1..format_count */
  u2ps_format_t format;        /* its format */
  /* The clock's, in Hz, at on_stream_start. If the host changes it while
   * a deferred start is pending, the app learns the new one from
   * u2ps_app_on_sampling_frequency(), and the stream runs at it
   * (u2ps_dev_stream_open). */
  uint32_t sampling_frequency;
  uint16_t max_packet_size;    /* wMaxPacketSize: no packet is longer */
} u2ps_stream_info_t;

/* DMA memory. The stack's control buffer and the port's own memory (port
 * memory) lie in section .bss.u2ps_dma (U2PS_DMA_SECTION, u2ps_dport.h),
 * which the app's linker script places in RAM the port's DMA reaches,
 * ahead of its *(.bss .bss.*) rule (without a rule of its own, that rule
 * takes it). Class memory is the app's own (u2ps_stream_cfg_t).
 * - u2ps_dev_init() checks every byte of the section's memory against the
 *   port's rule before anything else, at every init, and returns
 *   U2PS_DEV_ERR_DMA_MEMORY if one lies elsewhere. On DWC2 the rule is
 *   u2ps_dwc2_dma_ok() (port/dwc2/common/u2ps_dwc2.h): STM32U5 SRAM1,
 *   SRAM2, SRAM3, SRAM5 (SRAM6 on the U5Fx/5Gx) from 0x2000_0000, not
 *   SRAM4 nor the backup SRAM; STM32F723 DTCM, SRAM1, SRAM2, not ITCM RAM.
 *   The ChipIdea port, without register code yet, names no rule: the
 *   i.MX RT1060 manual does not state which memories its USB DMA reaches.
 * - On the STM32U5 the rule also reads each SRAM's clock enable
 *   (RCC_AHB1ENR.SRAM1EN, RCC_AHB2ENR1.SRAM2EN and SRAM3EN,
 *   RCC_AHB2ENR2.SRAM5EN and SRAM6EN) and power-down bit (PWR_CR1.SRAMxPD):
 *   every SRAM is clocked and powered at reset, and must stay so while u2ps
 *   runs; on the STM32F723 RCC_AHB1ENR.DTCMRAMEN for DTCM. The port applies
 *   the same rule to class memory at each stream's open.
 * - The stack and the port zero their memory in the section at init, so it
 *   may be NOLOAD and the startup code need not zero it.
 * - A linker script may also check the placement at link time, e.g. for
 *   the STM32U5A5: ASSERT(ADDR(.u2ps_dma) >= 0x20000000 &&
 *   ADDR(.u2ps_dma) + SIZEOF(.u2ps_dma) <= 0x20270000, "...") for an output
 *   section .u2ps_dma that holds *(.bss.u2ps_dma). */

/* What the app gives a stream when it starts: class memory, 'count'
 * buffers of 'stride' bytes each, in DMA memory, and the first feedback
 * value. The port refuses the memory (U2PS_FAULT_OPEN) unless all of
 * these hold, with the constants of the port's u2ps_dport_cfg.h:
 * - mem is not NULL and lies on U2PS_DPORT_ALIGN (32 on the STM32F723 and
 *   the i.MX RT1062, 4 on the STM32U5): declare it
 *   _Alignas(U2PS_DPORT_ALIGN);
 * - stride >= U2PS_DPORT_STRIDE(max_packet_size), and stride is a multiple
 *   of U2PS_DPORT_ALIGN; extra bytes per buffer (a canary, a header) keep
 *   the stride a multiple of it: round the sum up to U2PS_DPORT_ALIGN;
 * - U2PS_DPORT_ISO_OUT_MIN <= count <= U2PS_ISO_OUT_COUNT_MAX. The port
 *   keeps U2PS_DPORT_ISO_OUT_RESERVE of the buffers armed (1 on DWC2, 2
 *   on ChipIdea), so the app holds at most count -
 *   U2PS_DPORT_ISO_OUT_RESERVE packets at a time: to hold k, lend k +
 *   U2PS_DPORT_ISO_OUT_RESERVE, that many more than it means to hold. A
 *   packet that comes while the app holds that many is a NO_BUFFER hole
 *   of its own microframe, not delivered;
 * - all count * stride bytes lie in memory the port's DMA reaches: on
 *   DWC2 the memories that u2ps_dwc2_dma_ok() lists
 *   (port/dwc2/common/u2ps_dwc2.h), on the STM32U5 with that SRAM's clock
 *   on and not powered down (DMA memory, above); the ChipIdea port,
 *   without register code yet, names none.
 * fb_value is the first feedback value (16.16 samples per microframe, USB
 * 2.0 5.12.4.2), which the stack fills in with the sampling frequency's
 * nominal value, sampling frequency / 8000 rounded to the nearest. */
typedef struct {
  uint8_t *mem;
  uint16_t stride;
  uint16_t count;
  uint32_t fb_value;
} u2ps_stream_cfg_t;

/* The counters: the port's (u2ps_dport_stats_t) and the stack's own */
typedef struct {
  uint32_t late_completions; /* packets whose re-arm read a later microframe */
  uint32_t late;             /* LATE microframes */
  uint32_t no_buffer;        /* NO_BUFFER microframes: packets that came while the app
                                held count - U2PS_DPORT_ISO_OUT_RESERVE */
  uint32_t corrupted;        /* CORRUPTED packets */
  uint32_t fb_recoveries;    /* feedback re-armed after a missed poll while data flowed
                                (each port's header states its rule) */
  uint32_t errors;           /* port errors: real faults only */
  uint32_t fallback_over;    /* microframes an estimated hole count reported too many
                                (ChipIdea's fallback count; 0 on DWC2) */
  uint32_t release_errors;   /* u2ps_dev_release() calls ignored */
  uint32_t uframe_back;      /* packets not after the last one's microframe */
} u2ps_dev_stats_t;

/* A fault: each value names its cause and what to change */
typedef enum {
  U2PS_FAULT_NONE, /* no fault */
  /* The port refused the class memory of u2ps_stream_cfg_t, and the stream
   * did not start (on_stream_stop follows). A rule of u2ps_stream_cfg_t is
   * broken: count, mem NULL or off U2PS_DPORT_ALIGN, stride below
   * U2PS_DPORT_STRIDE(max_packet_size) or not a multiple of
   * U2PS_DPORT_ALIGN, or memory the port's DMA does not reach. Fix that
   * field; on DWC2, u2ps_dwc2_dcd_open_error() names which. */
  U2PS_FAULT_OPEN,
  /* A stream's close has taken longer than U2PS_CLOSE_MAX, counted from
   * the first tick after it began; the next stream still starts when the
   * close ends. Call u2ps_dev_tick() at least every 10 ms; if it still
   * comes, the port's close is stuck: report it with the port's counters. */
  U2PS_FAULT_CLOSE,
  /* A start held SET_INTERFACE's status stage for U2PS_START_MAX: the app
   * deferred it (U2PS_STREAM_DEFER) and did not call
   * u2ps_dev_stream_open() in time, or the last stream's close was slow
   * (then U2PS_FAULT_CLOSE too). Open sooner, or return U2PS_STREAM_OPEN,
   * and call u2ps_dev_tick() at least every 10 ms. */
  U2PS_FAULT_START
} u2ps_fault_t;

/* What u2ps_app_on_stream_start() decides */
typedef enum {
  U2PS_STREAM_REFUSE, /* no stream: SET_INTERFACE stalls, the interface keeps alternate setting 0 */
  U2PS_STREAM_OPEN,   /* the port opens the stream now, with *cfg */
  U2PS_STREAM_DEFER   /* the app opens it later with u2ps_dev_stream_open() */
} u2ps_stream_start_t;

/* What u2ps_dev_stream_open() did */
typedef enum {
  U2PS_OPEN_DONE, /* the stream is open */
  /* Nothing happened: no start of that number waits for this call. A bus
   * reset, a detach, a VBUS loss, a re-init, SET_CONFIGURATION or a new
   * SET_INTERFACE cancelled it (its on_stream_stop has come), it was never
   * deferred, or the number is another stream's. Pass info->stream of an
   * on_stream_start that returned U2PS_STREAM_DEFER, before its
   * on_stream_stop. */
  U2PS_OPEN_NOT_PENDING,
  /* cfg was NULL: the start is refused, as with U2PS_STREAM_REFUSE; pass
   * the class memory to open it */
  U2PS_OPEN_REFUSED,
  /* The port refused the class memory: on_fault(U2PS_FAULT_OPEN) and
   * on_stream_stop have come; fix the field U2PS_FAULT_OPEN names */
  U2PS_OPEN_FAULT
} u2ps_open_result_t;

/* ---- Calls ------------------------------------------------------------------- */

/* App thread (Context, above): checks and keeps the description, resets
 * the stack and starts the port; the device stays detached until
 * u2ps_dev_connect(true).
 * Returns U2PS_DEV_OK, U2PS_DEV_ERR_DESCRIPTION if the description is
 * invalid (a field out of its range, initial_sampling_frequency not one
 * of sampling_frequencies[], a descriptor larger than U2PS_EP0_BUF, or
 * in VBUS mode bus a self_powered device; then nothing changes; every
 * rule is listed at U2PS_DEV_ERR_DESCRIPTION, u2ps_types.h, and
 * u2ps_dev_description_error() names the one broken),
 * U2PS_DEV_ERR_DMA_MEMORY if the stack's or the port's DMA memory lies
 * where the port's DMA does not reach it (DMA memory, above; checked next,
 * at every init; then nothing changes either), or the port's reason why
 * the controller did not start (u2ps_dev_err_t, each value with its fix).
 * It zeroes the control buffer.
 * Re-init: it may be called again, to change the description, while
 * detached (after u2ps_dev_connect(false), or before the first attach).
 * Called while attached, it detaches first, as u2ps_dev_connect(false)
 * does, with on_configured(false, U2PS_CONFIG_END_REINIT). Once the port
 * has started, a re-init skips the hardware start-up (u2ps_dport_reinit):
 * it reads the new description and resets the stack (the stack's counters
 * in u2ps_dev_stats_t restart, the port's go on). The clock keeps its
 * sampling frequency if the new description lists it, and otherwise
 * takes the new initial one. The VBUS level the stack knows is kept. A
 * stream still open then ends as at a detach. Stream numbers and request
 * ids go on across a re-init, so none the app still names matches a new
 * one. */
u2ps_dev_err_t u2ps_dev_init(const u2ps_dev_description_t *desc);

/* Any context: the rule of u2ps_dev_description_t that the description of
 * the last u2ps_dev_init() broke, if that init returned
 * U2PS_DEV_ERR_DESCRIPTION; otherwise, and before the first init,
 * U2PS_DESCRIPTION_OK. */
u2ps_dev_description_error_t u2ps_dev_description_error(void);

/* App thread, the one of u2ps_dev_init(), reply, tick and (in mode app)
 * vbus (Context, above): allow (true) or end (false) the attach; nothing before a successful
 * u2ps_dev_init(). The pull-up is on while the attach is allowed and VBUS
 * is present: at once in mode bus, and in mode app once u2ps_dev_vbus()
 * reports VBUS present. A bus-powered device calls it
 * within 100 ms of power-up (above, TSIGATT).
 * A detach ends the device's session as a bus reset would: the port stops
 * a stream at once, then, from this thread with the vectors masked,
 * on_stream_stop (if a stream had started) and on_configured(false,
 * U2PS_CONFIG_END_DETACH) (if configured); the device is in the Default
 * state at address 0, and u2ps_dev_speed() returns U2PS_SPEED_NONE.
 * u2ps_dev_connect(false) while detached (a second one, one before any
 * u2ps_dev_connect(true), or one while VBUS is absent) only ends the
 * permission: no callback, no port call. */
void u2ps_dev_connect(bool on);

/* Any context: the speed the last bus reset since the attach ended at;
 * U2PS_SPEED_NONE while detached, after a VBUS loss, and after a bus reset
 * until the port has reported the speed that reset ended at. */
u2ps_speed_t u2ps_dev_speed(void);

#if U2PS_VBUS == U2PS_VBUS_APP
/* VBUS mode app only. App, from the thread that calls reply and tick (never
 * from an interrupt, because the port's mask does not nest): the VBUS level,
 * debounced by the app, which calls it with the level on every poll; the
 * stack acts on changes only. Calls made before the first u2ps_dev_init()
 * count, and a re-init keeps the level. Until the first call VBUS is
 * absent.
 * - A loss: the pull-up goes off as at u2ps_dev_connect(false); then, from
 *   this thread with the vectors masked, on_stream_stop (if a stream had
 *   started, also one whose close was pending: its on_closed never comes),
 *   on_configured(false, U2PS_CONFIG_END_VBUS) (if configured) and
 *   on_vbus(false). u2ps_dev_speed() returns U2PS_SPEED_NONE, a deferred
 *   request is cancelled, a suspended device gets no on_resume, and suspend
 *   is reported again only after the next bus reset. While VBUS is absent
 *   the stack ignores every event the port reports (reset, suspend,
 *   resume, SETUP, EP0 and stream events) and counts none.
 * - A return: the pull-up goes on if the attach is allowed, then on_vbus(true)
 *   only; the host's bus reset follows as at an attach.
 * Fail-safe only as far as this goes: a device whose app never reports VBUS
 * never attaches; but the pull-up follows the last report, so a poll that
 * stalls with VBUS present leaves it on after the cable is pulled. */
void u2ps_dev_vbus(bool present);
#endif

/* Any context: whether VBUS is present as the stack knows it: in mode bus
 * always true; in mode app the last level u2ps_dev_vbus() reported, false
 * before the first report. */
bool u2ps_dev_vbus_present(void);

/* App, thread only (never from an interrupt, because the port's mask does
 * not nest; one thread context for reply, release from thread, tick,
 * stats and, in mode app, vbus): the deferred answer to request 'id'. result is
 * U2PS_REQ_STALL or >= 0; for a device-to-host request, data holds the
 * result bytes in the app's own memory (never the request's buf), which the
 * stack copies into its control buffer in pieces of at most U2PS_EP0_MPS
 * bytes, each with the port's vectors masked and only while the request is
 * still pending; it then arms the stage in a masked region of its own. So
 * no masked region grows with the reply's size. Returns false, and sends
 * nothing, if 'id' is no longer pending, also when a new SETUP, a bus
 * reset, a detach, a VBUS loss or a re-init cancelled it during the copy.
 * A reply the stack accepts can still be dropped by the port if a new
 * SETUP arrived meanwhile (u2ps_dport.h). */
bool u2ps_dev_reply(uint32_t id, const void *data, int result);

/* Inside an app callback, or from the thread that calls reply and tick
 * (the stack then masks the port's vectors): the n oldest packets of stream
 * 'stream' that the app holds go back to the port, in arrival order. Every
 * packet, zero-length ones included, is held until released. A release
 * for a stream that has stopped, or of more packets than the app holds, is
 * ignored and counted in release_errors. */
void u2ps_dev_release(uint32_t stream, uint16_t n);

/* The length on_pause() gives when the stack cannot tell it: the gap was
 * longer than the microframe numbers can count (u2ps_dev_tick()) */
#define U2PS_PAUSE_LONG 0xFFFFu

/* Any context: the feedback value (samples per microframe, 16.16, USB 2.0
 * 5.12.4.2) the stream's feedback endpoint sends from now on, within two
 * polls. A stream starts with u2ps_stream_cfg_t.fb_value, and a change of
 * the sampling frequency sets the new one's nominal value. */
void u2ps_dev_fb_set(uint32_t value);

/* App thread: the time in milliseconds, called at least every 10 ms, the
 * one tick period of the stack (U2PS_START_MAX and U2PS_CLOSE_MAX rest on
 * it). The stack measures a stream's close with it, from the
 * first tick after the close began; it masks the port's vectors briefly.
 * It also measures the time since the stream's last packet: past
 * U2PS_GAP_PAUSE microframes' worth (50 ms), a long gap, the next packet
 * ends a pause, however the microframe numbers wrapped, unless the
 * port's reports of other reasons account for that time (see
 * u2ps_app_on_pause()). At each tick of a long gap the stack reports the
 * holes held back so far (u2ps_app_on_hole()), with the vectors masked.
 * Without tick calls, a gap of 1.024 s or more is
 * decided by the port's hole reports alone, and a gap of k * 2^14
 * microframes plus a few looks like a hole of those few.
 * Returns U2PS_FAULT_CLOSE once per close that has taken longer than
 * U2PS_CLOSE_MAX (the next stream still starts when the close ends), and
 * U2PS_FAULT_START once per start that has held SET_INTERFACE's status
 * stage for longer than U2PS_START_MAX, counted from the first tick after
 * the SETUP: the status then completes, and the start stays pending and
 * may still open. Both for one SETUP come on separate calls. Otherwise
 * U2PS_FAULT_NONE. Without tick calls there is neither bound; to keep
 * USB 2.0 9.2.6.4's 50 ms, tick at least every 10 ms (above). */
u2ps_fault_t u2ps_dev_tick(uint32_t now_ms);

/* App, thread only (the thread of reply and tick; the port's mask does not
 * nest): opens the stream whose start the app deferred (U2PS_STREAM_DEFER),
 * 'stream' naming it as u2ps_stream_info_t.stream did, with class memory
 * *cfg as on_stream_start would have filled it in (cfg NULL refuses the
 * start). With the vectors masked: the port opens the stream, and
 * SET_INTERFACE's held status stage completes (or stalls, if refused); if
 * the port refuses the memory, on_fault(U2PS_FAULT_OPEN) and
 * on_stream_stop come here, in this thread. Returns what happened
 * (u2ps_open_result_t); U2PS_OPEN_NOT_PENDING, changing nothing, if the
 * start has been cancelled meanwhile (a bus reset, a detach, a VBUS loss,
 * a re-init, SET_CONFIGURATION or a new SET_INTERFACE of the streaming
 * interface; each gave on_stream_stop) or was never deferred.
 * The stream runs at the clock's sampling frequency at this call: if the
 * host changed it while the start was pending (possible only after the
 * bound let the status complete), the feedback starts at the new one's
 * nominal value instead of cfg->fb_value, and info.sampling_frequency of
 * on_stream_start no longer holds (on_sampling_frequency told the app).
 * The masked time here is the port's open with its cache maintenance,
 * longer than the few microseconds of the stack's other masked regions
 * (about 20 us, inferred; to be measured on the F723 and the RT1060); it
 * is harmless, as no stream runs while a start is pending. */
u2ps_open_result_t u2ps_dev_stream_open(uint32_t stream, const u2ps_stream_cfg_t *cfg);

/* App, thread: a copy of the counters, taken with the vectors masked */
void u2ps_dev_stats(u2ps_dev_stats_t *stats);

/* ---- App callbacks: the app defines them; port interrupt ------------------ */

/* A vendor request to an interface of the current configuration. Called at
 * the SETUP for device-to-host and no-data requests, and after the data
 * stage for host-to-device requests with data. Returns a result as above,
 * or U2PS_REQ_DEFER; any other negative result is a request error, as
 * U2PS_REQ_STALL (and U2PS_CONTRACT_DEV_CALLBACK_RESULT with the contract
 * checks). (Class requests are the stack's: the clock's, and a request
 * error for every other, since the description has no other Control,
 * UAC2 5.2.2.) */
int u2ps_app_on_request(const u2ps_request_t *req);

/* The host sets the clock's sampling frequency (UAC2 5.2.5.1.1).
 * 'requested' is the value the host sent; 'chosen' is the description's
 * sampling frequency closest to it (UAC2 5.2.2, p. 91: a Control adjusts
 * to the closest valid value; on a tie the lower one). Returns >= 0 to
 * accept, U2PS_REQ_STALL to refuse, or U2PS_REQ_DEFER to answer later
 * with u2ps_dev_reply(id, NULL, result). The chosen one applies once
 * accepted, and GET CUR returns it. */
int u2ps_app_on_sampling_frequency(uint32_t requested, uint32_t chosen, uint32_t id);

/* The configuration: true, with why U2PS_CONFIG_END_NONE, when the device
 * enters the Configured state (SET_CONFIGURATION to 1, called at that
 * request's SETUP, before its status stage); false when it leaves it, with
 * the cause in 'why': SET_CONFIGURATION 0, a bus reset,
 * u2ps_dev_connect(false), a VBUS loss or a re-init (the order with
 * on_stream_stop is at the top of this file). Calls alternate: a
 * SET_CONFIGURATION 1 while configured gives none, although its streams
 * stop (USB 2.0 9.4.7). u2ps_dev_speed() tells the speed here: at full
 * speed the minimal FS configuration has no streaming interface. */
void u2ps_app_on_configured(bool configured, u2ps_config_end_t why);

/* A stream starts: the host selected a streaming alternate setting, and the
 * last stream has stopped (possibly within the same interrupt, so the
 * memory of the last stream may be lent again at once). The port has not
 * yet enabled the stream's endpoints; it does so only once the app has
 * given the memory, so no packet of the stream comes before. Returns:
 * - U2PS_STREAM_OPEN: the app has filled in *cfg (class memory; fb_value,
 *   already nominal, if it wants another), and the port opens the stream
 *   now;
 * - U2PS_STREAM_DEFER: the app opens it later from thread with
 *   u2ps_dev_stream_open(info->stream, cfg); SET_INTERFACE's status stage
 *   waits for it, at most U2PS_START_MAX; if the host changes the
 *   sampling frequency meanwhile, u2ps_app_on_sampling_frequency() tells
 *   the app the new one, at which the stream runs;
 * - U2PS_STREAM_REFUSE: nothing opens, SET_INTERFACE stalls, and no
 *   on_stream_stop follows.
 * Every start not refused gets exactly one on_stream_stop, also one that
 * is still pending when a bus reset, a detach, a VBUS loss, a re-init,
 * SET_CONFIGURATION or a new SET_INTERFACE of the streaming interface
 * cancels it, and one the port refuses (after on_fault). The next stream's
 * start still waits for the last stream's close to end (u2ps_dport.h: the
 * close is asynchronous). A SETUP of another request while the status is
 * held ends the hold, as for a deferred reply, but not the start: the
 * stream may still open, while the host keeps the alternate setting it
 * had (harmless). */
u2ps_stream_start_t u2ps_app_on_stream_start(const u2ps_stream_info_t *info,
                                             u2ps_stream_cfg_t *cfg);

/* A packet of the stream, in arrival order: data lies in class memory and is
 * held until u2ps_dev_release(). A zero-length packet has len 0, and the
 * stream goes on. uframe is the microframe it came in, or
 * U2PS_UFRAME_UNKNOWN. */
void u2ps_app_on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe);

/* A hole: 'uframes' microframes without a packet, for reason 'why'; one
 * call per reason, NO_PACKET first, and a hole longer than 65,535
 * microframes in several calls of 65,535 or fewer. It comes immediately
 * before the packet that ends the hole if the port has
 * U2PS_DPORT_UFRAME_EXACT; otherwise before the first packet after the
 * port's report, which can be a later packet of the same frame. NO_PACKET
 * microframes are those of the microframe gap since the last packet, when
 * both packets have microframe numbers, that no other reason accounts
 * for; otherwise those the port reported. The stream's tail, the holes
 * after its last packet up to the last packet the port received, comes
 * before u2ps_app_on_stream_stop(), with the port's counts (NO_PACKET
 * beyond U2PS_GAP_PAUSE as on_pause); nothing after the last packet the
 * port received is reported. A long gap (no packet for more than
 * U2PS_GAP_PAUSE microframes' worth, u2ps_dev_tick()) does not wait for
 * the next packet: at each tick of it the holes after the last packet up
 * to the last one received, so far, come the same way, in thread with the
 * vectors masked, so that the holes of a hold of every buffer the app may
 * hold reach it before a host pause ends; each is reported once, and the
 * next packet or the tail brings only what came after. */
void u2ps_app_on_hole(uint16_t uframes, u2ps_miss_t why);

/* A pause, in place of a NO_PACKET hole of more than U2PS_GAP_PAUSE
 * microframes: 'uframes' NO_PACKET microframes. Only microframes in which
 * the host sent nothing form a pause. If u2ps_dev_tick() saw no packet for
 * more than 50 ms, the gap is a pause also where the microframe count
 * wrapped, as long as the time the ticks saw exceeds the port's reports
 * of other reasons (LATE, NO_BUFFER, CORRUPTED), and the holes the ticks
 * reported, by more than U2PS_GAP_PAUSE microframes; 'uframes' is the
 * NO_PACKET count, the gap's microframes that no hole accounts for, only
 * where the microframe gap is at least the time the ticks saw, otherwise
 * U2PS_PAUSE_LONG. Holes of other reasons in the same gap still come as
 * holes, and a gap of other reasons alone, such as a hold of every buffer
 * the app may hold while the host sends, is no pause. */
void u2ps_app_on_pause(uint16_t uframes);

/* The stream has stopped, or its pending start has been cancelled: closed
 * at alternate setting 0, at another alternate setting or at
 * SET_CONFIGURATION, or ended by a bus reset, a detach, a VBUS loss or a
 * re-init. The stream's tail holes have come before it
 * (u2ps_app_on_hole()).
 * Class memory is the app's again, and the next stream may start in the
 * same interrupt on the same memory: the app drops every reference to the
 * stopped stream's packets here. */
void u2ps_app_on_stream_stop(void);

/* A fault: only U2PS_FAULT_OPEN (its cause and fix at u2ps_fault_t; on
 * DWC2 u2ps_dwc2_dcd_open_error() names which rule broke), followed by
 * u2ps_app_on_stream_stop(), in the port's interrupt, or for a deferred
 * start in the thread that calls u2ps_dev_stream_open().
 * (U2PS_FAULT_CLOSE and U2PS_FAULT_START are u2ps_dev_tick()'s results.) */
void u2ps_app_on_fault(u2ps_fault_t fault);

/* Bus reset: the device is in the Default state again (USB 2.0 9.1.1.3). A
 * stream has stopped before, and the configuration has ended. */
void u2ps_app_on_reset(void);

/* Suspend and resume (USB 2.0 7.1.7.6). Suspend is reported only once a
 * bus reset has come since the pull-up went on: an idle bus after the
 * pull-up, before the host's first reset, is not a suspend of an
 * enumerated device. A resume comes only after a reported suspend, and
 * none after a detach or a VBUS loss. */
void u2ps_app_on_suspend(void);
void u2ps_app_on_resume(void);

#if U2PS_VBUS != U2PS_VBUS_BUS
/* VBUS mode app (and later port): VBUS has gone (false) or come (true);
 * the order with the other callbacks is at u2ps_dev_vbus(). In mode app it
 * runs in the thread that called u2ps_dev_vbus(), with the port's vectors
 * masked (before the first successful u2ps_dev_init() unmasked, as no
 * vector runs yet). The port's mask does not nest (u2ps_dport.h), so it
 * makes only the calls the table at the top of this file allows inside a
 * callback: u2ps_dev_speed(), u2ps_dev_vbus_present(), u2ps_dev_fb_set()
 * and u2ps_dev_release() (ignored and counted here, as the stream has
 * stopped); never u2ps_dev_connect(), u2ps_dev_init(), u2ps_dev_vbus(),
 * u2ps_dev_reply(), u2ps_dev_tick(), u2ps_dev_stream_open() or
 * u2ps_dev_stats(). */
void u2ps_app_on_vbus(bool present);
#endif

#if U2PS_CONTROL_HOOK
/* Optional: declared, called and needed only in a build with
 * U2PS_CONTROL_HOOK=1; an app built without it (the default) does not
 * define it. Every SETUP the stack answers, standard, class and vendor
 * alike, once the stack has acted on it. 'result' is the length of the
 * data stage sent for a device-to-host request with data (after the limit
 * of wLength), 0 for any other success, or U2PS_REQ_STALL for a request
 * error. Called in the port's interrupt; for a deferred request, a
 * deferred stream start or a start past U2PS_START_MAX in the thread that
 * calls u2ps_dev_reply(), u2ps_dev_stream_open() or u2ps_dev_tick(), with
 * the vectors masked. A request
 * that a new SETUP, a bus reset, a detach, a VBUS loss or a re-init
 * cancels before its answer gets no call. For logging; it changes
 * nothing. */
void u2ps_app_on_control(const u2ps_setup_t *setup, int result);
#endif

#if U2PS_CONTRACT_CHECKS
/* Optional: declared, called and needed only in a build with
 * U2PS_CONTRACT_CHECKS=1 (the host library declares the same). The app has
 * broken rule 'violation' of this header (u2ps_contract_t, in u2ps_types.h,
 * names each rule and its fix); called once per occurrence, after which the
 * stack goes on as a build without the checks. It runs where the stack
 * found the violation: in the port's interrupt, inside a callback, or in
 * the app's thread, possibly with the vectors masked. So it stays short and
 * calls nothing of the stack: it counts, logs to memory or stops at a
 * breakpoint. While integrating, build with the checks and treat any call
 * as a bug of the app. */
void u2ps_app_on_contract(u2ps_contract_t violation);
#endif

#endif /* U2PS_DEV_H */
