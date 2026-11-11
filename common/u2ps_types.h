/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_types.h: the types that the app APIs and the port interfaces of
 * the device stack and the host stack share: microframe numbers, the
 * reasons of a hole, the bus speed, the format of an alternate setting,
 * the reasons a start failed and the contract violations; and the
 * sections of the packet path.
 */
#ifndef U2PS_TYPES_H
#define U2PS_TYPES_H

#include "u2ps_uframe.h"

/* A microframe number the port does not know */
#define U2PS_UFRAME_UNKNOWN 0xFFFFu

/* The packet path's sections, so that a linker script can place them in
 * tightly coupled memory with one rule each. U2PS_FAST_TEXT puts a
 * function in section .text.u2ps_fast: the device port's OUT pass and
 * everything it calls (the device stack's packet upcalls), the feedback
 * endpoint's passes and the release. U2PS_FAST_BSS puts a zero-initialised
 * variable in section .bss.u2ps_fast, which has no load image: the port's
 * and the device stack's state, the injection build's state and pass
 * profile. A script without a rule for them places them with its
 * *(.text .text.*) and *(.bss .bss.*) rules. */
#define U2PS_FAST_TEXT __attribute__((section(".text.u2ps_fast")))
#define U2PS_FAST_BSS  __attribute__((section(".bss.u2ps_fast")))

/* Why microframes of a stream passed without a packet, on either side;
 * each value names its cause and what to change */
typedef enum {
  /* No valid packet: the device port received none where armed, or the
   * host got none to a feedback poll; cause unknown. On the device, the
   * host sent nothing (a pause, if longer than U2PS_GAP_PAUSE) or the
   * packet was lost on the bus. Nothing to change in the app; if it
   * recurs while the host plays, check the cable, hubs and the host. */
  U2PS_MISS_NO_PACKET,
  /* The port armed too late: re-armed after the microframe, or lost its
   * prime; on the host the packet goes out later, a poll is skipped. The
   * USB interrupt ran late: give the port's vector the highest priority
   * that runs code, and keep other handlers and masked regions short. */
  U2PS_MISS_LATE,
  /* No buffer. On the device, the host sent a packet and no buffer was
   * free: the app held count - U2PS_DPORT_ISO_OUT_RESERVE packets (it
   * releases them too late: call u2ps_dev_release() sooner, or lend more
   * buffers, u2ps_stream_cfg_t.count). On the host, the host queue was
   * empty (the app queues too late: queue sooner or keep more packets
   * queued, up to U2PS_HOST_QUEUE_MAX; a gap on purpose is a drain,
   * u2ps_host_drain()) */
  U2PS_MISS_NO_BUFFER,
  /* A packet that did not cross the bus intact: received with a CRC error
   * and dropped, or sent damaged or not fetched. The signal path: cable,
   * connector, hub or board; on the host also the DMA's reach of the
   * packet's memory. */
  U2PS_MISS_CORRUPTED
} u2ps_miss_t;

/* The speed a bus reset ended at. U2PS_SPEED_NONE only from
 * u2ps_dev_speed(): detached, or no reset has ended since the attach.
 * U2PS_SPEED_LOW only on the host side (u2ps_host_on_enabled): a device
 * never runs at low speed. */
typedef enum {
  U2PS_SPEED_NONE = 0,
  U2PS_SPEED_FULL = 1,
  U2PS_SPEED_HIGH = 2,
  U2PS_SPEED_LOW = 3
} u2ps_speed_t;

/* The Type I Audio Data Formats the stacks carry (Formats 2.3.1.7). 0 is
 * PCM, so a format whose designated initializer leaves it out is PCM. */
typedef enum {
  U2PS_AUDIO_DATA_FORMAT_PCM = 0,   /* signed two's-complement, left-justified (2.3.1.7.1) */
  U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT /* IEEE-754 single precision (2.3.1.7.3) */
} u2ps_audio_data_format_t;

/* The format of one streaming alternate setting: Type I (Formats 2.3.1.6,
 * Table 2-2) */
typedef struct {
  uint8_t subslot_size;   /* bSubslotSize: bytes per sample, 1..4 */
  uint8_t bit_resolution; /* bBitResolution: 1..8 * subslot_size */
  /* The Audio Data Format, the bit set in bmFormats (Formats A.2.1).
   * IEEE_FLOAT needs subslot_size 4 and bit_resolution 32 (Formats
   * 2.3.1.7.3). */
  u2ps_audio_data_format_t audio_data_format;
} u2ps_format_t;

