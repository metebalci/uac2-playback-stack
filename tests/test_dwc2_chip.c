/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dwc2_chip.c: the DWC2 chip facts (u2ps_dwc2_chip.h) and the pure
 * parts of the start-up (u2ps_dwc2_start.h). Built once per chip with its
 * macro (TEST_U5A5, TEST_F723, TEST_H7S3), each against the values of its
 * manuals: the OTG_HS IRQ number, the base addresses, the core's highest
 * clock, the features (Cortex-M7, data cache, GCCFG overrides, the F723
 * PHY selection), the errata gates and DTCM. The headers that branch on
 * the chip (u2ps_dwc2.h, u2ps_dwc2_regs.h, the configuration headers)
 * must compile for it. Every variant also checks the start reasons'
 * mapping onto the stacks' errors and the PHY start-up values.
 *
 * TEST_UNKNOWN builds the same source with an unknown chip macro and
 * TEST_TWO with two chip macros: both must fail to compile, at the chip
 * header's #error (CTest WILL_FAIL); their expected values compile.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "u2ps_dwc2.h"
#include "u2ps_dwc2_regs.h"
#include "u2ps_dwc2_start.h"
#include "u2ps_hport_cfg.h"

typedef struct {
  unsigned irq;
  unsigned otg, rcc;
  unsigned core_max;
  int cm7, dcache, gccfg_ovr, f7_physel, iso_in, zlp;
  unsigned dtcm_start, dtcm_end;
} chip_t;

#if defined(TEST_U5A5)
/* RM0456 Rev 7: Table 186 p. 916; Table 6 pp. 145-146; 11.4.9 p. 498; 9.3
 * p. 383; 73.14.18 p. 3347 */
static const chip_t want = {73, 0x42040000u, 0x46020C00u, 160000000u, 0, 0, 1, 0, 0, 0, 0, 0};
#define TEST_NAME "stm32u5a5"
#elif defined(TEST_F723)
/* stm32f723xx.h (IRQ 77, the addresses); RM0431 5.2, 2.1, 32.15.4,
 * 32.15.15 */
static const chip_t want = {77,  0x40040000u, 0x40023800u, 216000000u, 1, 1, 0, 1, 0, 0,
                            0x20000000u, 0x20010000u};
#define TEST_NAME "stm32f723"
#elif defined(TEST_H7S3)
/* RM0477 Rev 10: Table 142 p. 863; Table 7 p. 158; p. 154; Table 6;
 * pp. 3080, 3103-3104; Table 13 p. 162. DS14359 Rev 7 Table 26 p. 159.
 * ES0596 Rev 10 2.21.1 p. 34, 2.21.3 pp. 34-35. */
static const chip_t want = {91,  0x40040000u, 0x58024400u, 600000000u, 1, 1, 1, 0, 1, 1,
                            0x20000000u, 0x20030000u};
#define TEST_NAME "stm32h7s3"
#elif defined(TEST_UNKNOWN) || defined(TEST_TWO)
static const chip_t want;
#define TEST_NAME "none"
#endif

