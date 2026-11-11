/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_regs.h: the registers and bits the DWC2 code uses, and only
 * those: OTG_HS from RM0456 Rev 7 chapter 73 (cited by section and page)
 * and RM0431 chapter 32 (cited by section), which RM0477 Rev 10 chapter 62
 * (STM32H7S3) matches but where noted; the Cortex-M debug registers; and
 * per chip the RCC, PWR, FLASH, SYSCFG and USBPHYC registers of the
 * start-up and the DMA memory rule. Included by the ports' sources only:
 * the names carry no prefix and stay out of the app.
 */
#ifndef U2PS_DWC2_REGS_H
#define U2PS_DWC2_REGS_H

#include <stdint.h>

#include "u2ps_dwc2_chip.h"

/* Register access, by words, and by half-words where a register needs it
 * (H7S3 PWR_CSR2). A PC test of the start-up defines both before this
 * header to reach a register model instead (tests/fake_regs.h). */
#ifndef U2PS_DWC2_R32
#define U2PS_DWC2_R32(addr) (*(volatile uint32_t *)(addr))
#endif
#ifndef U2PS_DWC2_R16
#define U2PS_DWC2_R16(addr) (*(volatile uint16_t *)(addr))
#endif
#define U2PS_OTG(off) U2PS_DWC2_R32(U2PS_DWC2_OTG_BASE + (uint32_t)(off))

/* ---- Cortex-M debug: the DWT cycle counter --------------------------------- */

/* Armv7-M and Armv8-M Architecture Reference Manuals, "DWT" and "DEMCR":
 * DEMCR.TRCENA (bit 24) enables the DWT, DWT_CTRL.CYCCNTENA (bit 0) the
 * counter. DWT_LAR, the CoreSight lock access register at DWT offset
 * 0xFB0 (CMSIS core_cm7.h, DWT_Type.LAR), which a Cortex-M7 has: writing
 * the key unlocks the DWT's registers for software (the key and the lock's
 * effect on CYCCNTENA are inferred from reports, not from a manual). */
#define DEMCR      U2PS_DWC2_R32(0xE000EDFCu)
#define DWT_CTRL   U2PS_DWC2_R32(0xE0001000u)
#define DWT_CYCCNT U2PS_DWC2_R32(0xE0001004u)
#define DWT_LAR    U2PS_DWC2_R32(0xE0001FB0u)
#define DEMCR_TRCENA       (1u << 24)
#define DWT_CTRL_CYCCNTENA (1u << 0)
#define DWT_LAR_KEY        0xC5ACCE55u

/* ---- System registers per chip ---------------------------------------------- */

#if defined(U2PS_CHIP_STM32U5A5)

/* RCC (RM0456 11.8) */
#define RCC_CR       U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x000u)
#define RCC_AHB1ENR  U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x088u)
#define RCC_AHB2ENR1 U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x08Cu)
#define RCC_AHB2ENR2 U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x090u)
#define RCC_AHB3ENR  U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x094u)
#define RCC_APB3ENR  U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x0A8u)
#define RCC_CCIPR2   U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x0E4u)
#define RCC_CR_HSERDY          (1u << 17)  /* 11.8.1 */
#define RCC_AHB2ENR1_OTGHSPHYEN (1u << 15) /* 11.8.30, p. 558 */
#define RCC_AHB2ENR1_OTGEN     (1u << 14)
#define RCC_AHB3ENR_PWREN      (1u << 2)   /* 11.8.32 */
#define RCC_APB3ENR_SYSCFGEN   (1u << 1)   /* 11.8.36 */
#define RCC_CCIPR2_OTGHSSEL_MSK (0x3u << 30) /* 11.8.47, p. 592: 00 = HSE */

/* FLASH_OPTR.TZEN (RM0456 7.9.13, p. 342) */
#define FLASH_OPTR      U2PS_DWC2_R32(U2PS_DWC2_FLASH_BASE + 0x40u)
#define FLASH_OPTR_TZEN (1u << 31)

