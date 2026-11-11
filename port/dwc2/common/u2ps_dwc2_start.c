/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_start.c: the pure parts of the DWC2 ports' start-up
 * (u2ps_dwc2_start.h).
 */
#include "u2ps_dwc2_start.h"

u2ps_dev_err_t u2ps_dwc2_dev_err(u2ps_dwc2_err_t e)
{
  switch (e) {
  case U2PS_DWC2_OK:
    return U2PS_DEV_OK;
  case U2PS_DWC2_ERR_CLOCK:
    return U2PS_DEV_ERR_CLOCK;
  case U2PS_DWC2_ERR_POWER:
    return U2PS_DEV_ERR_POWER;
  case U2PS_DWC2_ERR_TZEN:
    return U2PS_DEV_ERR_SECURITY;
  case U2PS_DWC2_ERR_PHY:
    return U2PS_DEV_ERR_PHY;
  case U2PS_DWC2_ERR_RESET:
    return U2PS_DEV_ERR_RESET;
  case U2PS_DWC2_ERR_MODE:
    return U2PS_DEV_ERR_MODE;
  case U2PS_DWC2_ERR_FIFO:
    return U2PS_DEV_ERR_MEMORY;
  case U2PS_DWC2_ERR_CYCLES:
    return U2PS_DEV_ERR_TIMER;
  }
  return U2PS_DEV_ERR_PORT;
}

u2ps_host_err_t u2ps_dwc2_host_err(u2ps_dwc2_err_t e)
{
  switch (e) {
  case U2PS_DWC2_OK:
    return U2PS_HOST_OK;
  case U2PS_DWC2_ERR_CLOCK:
    return U2PS_HOST_ERR_CLOCK;
  case U2PS_DWC2_ERR_POWER:
    return U2PS_HOST_ERR_POWER;
  case U2PS_DWC2_ERR_TZEN:
    return U2PS_HOST_ERR_SECURITY;
  case U2PS_DWC2_ERR_PHY:
    return U2PS_HOST_ERR_PHY;
  case U2PS_DWC2_ERR_RESET:
    return U2PS_HOST_ERR_RESET;
  case U2PS_DWC2_ERR_MODE:
    return U2PS_HOST_ERR_MODE;
  case U2PS_DWC2_ERR_FIFO:
    return U2PS_HOST_ERR_MEMORY;
  case U2PS_DWC2_ERR_CYCLES:
    return U2PS_HOST_ERR_TIMER;
  }
  return U2PS_HOST_ERR_PORT;
}

/* PWR_CSR2 bits 24 and 27, in the upper half-word */
#define CSR2_HI_USB33DEN   (1u << (24 - 16))
#define CSR2_HI_USBHSREGEN (1u << (27 - 16))

uint16_t u2ps_dwc2_h7s3_csr2_hi(uint16_t hi)
{
  return (uint16_t)(hi | CSR2_HI_USB33DEN | CSR2_HI_USBHSREGEN);
}

/* RCC_CCIPR1: USBPHYCSEL [13:12], USBREFCKSEL [11:8] */
#define CCIPR1_USB_MSK       (0x3Fu << 8)
#define CCIPR1_USBREFCKSEL(v) (((uint32_t)(v) & 0xFu) << 8)

uint32_t u2ps_dwc2_h7s3_ccipr1(uint32_t ccipr1, uint32_t refsel)
{
  return (ccipr1 & ~CCIPR1_USB_MSK) | CCIPR1_USBREFCKSEL(refsel);
}
