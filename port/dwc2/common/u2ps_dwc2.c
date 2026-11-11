/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2.c: DWC2 code shared by the device and host ports
 * (u2ps_dwc2.h): bounded waits, core reset, FIFO RAM set-up and flushes,
 * the NVIC mask, data cache maintenance and the DMA memory check. ARM
 * builds only; the start-up is in u2ps_dwc2_phy.c.
 */
#include "u2ps_dwc2.h"

#include "u2ps_dwc2_dma.h"
#include "u2ps_dwc2_fifo.h"
#include "u2ps_dwc2_regs.h"

#ifndef U2PS_DWC2_CORE_HZ
#error "u2ps_dwc2.c: U2PS_DWC2_CORE_HZ, the highest core clock in Hz, is not defined: build with the project's CMake, whose cache variable U2PS_DWC2_CORE_HZ sets it (default 160000000 on the STM32U5, 216000000 on the STM32F723, 600000000 on the STM32H7S3)"
#endif

/* The core clock: above 30 MHz, since the AHB clock must be (RM0456
 * 73.4.3, p. 3294; RM0431 32.4.3; RM0477 62.4.3, p. 3051) and is at most
 * the core's, and at most the chip's highest, U2PS_DWC2_CORE_HZ_MAX: 160
 * MHz on the U5 (RM0456 11.4.9, p. 498), 216 MHz on the F723 (RM0431 5.2),
 * 600 MHz on the H7S3 (DS14359 Table 26, p. 159) */
#if defined(U2PS_CHIP_STM32U5A5)
_Static_assert(U2PS_DWC2_CORE_HZ > 30000000u && U2PS_DWC2_CORE_HZ <= U2PS_DWC2_CORE_HZ_MAX,
               "U2PS_DWC2_CORE_HZ must be 30000001..160000000 on the STM32U5 (OTG_HS needs an AHB "
               "clock above 30 MHz): set -DU2PS_DWC2_CORE_HZ to the highest core clock the app "
               "runs at (default 160000000)");
#elif defined(U2PS_CHIP_STM32F723)
_Static_assert(U2PS_DWC2_CORE_HZ > 30000000u && U2PS_DWC2_CORE_HZ <= U2PS_DWC2_CORE_HZ_MAX,
               "U2PS_DWC2_CORE_HZ must be 30000001..216000000 on the STM32F723 (OTG_HS needs an "
               "AHB clock above 30 MHz): set -DU2PS_DWC2_CORE_HZ to the highest core clock the "
               "app runs at (default 216000000)");
#elif defined(U2PS_CHIP_STM32H7S3)
_Static_assert(U2PS_DWC2_CORE_HZ > 30000000u && U2PS_DWC2_CORE_HZ <= U2PS_DWC2_CORE_HZ_MAX,
               "U2PS_DWC2_CORE_HZ must be 30000001..600000000 on the STM32H7S3 (OTG_HS needs an "
               "AHB clock above 30 MHz; 600 MHz is the CPU's highest, at VOS high): set "
               "-DU2PS_DWC2_CORE_HZ to the highest core clock the app runs at (default "
               "600000000)");
#else
#error "u2ps_dwc2.c: no core clock bound for this chip: add one with its facts (u2ps_dwc2_chip.h)"
#endif

bool u2ps_dwc2_wait_us(volatile uint32_t *reg, uint32_t mask, uint32_t value, uint32_t us)
{
  uint32_t t0 = DWT_CYCCNT;
  uint32_t limit = us * U2PS_DWC2_CYCLES_PER_US;
  while ((*reg & mask) != value) {
    if (DWT_CYCCNT - t0 > limit)
      return (*reg & mask) == value;
  }
  return true;
}

void u2ps_dwc2_delay_us(uint32_t us)
{
  uint32_t t0 = DWT_CYCCNT;
  uint32_t limit = us * U2PS_DWC2_CYCLES_PER_US;
  while (DWT_CYCCNT - t0 < limit) {
  }
}

/* ---- Core ------------------------------------------------------------------- */