/* PWR (RM0456 10.10) */
#define PWR_CR1   U2PS_DWC2_R32(U2PS_DWC2_PWR_BASE + 0x00u)
#define PWR_VOSR  U2PS_DWC2_R32(U2PS_DWC2_PWR_BASE + 0x0Cu)
#define PWR_SVMCR U2PS_DWC2_R32(U2PS_DWC2_PWR_BASE + 0x10u)
#define PWR_VOSR_USBBOOSTEN  (1u << 20) /* 10.10.4, p. 449-451 */
#define PWR_VOSR_USBPWREN    (1u << 19)
#define PWR_VOSR_VOS_MSK     (0x3u << 16)
#define PWR_VOSR_VOS_RANGE2  (0x2u << 16)
#define PWR_VOSR_USBBOOSTRDY (1u << 13)
#define PWR_SVMCR_USV        (1u << 28) /* 10.10.5, p. 452 */

/* SYSCFG (RM0456 15.3.12-13, p. 664-665) */
#define SYSCFG_OTGHSPHYCR     U2PS_DWC2_R32(U2PS_DWC2_SYSCFG_BASE + 0x74u)
#define SYSCFG_OTGHSPHYTUNER2 U2PS_DWC2_R32(U2PS_DWC2_SYSCFG_BASE + 0x7Cu)
#define OTGHSPHYCR_CLKSEL(v)  ((uint32_t)(v) << 2)
#define OTGHSPHYCR_EN         (1u << 0)
#define TUNER2_SQRXTUNE_MSK   (0x7u << 4)
#define TUNER2_COMPDISTUNE_MSK 0x7u
#define TUNER2_COMPDISTUNE_P5_9 0x2u /* +5.9 %, recommended */
/* SQRXTUNE 000: +15 %, recommended */

#elif defined(U2PS_CHIP_STM32F723)

/* RCC (RM0431 5.3) */
#define RCC_CR      U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x00u)
#define RCC_AHB1ENR U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x30u)
#define RCC_APB2ENR U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x44u)
#define RCC_CR_HSERDY          (1u << 17)
#define RCC_AHB1ENR_OTGHSEN    (1u << 29) /* 5.3.10 */
#define RCC_AHB1ENR_OTGHSULPIEN (1u << 30) /* 5.3.10 */
#define RCC_APB2ENR_OTGPHYCEN  (1u << 31) /* 5.3.14 */

/* USBPHYC (RM0431 33.4) */
#define USBPHYC_PLL1 U2PS_DWC2_R32(U2PS_DWC2_USBPHYC_BASE + 0x000u)
#define USBPHYC_LDO  U2PS_DWC2_R32(U2PS_DWC2_USBPHYC_BASE + 0x018u)
#define PLL1_SEL(v)        ((uint32_t)(v) << 1) /* 33.4.1 */
#define PLL1_EN            (1u << 0)
/* Bit 2 of USBPHYC_LDO is LDO_DISABLE, "0: LDO enabled", in RM0431
 * 33.4.3, but the register's reset value there, 0x1 with STATUS 0, does
 * not fit that. ST's CMSIS device header stm32f723xx.h (cmsis_device_f7)
 * names the same bit USB_HS_PHYC_LDO_ENABLE, so the LDO is enabled by
 * setting it (to be checked on the board: LDO_STATUS after the write). */
#define LDO_ENABLE         (1u << 2)
#define LDO_STATUS         (1u << 1)

#elif defined(U2PS_CHIP_STM32H7S3)

/* RCC (RM0477 7.8): RCC_CR (7.8.1, p. 424), RCC_CCIPR1 (7.8.24, p. 458),
 * RCC_AHB1ENR (7.8.45, p. 497), RCC_AHB2ENR (7.8.46, p. 499) */