static void test_facts(void)
{
  printf("  chip %s\n", TEST_NAME);
  CHECK_EQ(U2PS_DWC2_IRQ, want.irq);
  CHECK_EQ(U2PS_DWC2_OTG_BASE, want.otg);
  CHECK_EQ(U2PS_DWC2_RCC_BASE, want.rcc);
  CHECK_EQ(U2PS_DWC2_CORE_HZ_MAX, want.core_max);
  CHECK_EQ(U2PS_DWC2_CM7, want.cm7);
  CHECK_EQ(U2PS_DWC2_DCACHE, want.dcache);
  CHECK_EQ(U2PS_DWC2_GCCFG_OVR, want.gccfg_ovr);
  CHECK_EQ(U2PS_DWC2_F7_PHYSEL, want.f7_physel);
  CHECK_EQ(U2PS_DWC2_ERRATUM_ISO_IN, want.iso_in);
  CHECK_EQ(U2PS_DWC2_ERRATUM_ZLP, want.zlp);
  CHECK_EQ(U2PS_DWC2_DTCM_START, want.dtcm_start);
  CHECK_EQ(U2PS_DWC2_DTCM_END, want.dtcm_end);
#if defined(TEST_U5A5)
  CHECK_EQ(U2PS_DWC2_PWR_BASE, 0x46020800u);
  CHECK_EQ(U2PS_DWC2_SYSCFG_BASE, 0x46000400u);
  CHECK_EQ(U2PS_DWC2_FLASH_BASE, 0x40022000u);
#elif defined(TEST_F723)
  CHECK_EQ(U2PS_DWC2_USBPHYC_BASE, 0x40017C00u);
#elif defined(TEST_H7S3)
  CHECK_EQ(U2PS_DWC2_PWR_BASE, 0x58024800u);
  CHECK_EQ(U2PS_DWC2_FLASH_BASE, 0x52002000u);
  /* The registers of the start-up and the DMA rule (RM0477 7.8, 6.8.4,
   * 5.9.34) */
  CHECK_EQ((uintptr_t)&RCC_CR, 0x58024400u);
  CHECK_EQ((uintptr_t)&RCC_CCIPR1, 0x5802444Cu);
  CHECK_EQ((uintptr_t)&RCC_AHB1ENR, 0x58024538u);
  CHECK_EQ((uintptr_t)&RCC_AHB2ENR, 0x5802453Cu);
  CHECK_EQ((uintptr_t)&PWR_CSR2_HI, 0x5802480Eu);
  CHECK_EQ((uintptr_t)&FLASH_OBW2SR, 0x52002268u);
  CHECK_EQ(RCC_CR_HSERDY, 1u << 17);
  CHECK_EQ(RCC_AHB1ENR_USBPHYCEN, 1u << 26);
  CHECK_EQ(RCC_AHB1ENR_OTGHSEN, 1u << 25);
#endif
  /* The cache helpers exist on every chip; DMA memory alignment of the
   * host port (U2PS_HPORT_ALIGN 4 on every DWC2 chip) */
  CHECK_EQ(U2PS_HPORT_ALIGN, 4u);
  CHECK_EQ((uintptr_t)&OTG_GINTMSK - U2PS_DWC2_OTG_BASE, 0x018u);
  CHECK_EQ(GINT_CIDSCHG, 1u << 28);
}

/* The port's start reasons onto the stacks' errors (u2ps_dwc2_dcd.h,
 * u2ps_dwc2_hcd.h): each one, and an unknown value onto PORT */
static void test_errors(void)
{
  static const struct {
    u2ps_dwc2_err_t port;
    u2ps_dev_err_t dev;
    u2ps_host_err_t host;
  } map[] = {
    {U2PS_DWC2_OK, U2PS_DEV_OK, U2PS_HOST_OK},
    {U2PS_DWC2_ERR_CLOCK, U2PS_DEV_ERR_CLOCK, U2PS_HOST_ERR_CLOCK},
    {U2PS_DWC2_ERR_POWER, U2PS_DEV_ERR_POWER, U2PS_HOST_ERR_POWER},
    {U2PS_DWC2_ERR_TZEN, U2PS_DEV_ERR_SECURITY, U2PS_HOST_ERR_SECURITY},
    {U2PS_DWC2_ERR_PHY, U2PS_DEV_ERR_PHY, U2PS_HOST_ERR_PHY},
    {U2PS_DWC2_ERR_RESET, U2PS_DEV_ERR_RESET, U2PS_HOST_ERR_RESET},
    {U2PS_DWC2_ERR_MODE, U2PS_DEV_ERR_MODE, U2PS_HOST_ERR_MODE},
    {U2PS_DWC2_ERR_FIFO, U2PS_DEV_ERR_MEMORY, U2PS_HOST_ERR_MEMORY},
    {U2PS_DWC2_ERR_CYCLES, U2PS_DEV_ERR_TIMER, U2PS_HOST_ERR_TIMER},
  };
  for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
    CHECK_EQ(u2ps_dwc2_dev_err(map[i].port), map[i].dev);
    CHECK_EQ(u2ps_dwc2_host_err(map[i].port), map[i].host);
  }
  /* Every reason has its line: the last one is CYCLES */
  CHECK_EQ(sizeof map / sizeof map[0], U2PS_DWC2_ERR_CYCLES + 1);
  CHECK_EQ(u2ps_dwc2_dev_err((u2ps_dwc2_err_t)(U2PS_DWC2_ERR_CYCLES + 1)), U2PS_DEV_ERR_PORT);
  CHECK_EQ(u2ps_dwc2_host_err((u2ps_dwc2_err_t)(U2PS_DWC2_ERR_CYCLES + 1)), U2PS_HOST_ERR_PORT);
  /* The new errors come last: the earlier values keep their numbers */
  CHECK_EQ(U2PS_DEV_ERR_TIMER, U2PS_DEV_ERR_DMA_MEMORY + 1);
  CHECK_EQ(U2PS_HOST_ERR_TIMER, U2PS_HOST_ERR_DMA_MEMORY + 1);
}

