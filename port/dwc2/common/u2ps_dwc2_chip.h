/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_chip.h: the facts of each chip the DWC2 ports support, in one
 * place: the OTG_HS interrupt, the base addresses, the core, and the
 * features and errata the ports' code branches on. Pure macros: the PC
 * tests build it once per chip (tests/test_dwc2_chip.c).
 *
 * The ARM build defines exactly one chip macro (port/dwc2/common/
 * CMakeLists.txt): U2PS_CHIP_STM32U5A5, U2PS_CHIP_STM32F723 or
 * U2PS_CHIP_STM32H7S3. Every chain over the chips ends with #error, here
 * and in the ports' code, so a chip without its facts never builds.
 *
 * Manuals: RM0456 Rev 7 (STM32U5); RM0431 (STM32F72x/F73x; HTML copy,
 * probably Rev 4, without page numbers); RM0477 Rev 10 (STM32H7Rx/7Sx),
 * DS14359 Rev 7 (STM32H7Sxx8) and ES0596 Rev 10 (STM32H7Rx/7Sx errata).
 *
 * The features:
 * - U2PS_DWC2_CM7: the core is a Cortex-M7, whose DWT may be locked: the
 *   port unlocks it and checks that its cycle counter counts
 *   (u2ps_dwc2_cycles_start; inferred from reports, not from a manual).
 * - U2PS_DWC2_DCACHE: a data cache lies on the path between the CPU and
 *   the RAM the OTG_HS DMA reaches, so the ports clean and invalidate DMA
 *   memory (u2ps_dwc2.h).
 * - U2PS_DWC2_GCCFG_OVR: OTG_GCCFG has the U5's VBUS override and the host
 *   pull-down bits (VBVALOVEN 24, VBVALOVAL 23, VBDEN 21, FORCEHOSTPD 25).
 * - U2PS_DWC2_F7_PHYSEL: the F723's PHY selection (GUSBCFG PHYSEL and
 *   ULPISEL, GCCFG PWRDWN and PHYHSEN), set before the core reset.
 * - U2PS_DWC2_ERRATUM_ISO_IN and U2PS_DWC2_ERRATUM_ZLP: the chip's errata
 *   sheet lists ES0596 2.21.1 (an isochronous IN endpoint disabled during
 *   a packet fetch sends the previous packet) and 2.21.3 (an unexpected
 *   packet instead of a zero-length packet), whose workarounds the device
 *   port does not have yet: a device port build for such a chip stops
 *   (u2ps_dwc2_dcd.c). Its two other OTG_HS items need no workaround in
 *   u2ps: 2.21.2 (a false OTG_GINTSTS.CIDSCHG after a reset), as neither
 *   port ever unmasks CIDSCHG (a static assert in each), and 2.21.4 (a
 *   false chirp-K if the port reset starts within 150 PHY clock cycles of
 *   a full-speed device's connect), as the host stack starts the reset
 *   only after the 100 ms connect debounce, TATTDB (u2ps_host.c). The
 *   errata sheets of the STM32U5 (ES0553 Rev 6,
 *   ES0595 Rev 4) and the STM32F723 (ES0360 Rev 5) list no OTG_HS item.
 * - U2PS_DWC2_DTCM_START, U2PS_DWC2_DTCM_END: the CPU's DTCM, which is not
 *   cached (none on the U5: an empty range).
 */
#ifndef U2PS_DWC2_CHIP_H
#define U2PS_DWC2_CHIP_H

#if defined(U2PS_CHIP_STM32U5A5) + defined(U2PS_CHIP_STM32F723) + defined(U2PS_CHIP_STM32H7S3) > 1
#error "u2ps_dwc2_chip.h: more than one of U2PS_CHIP_STM32U5A5, U2PS_CHIP_STM32F723 and U2PS_CHIP_STM32H7S3 is defined: the build defines one, from -DU2PS_CHIP; do not define them yourself"
#endif

#if defined(U2PS_CHIP_STM32U5A5)
/* The OTG_HS global interrupt, IRQ number 73 (RM0456 Table 186, p. 916) */
#define U2PS_DWC2_IRQ 73u
/* Nonsecure aliases, for a build with TrustZone disabled (TZEN = 0):
 * RM0456 2.3.2, Table 6, p. 145-146 */
#define U2PS_DWC2_OTG_BASE    0x42040000u
#define U2PS_DWC2_RCC_BASE    0x46020C00u
#define U2PS_DWC2_PWR_BASE    0x46020800u
#define U2PS_DWC2_SYSCFG_BASE 0x46000400u
#define U2PS_DWC2_FLASH_BASE  0x40022000u
/* The highest system clock, 160 MHz (RM0456 11.4.9, p. 498) */
#define U2PS_DWC2_CORE_HZ_MAX 160000000u
/* Cortex-M33 (RM0456 2.1); no data cache on the SRAM path (RM0456 9.3,
 * p. 383); the GCCFG bits of RM0456 73.14.18, p. 3347 */
#define U2PS_DWC2_CM7          0
#define U2PS_DWC2_DCACHE       0
#define U2PS_DWC2_GCCFG_OVR    1
#define U2PS_DWC2_F7_PHYSEL    0
#define U2PS_DWC2_ERRATUM_ISO_IN 0
#define U2PS_DWC2_ERRATUM_ZLP  0
#define U2PS_DWC2_DTCM_START   0u
#define U2PS_DWC2_DTCM_END     0u

#elif defined(U2PS_CHIP_STM32F723)
/* IRQ number 77, from ST's device header for the STM32F723
 * (stm32f723xx.h), since the local copy of RM0431 has no vector table */
#define U2PS_DWC2_IRQ 77u
/* The OTG_HS, RCC and USBPHYC addresses come from ST's device header for
 * the STM32F723 (stm32f723xx.h); the local copy of RM0431 has no memory
 * map */
#define U2PS_DWC2_OTG_BASE     0x40040000u
#define U2PS_DWC2_RCC_BASE     0x40023800u
#define U2PS_DWC2_USBPHYC_BASE 0x40017C00u
/* The highest system clock, 216 MHz (RM0431 5.2) */
#define U2PS_DWC2_CORE_HZ_MAX 216000000u
/* Cortex-M7 with its data cache (Arm facts that RM0431 does not state);
 * GCCFG without the U5's override bits (RM0431 32.15.15); the PHY
 * selection of RM0431 32.15.4 and 32.15.15 */
#define U2PS_DWC2_CM7          1
#define U2PS_DWC2_DCACHE       1
#define U2PS_DWC2_GCCFG_OVR    0
#define U2PS_DWC2_F7_PHYSEL    1
#define U2PS_DWC2_ERRATUM_ISO_IN 0
#define U2PS_DWC2_ERRATUM_ZLP  0
/* DTCM, 64 KB from 0x2000_0000 (RM0431 2.1) */
#define U2PS_DWC2_DTCM_START   0x20000000u
#define U2PS_DWC2_DTCM_END     0x20010000u

#elif defined(U2PS_CHIP_STM32H7S3)
/* The OTG_HS global interrupt, IRQ number 91, vector 0x1AC (RM0477
 * Table 142, p. 863) */
#define U2PS_DWC2_IRQ 91u
/* OTG_HS on AHB1 (RM0477 Table 7, p. 158); RCC and PWR (p. 154); FLASH
 * (p. 156) */
#define U2PS_DWC2_OTG_BASE    0x40040000u
#define U2PS_DWC2_RCC_BASE    0x58024400u
#define U2PS_DWC2_PWR_BASE    0x58024800u
#define U2PS_DWC2_FLASH_BASE  0x52002000u
/* The highest CPU clock, 600 MHz at VOS high (DS14359 Table 26, p. 159;
 * its note 9: with ECC on the AXI SRAM off) */
#define U2PS_DWC2_CORE_HZ_MAX 600000000u
/* Cortex-M7 with a 32 KB data cache (RM0477 Figure 1, p. 124), which
 * caches the AXI and AHB SRAMs by default (Table 6); OTG_GCCFG as the U5's
 * but for bit 16 (HCDPEN, pp. 3103-3104); no PHYSEL or ULPISEL (OTG_GUSBCFG
 * bits 6 and 4 reserved, p. 3080); ES0596 2.21.1 (p. 34) and 2.21.3 (pp.
 * 34-35) */
#define U2PS_DWC2_CM7          1
#define U2PS_DWC2_DCACHE       1
#define U2PS_DWC2_GCCFG_OVR    1
#define U2PS_DWC2_F7_PHYSEL    0
#define U2PS_DWC2_ERRATUM_ISO_IN 1
#define U2PS_DWC2_ERRATUM_ZLP  1
/* DTCM from 0x2000_0000, 64, 128 or 192 KB as FLASH_OBW2SR.DTCM_AXI_SHARE
 * says (RM0477 Table 13, p. 162): the largest; the rest of that span is
 * unmapped otherwise ("gives a TCM error") */
#define U2PS_DWC2_DTCM_START   0x20000000u
#define U2PS_DWC2_DTCM_END     0x20030000u

#else
#error "u2ps_dwc2_chip.h: none of U2PS_CHIP_STM32U5A5, U2PS_CHIP_STM32F723 and U2PS_CHIP_STM32H7S3 is defined: build the DWC2 port from an ARM build with -DU2PS_CHIP=stm32u5a5, stm32f723 or stm32h7s3, which defines it"
#endif

#endif /* U2PS_DWC2_CHIP_H */