#define RCC_CR      U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x000u)
#define RCC_CCIPR1  U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x04Cu)
#define RCC_AHB1ENR U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x138u)
#define RCC_AHB2ENR U2PS_DWC2_R32(U2PS_DWC2_RCC_BASE + 0x13Cu)
#define RCC_CR_HSERDY         (1u << 17) /* p. 425 */
#define RCC_AHB1ENR_USBPHYCEN (1u << 26) /* p. 497 */
#define RCC_AHB1ENR_OTGHSEN   (1u << 25)

/* PWR_CSR2 (RM0477 6.8.4, p. 323), reset by power-on only, its lower byte
 * write-once: only its upper half-word, at offset 0x00E, is ever accessed
 * (half-word access allowed, 6.8, p. 318). Bits of that half-word: USB33RDY
 * (bit 26, read-only). */
#define PWR_CSR2_HI U2PS_DWC2_R16(U2PS_DWC2_PWR_BASE + 0x00Eu)
#define CSR2_HI_USB33RDY (1u << (26 - 16))

/* FLASH_OBW2SR (RM0477 5.9.34, p. 273): ITCM_AXI_SHARE [2:0],
 * DTCM_AXI_SHARE [6:4], ECC_ON_SRAM 8, read-only */
#define FLASH_OBW2SR U2PS_DWC2_R32(U2PS_DWC2_FLASH_BASE + 0x268u)

#else
#error "u2ps_dwc2_regs.h: no system registers for this chip: add them with its facts (u2ps_dwc2_chip.h)"
#endif

/* ---- OTG_HS core global registers (RM0456 73.13.1, Table 766, p. 3314) ---- */

#define OTG_GOTGCTL  U2PS_OTG(0x000u)
#define OTG_GOTGINT  U2PS_OTG(0x004u)
#define OTG_GAHBCFG  U2PS_OTG(0x008u)
#define OTG_GUSBCFG  U2PS_OTG(0x00Cu)
#define OTG_GRSTCTL  U2PS_OTG(0x010u)
#define OTG_GINTSTS  U2PS_OTG(0x014u)
#define OTG_GINTMSK  U2PS_OTG(0x018u)
#define OTG_GRXFSIZ  U2PS_OTG(0x024u)
#define OTG_DIEPTXF0 U2PS_OTG(0x028u)
#define OTG_GCCFG    U2PS_OTG(0x038u)
/* x = 1..8 (RM0456 73.14.22, p. 3353) */
#define OTG_DIEPTXF(x) U2PS_OTG(0x104u + 4u * ((uint32_t)(x) - 1u))

/* OTG_GOTGCTL (RM0456 73.14.1, p. 3319-3320) */
#define GOTGCTL_BVALOVAL (1u << 7)
#define GOTGCTL_BVALOEN  (1u << 6)

/* OTG_GAHBCFG (RM0456 73.14.3, p. 3322-3323) */
#define GAHBCFG_DMAEN        (1u << 5)
#define GAHBCFG_HBSTLEN_MSK  (0xFu << 1)
#define GAHBCFG_HBSTLEN_INCR4 (0x3u << 1)
#define GAHBCFG_GINTMSK      (1u << 0)

/* OTG_GUSBCFG (RM0456 73.14.4, p. 3323-3325; RM0431 32.15.4 for PHYSEL and
 * ULPISEL, which the U5's register does not have) */
#define GUSBCFG_FDMOD    (1u << 30)
#define GUSBCFG_FHMOD    (1u << 29)
#define GUSBCFG_TRDT_MSK (0xFu << 10)
#define GUSBCFG_TRDT(v)  ((uint32_t)(v) << 10)
#define GUSBCFG_PHYSEL   (1u << 6)
#define GUSBCFG_ULPISEL  (1u << 4)

/* OTG_GRSTCTL (RM0456 73.14.5, p. 3325-3327) */
#define GRSTCTL_AHBIDL    (1u << 31)
#define GRSTCTL_TXFNUM(n) ((uint32_t)(n) << 6)
#define GRSTCTL_TXFNUM_ALL GRSTCTL_TXFNUM(0x10u)
#define GRSTCTL_TXFFLSH   (1u << 5)
#define GRSTCTL_RXFFLSH   (1u << 4)
#define GRSTCTL_CSRST     (1u << 0)