bool u2ps_dwc2_core_reset(void)
{
#if U2PS_DWC2_F7_PHYSEL == 1
  /* The internal UTMI HS PHY: PHYSEL = 0 and ULPISEL = 0 (RM0431 32.15.4),
   * and GCCFG.PWRDWN = 0, "USB FS PHY disabled; HS PHY enabled" (RM0431
   * 32.15.15). The PHY is selected before the reset, which "is used ...
   * when the user dynamically changes the PHY selection bits" (RM0456
   * 73.14.5, p. 3327). */
  OTG_GUSBCFG &= ~(GUSBCFG_PHYSEL | GUSBCFG_ULPISEL);
  OTG_GCCFG &= ~GCCFG_PWRDWN;
  /* The HS PHY's enable: bit 23 of OTG_GCCFG, which RM0431 32.15.15 marks
   * reserved; ST's stm32f723xx.h (cmsis_device_f7) names it
   * USB_OTG_GCCFG_PHYHSEN, "HS PHY enable" */
  OTG_GCCFG |= GCCFG_PHYHSEN;
#elif U2PS_DWC2_F7_PHYSEL != 0
#error "u2ps_dwc2.c: U2PS_DWC2_F7_PHYSEL must be 0 or 1 (u2ps_dwc2_chip.h)"
#endif
  /* Disconnected through the reset, which keeps SDIS (RM0456 73.14.5,
   * p. 3327); the device port attaches later */
  OTG_DCTL |= DCTL_SDIS;
  if (!u2ps_dwc2_wait_us(&OTG_GRSTCTL, GRSTCTL_AHBIDL, GRSTCTL_AHBIDL, 10000u))
    return false;
  OTG_GRSTCTL |= GRSTCTL_CSRST;
  if (!u2ps_dwc2_wait_us(&OTG_GRSTCTL, GRSTCTL_CSRST, 0u, 10000u))
    return false;
  /* "at least 3 PHY clocks" (30 or 60 MHz) before the PHY domain is
   * accessed: 1 ms is far more */
  u2ps_dwc2_delay_us(1000u);
  return u2ps_dwc2_wait_us(&OTG_GRSTCTL, GRSTCTL_AHBIDL, GRSTCTL_AHBIDL, 10000u);
}

bool u2ps_dwc2_flush_tx(uint32_t fifo)
{
  /* The core must not be writing the FIFO (AHBIDL); TXFNUM stays until the
   * core clears TXFFLSH (RM0456 73.14.5, p. 3326) */
  if (!u2ps_dwc2_wait_us(&OTG_GRSTCTL, GRSTCTL_AHBIDL, GRSTCTL_AHBIDL, U2PS_DWC2_FLUSH_US))
    return false;
  OTG_GRSTCTL = GRSTCTL_TXFNUM(fifo) | GRSTCTL_TXFFLSH;
  return u2ps_dwc2_wait_us(&OTG_GRSTCTL, GRSTCTL_TXFFLSH, 0u, U2PS_DWC2_FLUSH_US);
}

bool u2ps_dwc2_flush_rx(void)
{
  /* Transfer stop for OUT endpoints (RM0431 32.16.6; RM0456 73.15.6,
   * p. 3442): AHBIDL, RXFFLSH, polled for less than 10 ms, and once more if
   * it is still set (a packet on EP0 may be arriving) */
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!u2ps_dwc2_wait_us(&OTG_GRSTCTL, GRSTCTL_AHBIDL, GRSTCTL_AHBIDL, 1000u))
      return false;
    OTG_GRSTCTL |= GRSTCTL_RXFFLSH;
    if (u2ps_dwc2_wait_us(&OTG_GRSTCTL, GRSTCTL_RXFFLSH, 0u, 5000u))
      return true;
  }
  return false;
}

bool u2ps_dwc2_fifo_setup(uint16_t out_mps, uint16_t fb_mps)
{
  u2ps_dwc2_fifo_t f;
  bool ok = u2ps_dwc2_fifo_plan(out_mps, fb_mps, &f);

  /* Depths and start addresses in words (u2ps_dwc2_fifo.h): RM0456
   * 73.14.14, 73.14.16 and 73.14.22, p. 3344-3353 */
  OTG_GRXFSIZ = f.rx;
  OTG_DIEPTXF0 = ((uint32_t)f.tx0 << 16) | f.tx0_start;
  OTG_DIEPTXF(1) = ((uint32_t)f.tx_fb << 16) | f.tx_fb_start;
  /* "Flushing is normally recommended when FIFOs are reconfigured" (RM0456
   * 73.14.5, p. 3326) */
  ok = u2ps_dwc2_flush_tx(0x10u) && ok;
  ok = u2ps_dwc2_flush_rx() && ok;
  return ok;
}

/* ---- NVIC ------------------------------------------------------------------- */

#define NVIC_ISER(n) U2PS_DWC2_R32(0xE000E100u + 4u * (uint32_t)(n))
#define NVIC_ICER(n) U2PS_DWC2_R32(0xE000E180u + 4u * (uint32_t)(n))

