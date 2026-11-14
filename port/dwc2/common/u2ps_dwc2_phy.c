/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_phy.c: the DWC2 start-up's register code (u2ps_dwc2.h): the
 * DWT cycle counter and the PHY start of each chip. ARM builds; the PC
 * test test_dwc2_phy.c runs it against a register model (tests/
 * fake_regs.h), so it reaches registers through U2PS_DWC2_R32 and
 * U2PS_DWC2_R16 only and holds no assembly.
 */
#include "u2ps_dwc2.h"

#include "u2ps_dwc2_regs.h"
#include "u2ps_dwc2_start.h"

#ifndef U2PS_DWC2_HSE_HZ
#error "u2ps_dwc2_phy.c: U2PS_DWC2_HSE_HZ, the HSE frequency in Hz, is not defined: build with the project's CMake, whose cache variable U2PS_DWC2_HSE_HZ sets it (default 16000000 on the STM32U5, 25000000 on the STM32F723, 24000000 on the STM32H7S3)"
#endif

/* ---- The cycle counter ------------------------------------------------------ */

#if U2PS_DWC2_CM7 == 1
/* Cortex-M7: the DWT's registers may be locked against software writes
 * (CoreSight lock access register, DWT_LAR, u2ps_dwc2_regs.h), and then
 * CYCCNTENA does not set and every bounded wait of the port would end at
 * once or never (inferred from reports, not from a manual). So the key
 * first, then the enables, then a check that CYCCNT advances: it counts
 * core cycles, so two reads differ at once when it runs. */
bool u2ps_dwc2_cycles_start(void)
{
  DEMCR |= DEMCR_TRCENA;
  DWT_LAR = DWT_LAR_KEY;
  DWT_CTRL |= DWT_CTRL_CYCCNTENA;
  uint32_t t0 = DWT_CYCCNT;
  for (unsigned i = 0; i < 16u; i++)
    if (DWT_CYCCNT != t0)
      return true;
  return false;
}
#elif U2PS_DWC2_CM7 == 0
/* Cortex-M33 (U5): no lock access register on the DWT (Armv8-M) */
bool u2ps_dwc2_cycles_start(void)
{
  DEMCR |= DEMCR_TRCENA;
  DWT_CTRL |= DWT_CTRL_CYCCNTENA;
  return true;
}
#else
#error "u2ps_dwc2_phy.c: U2PS_DWC2_CM7 must be 0 or 1 (u2ps_dwc2_chip.h)"
#endif

/* ---- PHY start-up ------------------------------------------------------------ */

#if defined(U2PS_CHIP_STM32U5A5)

/* CLKSEL for the HSE frequency (RM0456 15.3.12) */
#if U2PS_DWC2_REFSEL(U2PS_DWC2_HSE_HZ) == 0
#error "u2ps_dwc2_phy.c: U2PS_DWC2_HSE_HZ must be 16000000, 19200000, 20000000, 24000000, 26000000 or 32000000 on the STM32U5, the references its OTG_HS PHY takes (RM0456 15.3.12): set -DU2PS_DWC2_HSE_HZ to the board's HSE, one of these"
#endif
#define PHY_CLKSEL U2PS_DWC2_REFSEL(U2PS_DWC2_HSE_HZ)

/* The sequences of RM0456 10.6.3 (p. 414, "before using the
 * USB/OTG_FS/OTG_HS") and 15.2.3 (p. 651, "Configuring the OTG_HS PHY").
 * The app has set voltage range 1 or 2 and the EPOD booster clock
 * (PLL1MBOOST in RCC_PLL1CFGR) with its clock set-up; here the range is
 * checked. VDDUSB is taken to be present: the board supplies it from VDD,
 * so the voltage monitor is not used. The port uses the nonsecure
 * addresses only, so it refuses to start with TrustZone enabled. */