/* OTG_GINTSTS and OTG_GINTMSK, device mode (RM0456 73.14.7, p. 3332-3337,
 * and 73.14.9) */
#define GINT_WKUPINT      (1u << 31)
/* CIDSCHG, which ES0596 2.21.2 (p. 34) sets falsely after every reset on
 * the STM32H7S3: no port unmasks it, on any chip (a static assert in each
 * port) */
#define GINT_CIDSCHG      (1u << 28)
#define GINT_INCOMPISOOUT (1u << 21)
#define GINT_IISOIXFR     (1u << 20)
#define GINT_OEPINT       (1u << 19)
#define GINT_IEPINT       (1u << 18)
#define GINT_EOPF         (1u << 15)
#define GINT_ISOODRP      (1u << 14)
#define GINT_ENUMDNE      (1u << 13)
#define GINT_USBRST       (1u << 12)
#define GINT_USBSUSP      (1u << 11)
#define GINT_ESUSP        (1u << 10)
#define GINT_GONAKEFF     (1u << 7)
#define GINT_RXFLVL       (1u << 4)
#define GINT_OTGINT       (1u << 2)
#define GINT_MMIS         (1u << 1)
#define GINT_CMOD         (1u << 0)

/* OTG_GCCFG: VBUS B-session override (RM0456 73.14.18, p. 3347; the same
 * bits on the H7S3, RM0477 62.14.18, pp. 3103-3104); on the F723 the
 * power-down bit that selects the HS PHY (RM0431 32.15.15) */
#define GCCFG_VBVALOVEN (1u << 24)
#define GCCFG_VBVALOVAL (1u << 23)
#define GCCFG_VBDEN     (1u << 21)
#define GCCFG_PWRDWN    (1u << 16)
/* F723 only: the HS PHY enable, from ST's stm32f723xx.h; RM0431 32.15.15
 * marks the bit reserved (the U5's bit 23 is VBVALOVAL) */
#define GCCFG_PHYHSEN   (1u << 23)

/* ---- OTG_HS device-mode registers (RM0456 73.13.1, Table 768, p. 3316-3318) */

#define OTG_DCFG     U2PS_OTG(0x800u)
#define OTG_DCTL     U2PS_OTG(0x804u)
#define OTG_DSTS     U2PS_OTG(0x808u)
#define OTG_DIEPMSK  U2PS_OTG(0x810u)
#define OTG_DOEPMSK  U2PS_OTG(0x814u)
#define OTG_DAINT    U2PS_OTG(0x818u)
#define OTG_DAINTMSK U2PS_OTG(0x81Cu)
/* Endpoint n = 0..8 */
#define OTG_DIEPCTL(n)  U2PS_OTG(0x900u + 0x20u * (uint32_t)(n))
#define OTG_DIEPINT(n)  U2PS_OTG(0x908u + 0x20u * (uint32_t)(n))
#define OTG_DIEPTSIZ(n) U2PS_OTG(0x910u + 0x20u * (uint32_t)(n))
#define OTG_DIEPDMA(n)  U2PS_OTG(0x914u + 0x20u * (uint32_t)(n))
#define OTG_DOEPCTL(n)  U2PS_OTG(0xB00u + 0x20u * (uint32_t)(n))
#define OTG_DOEPINT(n)  U2PS_OTG(0xB08u + 0x20u * (uint32_t)(n))
#define OTG_DOEPTSIZ(n) U2PS_OTG(0xB10u + 0x20u * (uint32_t)(n))
#define OTG_DOEPDMA(n)  U2PS_OTG(0xB14u + 0x20u * (uint32_t)(n))

