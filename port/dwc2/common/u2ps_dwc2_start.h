/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_start.h: the pure parts of the DWC2 ports' start-up, which
 * build on the PC too, for the tests: the reasons a start fails and their
 * mapping onto the stacks' errors, the PHY reference clock codes, and the
 * register values the STM32H7S3's start writes. The register code that
 * uses them is u2ps_dwc2_phy.c (u2ps_dwc2.h).
 *
 * Manuals: RM0456 Rev 7 (STM32U5), RM0477 Rev 10 (STM32H7Rx/7Sx).
 */
#ifndef U2PS_DWC2_START_H
#define U2PS_DWC2_START_H

#include <stdint.h>

#include "u2ps_types.h"

/* Why the controller did not start: each value names its cause and what
 * to change */
typedef enum {
  U2PS_DWC2_OK,
  /* The HSE is not running: start it before the port's init, at the
   * frequency U2PS_DWC2_HSE_HZ names */
  U2PS_DWC2_ERR_CLOCK,
  /* U5: the voltage range is below range 2: set range 1 or 2 (PWR) before
   * the port's init */
  U2PS_DWC2_ERR_POWER,
  /* U5: TrustZone is enabled (FLASH_OPTR.TZEN); the port is nonsecure
   * only: set option byte TZEN = 0 */
  U2PS_DWC2_ERR_TZEN,
  /* A PHY supply or LDO did not come up: check the board's USB supplies
   * (H7S3: VDD33USB, which PWR_CSR2.USB33RDY reports) */
  U2PS_DWC2_ERR_PHY,
  /* The core reset did not end: the core's bus clock or the PHY clock is
   * not running; check the clock set-up and U2PS_DWC2_HSE_HZ */
  U2PS_DWC2_ERR_RESET,
  /* The core did not enter device mode: other code drives the controller
   * in host mode; leave the controller to the port */
  U2PS_DWC2_ERR_MODE,
  /* The FIFO RAM plan does not fit, or a flush timed out: the plan is
   * sized from U2PS_ISO_OUT_MPS_MAX; lower it to the largest
   * wMaxPacketSize the description needs */
  U2PS_DWC2_ERR_FIFO,
  /* Cortex-M7 (F723, H7S3): the DWT cycle counter, which times every wait
   * of the port, does not count after the port unlocked the DWT (DWT_LAR)
   * and enabled it (DEMCR.TRCENA, DWT_CTRL.CYCCNTENA); nothing else was
   * touched. Leave the DWT to the port (no code or debugger may lock it or
   * clear those bits) */
  U2PS_DWC2_ERR_CYCLES
} u2ps_dwc2_err_t;

/* The port's reason as the device stack's and as the host stack's error:
 * OK to OK, CLOCK to ERR_CLOCK, POWER to ERR_POWER, TZEN to
 * ERR_SECURITY, PHY to ERR_PHY, RESET to ERR_RESET, MODE to ERR_MODE,
 * FIFO to ERR_MEMORY, CYCLES to ERR_TIMER; any other value to ERR_PORT */
u2ps_dev_err_t u2ps_dwc2_dev_err(u2ps_dwc2_err_t e);
u2ps_host_err_t u2ps_dwc2_host_err(u2ps_dwc2_err_t e);

/* The code of a PHY reference clock of hz Hz, the same on the STM32U5
 * (SYSCFG_OTGHSPHYCR.CLKSEL, RM0456 15.3.12) and the STM32H7S3
 * (RCC_CCIPR1.USBREFCKSEL, RM0477 7.8.24, p. 459): 16, 19.2, 20, 24, 26
 * or 32 MHz; 0 for any other frequency. A constant expression, also in
 * #if. */
#define U2PS_DWC2_REFSEL(hz)                                                                \
  ((hz) == 16000000 ? 0x3u                                                                  \
   : (hz) == 19200000 ? 0x8u                                                                \
   : (hz) == 20000000 ? 0x9u                                                                \
   : (hz) == 24000000 ? 0xAu                                                                \
   : (hz) == 26000000 ? 0xEu                                                                \
   : (hz) == 32000000 ? 0xBu                                                                \
                      : 0u)

/* STM32H7S3, PWR_CSR2 (RM0477 6.8.4, p. 323), its upper half-word as read
 * (bits 31:16 in 15:0): the value to write back, with USB33DEN (bit 24)
 * and USBHSREGEN (27) set and every other bit as read, USBREGEN (25)
 * among them: whether the USB regulator runs is the board's choice. The
 * port writes only this half-word, since the lower byte is write-once and
 * locks at its first write (6.8.4; Table 44, p. 287). */
uint16_t u2ps_dwc2_h7s3_csr2_hi(uint16_t hi);

/* STM32H7S3, RCC_CCIPR1 (RM0477 7.8.24, p. 459): ccipr1 with USBPHYCSEL
 * (bits 13:12) 00, hse_ker_ck, and USBREFCKSEL (11:8) refsel, every other
 * bit as it was. Written only while RCC_AHB1ENR.USBPHYCEN is 0 (7.5,
 * p. 401). */
uint32_t u2ps_dwc2_h7s3_ccipr1(uint32_t ccipr1, uint32_t refsel);

#endif /* U2PS_DWC2_START_H */