u2ps_dwc2_err_t u2ps_dwc2_phy_start(void)
{
  if (!u2ps_dwc2_cycles_start())
    return U2PS_DWC2_ERR_CYCLES;
  if ((FLASH_OPTR & FLASH_OPTR_TZEN) != 0u)
    return U2PS_DWC2_ERR_TZEN;
  u2ps_dwc2_pwr_clock();
  RCC_APB3ENR |= RCC_APB3ENR_SYSCFGEN;
  (void)RCC_APB3ENR; /* the clock is on before the next access */

  if ((RCC_CR & RCC_CR_HSERDY) == 0u)
    return U2PS_DWC2_ERR_CLOCK;
  if ((PWR_VOSR & PWR_VOSR_VOS_MSK) < PWR_VOSR_VOS_RANGE2)
    return U2PS_DWC2_ERR_POWER; /* OTG_HS works in range 1 and range 2 only (10.6.3) */

  /* VDDUSB valid, then the OTG_HS supply and its booster; "The OTG_HS clock
   * can be provided only after" USBBOOSTRDY (10.10.4, p. 451) */
  PWR_SVMCR |= PWR_SVMCR_USV;
  PWR_VOSR |= PWR_VOSR_USBPWREN | PWR_VOSR_USBBOOSTEN;
  if (!u2ps_dwc2_wait_us(&PWR_VOSR, PWR_VOSR_USBBOOSTRDY, PWR_VOSR_USBBOOSTRDY, 10000u))
    return U2PS_DWC2_ERR_PHY;

  /* The PHY's kernel clock: HSE (11.8.47), then the clocks of OTG_HS and
   * its PHY (15.2.3 step 1) */
  RCC_CCIPR2 &= ~RCC_CCIPR2_OTGHSSEL_MSK;
  RCC_AHB2ENR1 |= RCC_AHB2ENR1_OTGEN | RCC_AHB2ENR1_OTGHSPHYEN;
  (void)RCC_AHB2ENR1;

  /* 15.2.3 steps 2-4: reference clock, tuning, enable */
  SYSCFG_OTGHSPHYCR = OTGHSPHYCR_CLKSEL(PHY_CLKSEL);
  SYSCFG_OTGHSPHYTUNER2 =
    (SYSCFG_OTGHSPHYTUNER2 & ~(TUNER2_SQRXTUNE_MSK | TUNER2_COMPDISTUNE_MSK)) |
    TUNER2_COMPDISTUNE_P5_9;
  SYSCFG_OTGHSPHYCR = OTGHSPHYCR_CLKSEL(PHY_CLKSEL) | OTGHSPHYCR_EN;
  /* No PHY lock flag is documented; the core reset that follows waits for
   * the PHY clock domain with a timeout (inferred) */
  return U2PS_DWC2_OK;
}

#elif defined(U2PS_CHIP_STM32F723)

#if U2PS_DWC2_HSE_HZ == 12000000
#define PHY_PLLSEL 0x0u
#elif U2PS_DWC2_HSE_HZ == 12500000
#define PHY_PLLSEL 0x1u
#elif U2PS_DWC2_HSE_HZ == 16000000
#define PHY_PLLSEL 0x3u
#elif U2PS_DWC2_HSE_HZ == 24000000
#define PHY_PLLSEL 0x4u
#elif U2PS_DWC2_HSE_HZ == 25000000
#define PHY_PLLSEL 0x5u
#else
#error "u2ps_dwc2_phy.c: U2PS_DWC2_HSE_HZ must be 12000000, 12500000, 16000000, 24000000 or 25000000 on the STM32F723, the references its USBPHYC PLL takes (RM0431 33.4.1): set -DU2PS_DWC2_HSE_HZ to the board's HSE, one of these"
#endif

/* USBPHYC_TUNE (RM0431 33.4.2): INCURREN (bit 0), INCURRINT (1),
 * HSDRVDCCUR (4), HSDRVRFRED (8), HSDRVCHKITRM[3:0] (12:9) = 0111 */
#define PHYC_TUNE_VALUE 0x00000F13u

/* RM0431 gives no order, start-up time or lock bit for the PHY; the order
 * here is: clocks, LDO and its ready bit, the PLL's input selection, the
 * PHY's tuning, the PLL's enable, then a 2 ms wait for the PLL
 * (inferred). */
u2ps_dwc2_err_t u2ps_dwc2_phy_start(void)
{
  if (!u2ps_dwc2_cycles_start())
    return U2PS_DWC2_ERR_CYCLES;
  if ((RCC_CR & RCC_CR_HSERDY) == 0u)
    return U2PS_DWC2_ERR_CLOCK;
  /* OTGHSULPIEN as well: the embedded HS PHY is the core's
   * "24 to 60 MHz OTG_HS_ULPI_CK clock source" (RM0431 Figure 14), gated by
   * this bit (RM0431 5.3.10); without it the core soft reset never ends */
  RCC_AHB1ENR |= RCC_AHB1ENR_OTGHSEN | RCC_AHB1ENR_OTGHSULPIEN;
  RCC_APB2ENR |= RCC_APB2ENR_OTGPHYCEN;
  (void)RCC_APB2ENR;

  USBPHYC_LDO |= LDO_ENABLE;
  if (!u2ps_dwc2_wait_us(&USBPHYC_LDO, LDO_STATUS, LDO_STATUS, 10000u))
    return U2PS_DWC2_ERR_PHY;
  USBPHYC_PLL1 = PLL1_SEL(PHY_PLLSEL);
  /* The HS PHY's tuning, OR-ed into USBPHYC_TUNE (RM0431 33.4.2):
   * INCURREN and INCURRINT (a current boost of 2 mA), HSDRVDCCUR (the HS
   * driver's DC level down by 5 to 7 mV), HSDRVRFRED (rise and fall time
   * +20 %) and HSDRVCHKITRM = 0111 (HS driver current +10.92 %, for choke
   * compensation). The value assumes the other bits as at reset: with the
   * reset value's LFSCAPEN (0x4) the register reads 0xF17, and a field
   * already set is OR-ed, not replaced. RM0431 recommends no value. 0xF13
   * is the default of ST's STM32F7 HAL V1.3.3 (stm32f7xx_ll_usb.c), read
   * there only to locate this hardware setting; nothing but the value is
   * taken from it. The trim compensates a common-mode choke on D+ and D-,
   * such as the STM32F723E-DISCO's ECMF02-4CMX8; on a board without one,
   * the HS high level may exceed USB 2.0's VHSOH maximum of 440 mV (Table
   * 7-7). Not measured. */
  USBPHYC_TUNE |= PHYC_TUNE_VALUE;
  USBPHYC_PLL1 = PLL1_SEL(PHY_PLLSEL) | PLL1_EN;
  u2ps_dwc2_delay_us(2000u);
  return U2PS_DWC2_OK;
}