/* The PHY reference codes of the STM32U5 (OTG_HSPHYC CLKSEL, RM0456
 * 15.3.12) and the STM32H7S3 (RCC_CCIPR1.USBREFCKSEL, RM0477 7.8.24,
 * p. 459), the same codes; 0 for a frequency neither takes */
static void test_refsel(void)
{
  static const struct {
    unsigned long hz;
    unsigned code;
  } codes[] = {{16000000, 0x3}, {19200000, 0x8}, {20000000, 0x9}, {24000000, 0xA},
               {26000000, 0xE}, {32000000, 0xB}, {25000000, 0},   {12000000, 0},
               {0, 0},          {24000001, 0},   {8000000, 0}};
  for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; i++)
    CHECK_EQ(U2PS_DWC2_REFSEL(codes[i].hz), codes[i].code);
  /* A constant expression the preprocessor evaluates, as u2ps_dwc2_phy.c
   * needs for its #error */
#if U2PS_DWC2_REFSEL(24000000) != 0xA || U2PS_DWC2_REFSEL(25000000) != 0
  CHECK(false);
#endif
}

/* The STM32H7S3's PWR_CSR2, upper half-word (RM0477 6.8.4, p. 323):
 * USB33DEN (24) and USBHSREGEN (27) set, every other bit kept, USBREGEN
 * (25) as it was */
static void test_csr2(void)
{
  CHECK_EQ(u2ps_dwc2_h7s3_csr2_hi(0x0000u), 0x0900u);
  CHECK_EQ(u2ps_dwc2_h7s3_csr2_hi(0x0200u), 0x0B00u); /* USBREGEN kept */
  CHECK_EQ(u2ps_dwc2_h7s3_csr2_hi(0x0400u), 0x0D00u); /* USB33RDY (ro) as read */
  CHECK_EQ(u2ps_dwc2_h7s3_csr2_hi(0x0001u), 0x0901u); /* SDEXTRDY (ro) as read */
  CHECK_EQ(u2ps_dwc2_h7s3_csr2_hi(0xF000u), 0xF900u); /* reserved bits kept */
  CHECK_EQ(u2ps_dwc2_h7s3_csr2_hi(0x0900u), 0x0900u); /* already set */
  CHECK_EQ(u2ps_dwc2_h7s3_csr2_hi(0xFFFFu), 0xFFFFu);
}

/* The STM32H7S3's RCC_CCIPR1 (RM0477 7.8.24, p. 459): USBPHYCSEL[13:12]
 * 00 (hse_ker_ck), USBREFCKSEL[11:8] the code, every other bit kept */
static void test_ccipr1(void)
{
  CHECK_EQ(u2ps_dwc2_h7s3_ccipr1(0x00000A00u, 0xAu), 0x00000A00u); /* the reset value */
  CHECK_EQ(u2ps_dwc2_h7s3_ccipr1(0x00000000u, 0xEu), 0x00000E00u);
  CHECK_EQ(u2ps_dwc2_h7s3_ccipr1(0xFFFFFFFFu, 0x3u), 0xFFFFC3FFu);
  CHECK_EQ(u2ps_dwc2_h7s3_ccipr1(0x00003000u, 0xBu), 0x00000B00u); /* USBPHYCSEL 11 to 00 */
  CHECK_EQ(u2ps_dwc2_h7s3_ccipr1(0x000010C0u, 0x8u), 0x000008C0u);
}

int main(void)
{
  test_facts();
  test_errors();
  test_refsel();
  test_csr2();
  test_ccipr1();
  return check_done();
}