void u2ps_dwc2_irq_mask(bool masked)
{
  uint32_t bit = 1u << (U2PS_DWC2_IRQ % 32u);

  if (masked) {
    NVIC_ICER(U2PS_DWC2_IRQ / 32u) = bit;
    __asm volatile("dsb 0xF" ::: "memory");
    __asm volatile("isb 0xF" ::: "memory");
  } else {
    __asm volatile("" ::: "memory");
    NVIC_ISER(U2PS_DWC2_IRQ / 32u) = bit;
  }
}

/* ---- Data cache ------------------------------------------------------------- */

#if U2PS_DWC2_DCACHE == 1
/* The cache maintenance operations by address (u2ps_dwc2.h); the
 * Cortex-M7 line is 32 bytes. On the packet path, when the data cache is
 * on. */
#define LINE 32u

U2PS_FAST_TEXT void u2ps_dwc2_dcache_lines(uint32_t op, uintptr_t addr, uint32_t len)
{
  uintptr_t end = addr + len;
  __asm volatile("dsb 0xF" ::: "memory");
  for (addr &= ~(uintptr_t)(LINE - 1u); addr < end; addr += LINE)
    U2PS_DWC2_R32(op) = (uint32_t)addr;
  __asm volatile("dsb 0xF" ::: "memory");
  __asm volatile("isb 0xF" ::: "memory");
}

#elif U2PS_DWC2_DCACHE != 0
#error "u2ps_dwc2.c: U2PS_DWC2_DCACHE must be 0 or 1 (u2ps_dwc2_chip.h)"
#endif

/* ---- DMA memory ------------------------------------------------------------- */

#if defined(U2PS_CHIP_STM32U5A5)

void u2ps_dwc2_pwr_clock(void)
{
  RCC_AHB3ENR |= RCC_AHB3ENR_PWREN;
  (void)RCC_AHB3ENR; /* the clock is on before the next access */
}

/* The rule of u2ps_dwc2_dma.h over the registers' values now. PWR_CR1 is
 * read only while the PWR clock runs ("When the peripheral clock is not
 * active, read or write access to peripheral registers is not supported",
 * RM0456 11.8.32); the checks from thread enable it first
 * (u2ps_dwc2_pwr_clock), as the port's start does. */
bool u2ps_dwc2_dma_ok(const void *addr, uint32_t len)
{
  u2ps_dwc2_u5_ram_t ram = {
    .ahb1enr = RCC_AHB1ENR,
    .ahb2enr1 = RCC_AHB2ENR1,
    .ahb2enr2 = RCC_AHB2ENR2,
    .pwr_cr1 = (RCC_AHB3ENR & RCC_AHB3ENR_PWREN) != 0u ? PWR_CR1 : 0u,
  };
  return u2ps_dwc2_u5_dma_ok((uintptr_t)addr, len, &ram);
}

/* The same registers, with the PWR clock on */
unsigned u2ps_dwc2_dma_ranges(u2ps_dwc2_range_t *r)
{
  u2ps_dwc2_pwr_clock();
  u2ps_dwc2_u5_ram_t ram = {
    .ahb1enr = RCC_AHB1ENR,
    .ahb2enr1 = RCC_AHB2ENR1,
    .ahb2enr2 = RCC_AHB2ENR2,
    .pwr_cr1 = PWR_CR1,
  };
  return u2ps_dwc2_u5_dma_ranges(&ram, r);
}

#elif defined(U2PS_CHIP_STM32F723)

void u2ps_dwc2_pwr_clock(void)
{
}

bool u2ps_dwc2_dma_ok(const void *addr, uint32_t len)
{
  return u2ps_dwc2_f723_dma_ok((uintptr_t)addr, len, RCC_AHB1ENR);
}

unsigned u2ps_dwc2_dma_ranges(u2ps_dwc2_range_t *r)
{
  return u2ps_dwc2_f723_dma_ranges(RCC_AHB1ENR, r);
}

#elif defined(U2PS_CHIP_STM32H7S3)

/* FLASH_OBW2SR and RCC_AHB2ENR need no clock enable of their own */
void u2ps_dwc2_pwr_clock(void)
{
}

bool u2ps_dwc2_dma_ok(const void *addr, uint32_t len)
{
  return u2ps_dwc2_h7s3_dma_ok((uintptr_t)addr, len, FLASH_OBW2SR, RCC_AHB2ENR);
}

unsigned u2ps_dwc2_dma_ranges(u2ps_dwc2_range_t *r)
{
  return u2ps_dwc2_h7s3_dma_ranges(FLASH_OBW2SR, RCC_AHB2ENR, r);
}

#else
#error "u2ps_dwc2.c: no DMA memory rule for this chip: add one with its facts (u2ps_dwc2_chip.h)"
#endif