/* Why u2ps_dev_init() failed. The device stack refuses a description and
 * DMA memory the port's DMA does not reach (u2ps_dport_dma_check); every
 * other reason is the port's, which maps its own causes onto these
 * (each port's header names its mapping; on DWC2,
 * u2ps_dwc2_dcd_init_error() gives the port's own reason, u2ps_dwc2.h).
 * Each value names its cause and what to change. */
typedef enum {
  U2PS_DEV_OK = 0,
  /* The description breaks a rule of u2ps_dev_description_t (u2ps_dev.h),
   * and nothing changed. Fix the field: desc, formats and
   * sampling_frequencies not NULL; format_count, sampling_frequency_count
   * and nr_channels at least 1; max_power_ma at most 500; self_powered
   * false in VBUS mode bus (U2PS_VBUS_BUS); each string printable ASCII
   * (0x20..0x7E), at most 126 characters, and its descriptor, 2 + 2 *
   * characters bytes, within U2PS_EP0_BUF; channel_config with no more bits
   * set than nr_channels and none of D27..D31; terminal_type an output
   * type (not 0x01xx, 0x02xx, 0x0703 nor 0x0708..0x0710);
   * sampling_frequencies above 0, strictly ascending, at most
   * (U2PS_EP0_BUF - 2) / 12 of them; initial_sampling_frequency 0 or one
   * of them; each format with subslot_size 1..4, bit_resolution
   * 1..8 * subslot_size, audio_data_format PCM, or IEEE_FLOAT with
   * subslot_size 4 and bit_resolution 32; each format's wMaxPacketSize,
   * (ceil(highest sampling frequency / 8000) + 1) * nr_channels *
   * subslot_size, at most U2PS_ISO_OUT_MPS_MAX; and the configuration
   * descriptor within U2PS_EP0_BUF (fewer formats, or a larger
   * -DU2PS_EP0_BUF). u2ps_dev_description_error() (u2ps_dev.h) names the
   * rule broken. */
  U2PS_DEV_ERR_DESCRIPTION,
  /* A clock the controller or its PHY needs is not running: start it
   * before u2ps_dev_init() (on DWC2 the HSE, at the frequency
   * U2PS_DWC2_HSE_HZ names) */
  U2PS_DEV_ERR_CLOCK,
  /* A supply or voltage setting the controller needs is missing: set it
   * before u2ps_dev_init() (on the STM32U5 voltage range 1 or 2, PWR) */
  U2PS_DEV_ERR_POWER,
  /* A security setting keeps the port from the controller: on the STM32U5
   * TrustZone is enabled; the port is nonsecure only, so set option byte
   * TZEN = 0 */
  U2PS_DEV_ERR_SECURITY,
  /* The PHY, its supply or its PLL did not come up: check the board's USB
   * supply and that the PHY's reference clock matches U2PS_DWC2_HSE_HZ */
  U2PS_DEV_ERR_PHY,
  /* The controller's reset did not end: its bus clock or the PHY clock is
   * not running; check the clock set-up before u2ps_dev_init() */
  U2PS_DEV_ERR_RESET,
  /* The controller did not enter device mode: other code drives the
   * controller (a host stack, the ID pin forced to host); leave it to the
   * port */
  U2PS_DEV_ERR_MODE,
  /* FIFO RAM or port memory does not fit, or a FIFO flush timed out: the
   * receive FIFO is sized from U2PS_ISO_OUT_MPS_MAX, so lower it to the
   * largest wMaxPacketSize the description needs */
  U2PS_DEV_ERR_MEMORY,
  /* Another reason of the port: its header names it */
  U2PS_DEV_ERR_PORT,
  /* The device stack's or the port's DMA memory (section .bss.u2ps_dma,
   * U2PS_DMA_SECTION) lies where the port's DMA does not reach it, and
   * nothing changed; the stack checks it before anything else, at every
   * u2ps_dev_init(). Fix: place *(.bss.u2ps_dma) in RAM the port's DMA
   * reaches, ahead of the *(.bss .bss.*) rule (each port's header names
   * that RAM): on the STM32U5 SRAM1, SRAM2, SRAM3 or SRAM5 at
   * 0x2000_0000-0x2026_FFFF, or SRAM6 at 0x2027_0000-0x202E_FFFF on the
   * STM32U5Fx/5Gx, not SRAM4 nor the backup SRAM, and keep that SRAM's
   * clock (RCC_AHB1ENR.SRAM1EN, RCC_AHB2ENR1.SRAM2EN or SRAM3EN,
   * RCC_AHB2ENR2.SRAM5EN or SRAM6EN) and power (PWR_CR1.SRAMxPD = 0) on,
   * as they are at reset; on the STM32F723 DTCM (with
   * RCC_AHB1ENR.DTCMRAMEN on), SRAM1 or SRAM2 at 0x2000_0000-0x2003_FFFF,
   * not ITCM RAM; on the STM32H7S3 AXI SRAM2 at 0x2402_0000-0x2403_FFFF
   * (always mapped), the other AXI SRAMs as the option bytes map them, or
   * AHB SRAM1 or SRAM2 at 0x3000_0000-0x3000_7FFF with its clock on
   * (RCC_AHB2ENR.SRAM1EN, SRAM2EN, off at reset), not DTCM, ITCM nor the
   * backup SRAM. */
  U2PS_DEV_ERR_DMA_MEMORY,
  /* The cycle counter that times the port's waits does not count, and
   * nothing was started (on DWC2 the DWT's CYCCNT, which the port unlocks
   * on a Cortex-M7 and enables). Fix: leave the cycle counter to the port:
   * no code or debugger script may lock the DWT or clear DEMCR.TRCENA or
   * DWT_CTRL.CYCCNTENA; then call u2ps_dev_init() again. */
  U2PS_DEV_ERR_TIMER
} u2ps_dev_err_t;