#elif defined(U2PS_CHIP_STM32H7S3)

/* USBREFCKSEL for the HSE frequency (RM0477 7.8.24, p. 459), the U5's
 * CLKSEL codes */
#if U2PS_DWC2_REFSEL(U2PS_DWC2_HSE_HZ) == 0
#error "u2ps_dwc2_phy.c: U2PS_DWC2_HSE_HZ must be 16000000, 19200000, 20000000, 24000000, 26000000 or 32000000 on the STM32H7S3, the references its USB PHY takes (RM0477 7.8.24, USBREFCKSEL): set -DU2PS_DWC2_HSE_HZ to the board's HSE, one of these (24000000 on the NUCLEO-H7S3L8)"
#endif

/* USB33RDY within 10 ms (the U5's bound for its USB supply, inferred:
 * RM0477 6.4.7 gives no time), polled on PWR_CSR2's upper half-word: the
 * port never reads or writes the word, whose lower byte is write-once */
static bool usb33_ready(void)
{
  uint32_t t0 = DWT_CYCCNT;
  uint32_t limit = 10000u * U2PS_DWC2_CYCLES_PER_US;
  while ((PWR_CSR2_HI & CSR2_HI_USB33RDY) == 0u) {
    if (DWT_CYCCNT - t0 > limit)
      return (PWR_CSR2_HI & CSR2_HI_USB33RDY) != 0u;
  }
  return true;
}

/* RM0477 Rev 10. The app has written the supply configuration (the lower
 * byte of PWR_CSR2, once after power-on, Table 44 p. 286), set USB33DEN
 * and waited for USB33RDY before it configured PM0, PM1, PM8 or PM9
 * (6.8.4, p. 323; ES0596 2.2.19, p. 14), and started the HSE. The order:
 * - the cycle counter (u2ps_dwc2_cycles_start);
 * - HSERDY (7.8.1, p. 425);
 * - PWR_CSR2's upper half-word (6.8.4): USB33DEN, the VDD33USB level
 *   detector, "should be set if USB HS ... is used", and USBHSREGEN, the
 *   HS PHY regulator, "should only be set if USB HS is used"; USBREGEN, the
 *   USB regulator, stays as the board left it (the NUCLEO-H7S3L8 supplies
 *   VDD33USB from 3V3, MB1737 sheet 4); then USB33RDY, as ES0596 2.2.19
 *   orders it;
 * - the PHY's kernel clock: USBPHYCSEL 00 (hse_ker_ck) and USBREFCKSEL,
 *   changed only with USBPHYCEN 0 (7.5, p. 401), so USBPHYCEN is cleared
 *   first; then USBPHYCEN and OTGHSEN (7.8.45, p. 497), read back.
 * The PHY has no lock flag: the core reset that follows waits for the PHY
 * clock with a timeout (AHBIDL; inferred). */
u2ps_dwc2_err_t u2ps_dwc2_phy_start(void)
{
  if (!u2ps_dwc2_cycles_start())
    return U2PS_DWC2_ERR_CYCLES;
  if ((RCC_CR & RCC_CR_HSERDY) == 0u)
    return U2PS_DWC2_ERR_CLOCK;
  PWR_CSR2_HI = u2ps_dwc2_h7s3_csr2_hi(PWR_CSR2_HI);
  if (!usb33_ready())
    return U2PS_DWC2_ERR_PHY;
  RCC_AHB1ENR &= ~RCC_AHB1ENR_USBPHYCEN;
  (void)RCC_AHB1ENR; /* the clock is off before the selection changes */
  RCC_CCIPR1 = u2ps_dwc2_h7s3_ccipr1(RCC_CCIPR1, U2PS_DWC2_REFSEL(U2PS_DWC2_HSE_HZ));
  RCC_AHB1ENR |= RCC_AHB1ENR_USBPHYCEN | RCC_AHB1ENR_OTGHSEN;
  (void)RCC_AHB1ENR; /* the clocks are on before the next access */
  return U2PS_DWC2_OK;
}

#else
#error "u2ps_dwc2_phy.c: no PHY start-up for this chip: add one with its facts (u2ps_dwc2_chip.h)"
#endif