/* OTG_DCFG (RM0456 73.14.38, p. 3365-3367) */
#define DCFG_PFIVL_MSK  (0x3u << 11)
#define DCFG_PFIVL_95   (0x3u << 11)
#define DCFG_DAD_MSK    (0x7Fu << 4)
#define DCFG_DAD(a)     ((uint32_t)(a) << 4)
#define DCFG_NZLSOHSK   (1u << 2)
#define DCFG_DSPD_MSK   (0x3u << 0)
#define DCFG_DSPD_HS    (0x0u << 0)
#define DCFG_DSPD_FS    (0x1u << 0)

/* OTG_DCTL (RM0456 73.14.39, p. 3367-3368) */
#define DCTL_CGONAK   (1u << 10)
#define DCTL_SGONAK   (1u << 9)
#define DCTL_TCTL_MSK (0x7u << 4)
#define DCTL_TCTL(s)  ((uint32_t)(s) << 4)
#define DCTL_GONSTS   (1u << 3)
#define DCTL_SDIS     (1u << 1)

/* OTG_DSTS (RM0456 73.14.40, p. 3369) */
#define DSTS_FNSOF_POS 8u
#define DSTS_FNSOF_MSK (0x3FFFu << 8)
#define DSTS_ENUMSPD_MSK (0x3u << 1)

/* OTG_DIEPMSK, OTG_DIEPINTx (RM0456 73.14.41, p. 3370; 73.14.49, p. 3379) */
#define DIEPINT_INEPNE (1u << 6)
#define DIEPINT_ITTXFE (1u << 4)
#define DIEPINT_TOC    (1u << 3)
#define DIEPINT_AHBERR (1u << 2)
#define DIEPINT_EPDISD (1u << 1)
#define DIEPINT_XFRC   (1u << 0)

/* OTG_DOEPMSK, OTG_DOEPINTx (RM0456 73.14.42, p. 3371; 73.14.55, p. 3384) */
#define DOEPINT_STPKTRX (1u << 15)
#define DOEPINT_NAK     (1u << 13)
#define DOEPINT_B2BSTUP (1u << 6)
#define DOEPINT_STSPHSRX (1u << 5)
#define DOEPINT_STUP    (1u << 3)
#define DOEPINT_AHBERR  (1u << 2)
#define DOEPINT_EPDISD  (1u << 1)
#define DOEPINT_XFRC    (1u << 0)

/* OTG_DAINT, OTG_DAINTMSK (RM0456 73.14.43-44, p. 3372-3373) */
#define DAINT_IEP(n) (1u << (n))
#define DAINT_OEP(n) (1u << (16u + (n)))

/* OTG_DIEPCTLx and OTG_DOEPCTLx (RM0456 73.14.47-48, p. 3375-3378;
 * 73.14.54, p. 3382; 73.14.58-59, p. 3387-3391) */
#define EPCTL_EPENA      (1u << 31)
#define EPCTL_EPDIS      (1u << 30)
#define EPCTL_SODDFRM    (1u << 29)
#define EPCTL_SEVNFRM    (1u << 28)
#define EPCTL_SNAK       (1u << 27)
#define EPCTL_CNAK       (1u << 26)
#define EPCTL_TXFNUM(n)  ((uint32_t)(n) << 22)
#define EPCTL_STALL      (1u << 21)
#define EPCTL_EPTYP_ISO  (0x1u << 18)
#define EPCTL_EONUM      (1u << 16)
#define EPCTL_USBAEP     (1u << 15)
#define EPCTL_MPSIZ(m)   ((uint32_t)(m) & 0x7FFu)
/* EP0: MPSIZ[1:0] = 00 is 64 bytes (RM0456 73.14.54, p. 3382; RM0431
 * 32.15.50 for OTG_DIEPCTL0) */
#define EPCTL0_MPSIZ_64  0x0u

/* Transfer size registers (RM0456 73.14.50, 73.14.53, 73.14.56, 73.14.60) */
#define TSIZ_MCNT(n)      ((uint32_t)(n) << 29)
#define TSIZ_STUPCNT(n)   ((uint32_t)(n) << 29)
#define TSIZ_STUPCNT_POS  29u
#define TSIZ_RXDPID_POS   29u
#define TSIZ_PKTCNT(n)    ((uint32_t)(n) << 19)
#define TSIZ_PKTCNT_POS   19u
#define TSIZ_PKTCNT_MSK   (0x3FFu << 19)
#define TSIZ_XFRSIZ_MSK   0x7FFFFu
#define TSIZ0_XFRSIZ_MSK  0x7Fu

