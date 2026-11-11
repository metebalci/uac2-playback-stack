/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * board.h: the NUCLEO-U5A5ZJ-Q for the u2ps test app: clocks, the
 * millisecond tick, the ST-LINK virtual COM port, the LEDs and VBUS.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

/* Clocks (160 MHz from the 16 MHz HSE), tick, VCP, LEDs. Returns false if
 * a clock did not start; the VCP then stays silent. */
bool board_init(void);

/* Milliseconds since board_init(), from SysTick */
uint32_t board_millis(void);

/* The VCP, 115200 8N1: queues text for sending from its interrupt; what
 * does not fit the queue is dropped and counted */
void board_vcp_write(const char *s);
uint32_t board_vcp_dropped(void);

/* A received line without its end (CR or LF), or NULL if none is
 * complete; valid until the next call */
const char *board_vcp_line(void);

/* VBUS of the user USB-C port, read with ADC1 on PC2: present at or above
 * 3.5 V; absent if the ADC did not start (board_vbus_adc() false). Not
 * debounced. board_vbus_mv() is the last reading in mV, 0 without the
 * ADC. */
bool board_vbus(void);
bool board_vbus_adc(void);
uint32_t board_vbus_mv(void);

/* LD1 green and LD2 blue */
void board_led_green(bool on);
void board_led_blue(bool on);

/* DBGMCU_IDCODE: device and revision identifiers */
uint32_t board_idcode(void);

/* The DWT cycle counter, running from board_init(), and its cycles per
 * microsecond */
uint32_t board_cycles(void);
uint32_t board_cycles_per_us(void);

/* For clock checks against the host: OTG_DSTS.FNSOF, the number of the
 * last SOF; *hs tells that it counts microframes (high speed), not frames */
uint32_t board_usb_frame(bool *hs);

/* The 96-bit unique device ID, words 0, 1 and 2 (RM0456, UID_BASE) */
void board_uid(uint32_t uid[3]);

#endif /* BOARD_H */