/* Why u2ps_host_init() failed: DMA memory the port's DMA does not reach
 * (u2ps_hport_dma_check), or the host port's reason, which it maps its
 * own causes onto (each port's header names its mapping). Each value
 * names its cause and what to change. */
typedef enum {
  U2PS_HOST_OK = 0,
  /* A clock the controller or its PHY needs is not running: start it
   * before u2ps_host_init() (on DWC2 the HSE, at U2PS_DWC2_HSE_HZ) */
  U2PS_HOST_ERR_CLOCK,
  /* A supply or voltage setting the controller needs is missing: set it
   * before u2ps_host_init() (on the STM32U5 voltage range 1 or 2) */
  U2PS_HOST_ERR_POWER,
  /* A security setting keeps the port from the controller: on the STM32U5
   * set option byte TZEN = 0 */
  U2PS_HOST_ERR_SECURITY,
  /* The PHY, its supply or its PLL did not come up: check the board's USB
   * supply and that the PHY's reference clock matches U2PS_DWC2_HSE_HZ */
  U2PS_HOST_ERR_PHY,
  /* The controller's reset did not end: its bus clock or the PHY clock is
   * not running; check the clock set-up */
  U2PS_HOST_ERR_RESET,
  /* The controller did not enter host mode: other code drives the
   * controller (a device stack, the ID pin forced to device) */
  U2PS_HOST_ERR_MODE,
  /* FIFO RAM or port memory does not fit, or a FIFO flush timed out: the
   * port's header names its plan and what sizes it */
  U2PS_HOST_ERR_MEMORY,
  /* Another reason of the port: its header names it */
  U2PS_HOST_ERR_PORT,
  /* The host stack's or the port's DMA memory (section .bss.u2ps_dma,
   * U2PS_DMA_SECTION) lies where the port's DMA does not reach it, and
   * nothing changed; the stack checks it before anything else, at every
   * u2ps_host_init(). Fix: place *(.bss.u2ps_dma) as for
   * U2PS_DEV_ERR_DMA_MEMORY: in RAM the port's DMA reaches, with that
   * RAM's clock and power on. */
  U2PS_HOST_ERR_DMA_MEMORY,
  /* The cycle counter that times the port's waits does not count, and
   * nothing was started (on DWC2 the DWT's CYCCNT, which the port unlocks
   * on a Cortex-M7 and enables). Fix: leave the cycle counter to the port:
   * no code or debugger script may lock the DWT or clear DEMCR.TRCENA or
   * DWT_CTRL.CYCCNTENA; then call u2ps_host_init() again. */
  U2PS_HOST_ERR_TIMER
} u2ps_host_err_t;