/* OTG_DSTS.FNSOF, the microframe number (RM0456 73.14.40, p. 3369) */
static inline uint32_t u2ps_dwc2_fnsof(void)
{
  return (OTG_DSTS & DSTS_FNSOF_MSK) >> DSTS_FNSOF_POS;
}

/* ---- OTG_HS host-mode registers (RM0456 73.13.1, Tables 766 and 767,
 * pp. 3314-3316; RM0431 32.14, the same offsets) ------------------------------- */

/* OTG_HNPTXFSIZ shares its offset with OTG_DIEPTXF0 (73.14.15, p. 3345) */
#define OTG_HNPTXFSIZ U2PS_OTG(0x028u)
#define OTG_HPTXFSIZ  U2PS_OTG(0x100u)
#define OTG_HCFG      U2PS_OTG(0x400u)
#define OTG_HFNUM     U2PS_OTG(0x408u)
#define OTG_HAINT     U2PS_OTG(0x414u)
#define OTG_HAINTMSK  U2PS_OTG(0x418u)
#define OTG_HPRT      U2PS_OTG(0x440u)
/* Channel n = 0..15 (73.14.31-36, pp. 3360-3365) */
#define OTG_HCCHAR(n)   U2PS_OTG(0x500u + 0x20u * (uint32_t)(n))
#define OTG_HCINT(n)    U2PS_OTG(0x508u + 0x20u * (uint32_t)(n))
#define OTG_HCINTMSK(n) U2PS_OTG(0x50Cu + 0x20u * (uint32_t)(n))
#define OTG_HCTSIZ(n)   U2PS_OTG(0x510u + 0x20u * (uint32_t)(n))
#define OTG_HCDMA(n)    U2PS_OTG(0x514u + 0x20u * (uint32_t)(n))

/* OTG_GOTGCTL, host mode (73.14.1, pp. 3320-3321): the embedded host
 * state machine instead of the OTG A-device's, and the A-session and VBUS
 * valid overrides */
#define GOTGCTL_EHEN      (1u << 12)
#define GOTGCTL_AVALOVAL  (1u << 5)
#define GOTGCTL_AVALOEN   (1u << 4)
#define GOTGCTL_VBVALOVAL (1u << 3)
#define GOTGCTL_VBVALOEN  (1u << 2)

/* OTG_GINTSTS and OTG_GINTMSK, host mode (73.14.6, pp. 3328-3332; 73.14.8,
 * p. 3337); WKUPINT, OTGINT, MMIS and CMOD as above */
#define GINT_DISCINT (1u << 29)
#define GINT_HCINT   (1u << 25)
#define GINT_HPRTINT (1u << 24)
#define GINT_IPXFR   (1u << 21)
#define GINT_SOF     (1u << 3)

/* OTG_GCCFG.FORCEHOSTPD, the host pull-downs without an ID pin (73.14.18,
 * p. 3347); RM0431 32.15.15 has no such bit: the F723 sets the pull-downs in
 * host mode by itself (RM0431 32.7) */
#define GCCFG_FORCEHOSTPD (1u << 25)

/* OTG_HCCHARx (73.14.31, p. 3360) */
#define HCCHAR_CHENA  (1u << 31)
#define HCCHAR_CHDIS  (1u << 30)
#define HCCHAR_ODDFRM (1u << 29)

/* OTG_HFNUM.FRNUM, the (micro)frame number (73.14.26, p. 3355) */
static inline uint32_t u2ps_dwc2_hfnum(void)
{
  return OTG_HFNUM & 0xFFFFu;
}

#endif /* U2PS_DWC2_REGS_H */