/* A contract violation: the app broke a rule that u2ps_dev.h or
 * u2ps_host.h states. Only a library built with U2PS_CONTRACT_CHECKS=1
 * looks for them, and reports each occurrence once through
 * u2ps_app_on_contract(); the stack then goes on as a build without the
 * checks does. Each value names the rule broken and the fix. A thread call
 * is one that the context table at the top of the stack's header lists for
 * the app's thread. */
typedef enum {
  U2PS_CONTRACT_NONE = 0, /* no violation; never reported */
  /* Device stack (u2ps_dev.h) */
  /* A thread call (u2ps_dev_init, connect, vbus, reply, tick, stream_open
   * or stats) inside an app callback, in the port's interrupt or in thread
   * with the vectors masked. Its mask may nest, which unmasks the vectors
   * too early. Fix: set a flag in the callback; the app's thread makes the
   * call. */
  U2PS_CONTRACT_DEV_CALL_IN_CALLBACK,
  /* A thread call, or u2ps_dev_release() outside a callback, while another
   * thread call held the port's vectors masked: from an interrupt that
   * preempted it, or from a second thread. The mask does not nest. Fix:
   * make every thread call from one thread, never from an interrupt. */
  U2PS_CONTRACT_DEV_CALL_NESTED,
  /* u2ps_dev_connect(true) before a successful u2ps_dev_init(): it does
   * nothing, and the device never attaches. Fix: connect only after
   * u2ps_dev_init() returned U2PS_DEV_OK. */
  U2PS_CONTRACT_DEV_CONNECT_BEFORE_INIT,
  /* u2ps_dev_tick() came more than 10 ms after the last one while it timed
   * a stream's close (U2PS_CLOSE_MAX) or SET_INTERFACE's held status
   * (U2PS_START_MAX): both bounds, and USB 2.0 9.2.6.4's 50 ms, rest on a
   * tick at least every 10 ms. Fix: tick at least every 10 ms. */
  U2PS_CONTRACT_DEV_TICK_LATE,
  /* The description, or what it points to, changed after u2ps_dev_init()
   * (seen at u2ps_dev_tick()): the stack answers from it, so the host's
   * copy of the descriptors no longer matches. Fix: keep it unchanged
   * (static const); for another, detach and call u2ps_dev_init() again. */
  U2PS_CONTRACT_DEV_DESCRIPTION_CHANGED,
  /* u2ps_dev_release() of more packets than the app holds of the stream:
   * ignored and counted in release_errors. Fix: release each packet once,
   * after its on_packet. */
  U2PS_CONTRACT_DEV_RELEASE_COUNT,
  /* u2ps_dev_release() for a stream number that no on_stream_start gave
   * (0, or a later one), or, inside a callback, for a stream that has
   * stopped: ignored and counted in release_errors. Fix: name the stream
   * by info->stream, and drop its packets at on_stream_stop without
   * releasing them. (A release from thread that meets a stop the interrupt
   * has just made is not reported: the app cannot avoid it.) */
  U2PS_CONTRACT_DEV_RELEASE_STREAM,
  /* u2ps_dev_reply() with a result below 0 other than U2PS_REQ_STALL (for
   * example U2PS_REQ_DEFER): taken as a stall. Fix: U2PS_REQ_STALL, or the
   * result >= 0. */
  U2PS_CONTRACT_DEV_REPLY_RESULT,
  /* u2ps_dev_reply() of a device-to-host request with data to send but
   * data NULL (it stalls), or data in the control buffer, the request's
   * buf, which is the stack's again once the callback has returned. Fix:
   * pass the answer in the app's own memory. */
  U2PS_CONTRACT_DEV_REPLY_DATA,
  /* A callback returned a value its header does not list:
   * u2ps_app_on_request() or u2ps_app_on_sampling_frequency() one below 0
   * other than U2PS_REQ_STALL and U2PS_REQ_DEFER, or
   * u2ps_app_on_stream_start() one outside u2ps_stream_start_t (taken as
   * U2PS_STREAM_REFUSE). Fix: return a listed value. */
  U2PS_CONTRACT_DEV_CALLBACK_RESULT,
  /* Host stack (u2ps_host.h) */
  /* A thread call (u2ps_host_init, connect, vbus, tick, test_mode,
   * suspend, resume, stream_start, stream_stop or stats) inside an app
   * callback, in the
   * port's interrupt or in thread with the vectors masked. Fix: set a flag
   * in the callback; the app's thread makes the call. */
  U2PS_CONTRACT_HOST_CALL_IN_CALLBACK,
  /* A thread call, or u2ps_host_queue() outside a callback, while another
   * thread call held the port's vectors masked: from an interrupt that
   * preempted it, or from a second thread. Fix: make every thread call
   * from one thread, never from an interrupt. */
  U2PS_CONTRACT_HOST_CALL_NESTED,
  /* u2ps_host_connect(true) before a successful u2ps_host_init(): it does
   * nothing, and no session starts. Fix: connect only after
   * u2ps_host_init() returned U2PS_HOST_OK. */
  U2PS_CONTRACT_HOST_CONNECT_BEFORE_INIT,
  /* u2ps_host_tick() came more than 10 ms after the last one while it
   * timed a wait of the enumeration or a control transfer: the wait, or
   * the timeout, grows by the excess. Fix: tick at least every 10 ms. */
  U2PS_CONTRACT_HOST_TICK_LATE,
  /* u2ps_host_queue() for a stream number that u2ps_host_stream_start()
   * never returned (0, or a later one), or, inside a callback, for a
   * stream that does not run (before on_stream_start, or after
   * u2ps_host_stream_stop() or on_stream_stop): refused. Fix: queue only
   * between on_stream_start and the app's stop or on_stream_stop, with the
   * stream's number. (From thread, a queue that meets a stop the interrupt
   * has just made is not reported.) */
  U2PS_CONTRACT_HOST_QUEUE_STREAM,
  /* u2ps_host_queue() with data NULL or not on U2PS_HPORT_ALIGN: refused.
   * Or a packet with a byte where the port's DMA does not reach it, by the
   * port's reach rule (its configuration header, u2ps_hport_cfg.h) as the
   * port reported it at the last u2ps_host_init() (on the STM32H7S3 never
   * DTCM, ITCM nor the backup SRAM; AHB SRAM only with its clock on):
   * reported, and sent as without the checks, so the device gets what the
   * DMA reads there. Fix: place each packet on U2PS_HPORT_ALIGN in DMA
   * memory, all of its bytes. */
  U2PS_CONTRACT_HOST_QUEUE_DATA,
  /* u2ps_host_queue() with U2PS_HOST_QUEUE_MAX packets queued and not yet
   * reported by on_sent: refused. Fix: count the packets in flight, and
   * queue the next one after an on_sent. */
  U2PS_CONTRACT_HOST_QUEUE_FULL,
  /* u2ps_host_queue() without u2ps_host_slots_next() since the last queued
   * packet: the stack sends the size it computes now, which the app did
   * not know when it filled the packet. Fix: call u2ps_host_slots_next()
   * for each packet and fill exactly that many audio slots. */
  U2PS_CONTRACT_HOST_QUEUE_UNSIZED,
  /* u2ps_host_suspend() while a stream starts, runs or stops: refused,
   * since an isochronous stream needs SOFs. Fix: stop the stream
   * (u2ps_host_stream_stop) and suspend after its on_stream_stop. */
  U2PS_CONTRACT_HOST_SUSPEND_STREAM,
  /* u2ps_host_suspend() while the port is already suspended or resuming:
   * refused. Fix: suspend once; after u2ps_host_resume(), suspend again
   * only after on_resumed. (Not reported while a resume that the device's
   * remote wakeup began runs: the app cannot know of it before
   * on_resumed.) */
  U2PS_CONTRACT_HOST_SUSPEND_SUSPENDED,
  /* u2ps_host_resume() while the port is not suspended, or a second time
   * while the app's resume runs: refused. Fix: resume only after a
   * u2ps_host_suspend() that returned true, once, and wait for on_resumed.
   * (Not reported after the session ended, nor while or after a resume
   * that the device's remote wakeup began.) */
  U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED
} u2ps_contract_t;

#endif /* U2PS_TYPES_H */
