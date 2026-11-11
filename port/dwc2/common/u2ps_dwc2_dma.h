/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_dma.h: the DWC2 ports' DMA memory rule, as pure logic over
 * an address range and the register values that keep its RAM mapped,
 * clocked and powered, for the STM32U5, the STM32F723 and the STM32H7S3,
 * and the same rule as ranges of memory, for the host stack's contract
 * check of each queued packet (u2ps_hport_dma_ranges). It builds on the
 * PC too, for the tests; u2ps_dwc2_dma_ok() and u2ps_dwc2_dma_ranges()
 * (u2ps_dwc2.h) read the registers on the chip and apply it.
 *
 * Manuals: RM0456 Rev 7 (STM32U5), RM0431 (STM32F72x/F73x; HTML copy,
 * probably Rev 4, without page numbers), RM0477 Rev 10 (STM32H7Rx/7Sx).
 */
#ifndef U2PS_DWC2_DMA_H
#define U2PS_DWC2_DMA_H

#include <stdbool.h>
#include <stdint.h>

/* A range of memory the DMA reaches, [start, end) */
typedef struct {
  uintptr_t start;
  uintptr_t end;
} u2ps_dwc2_range_t;

/* The most ranges a rule gives: 3 on the U5 (SRAMs on, off, on, off, on)
 * and on the H7S3 (SRAM1 to SRAM3, SRAM4 after a gap, the AHB SRAMs) */
#define U2PS_DWC2_RANGES_MAX 4u

/* True if every byte of [addr, addr + len) lies in one of the n ranges,
 * without a wrap of the address; a len of 0 needs addr within one, its
 * end included */
bool u2ps_dwc2_in_ranges(uintptr_t addr, uint32_t len, const u2ps_dwc2_range_t *r, unsigned n);

/* STM32U5: the registers whose bits keep an SRAM clocked and powered */
typedef struct {
  uint32_t ahb1enr;  /* RCC_AHB1ENR: SRAM1EN, bit 31 (11.8.29, p. 553) */
  uint32_t ahb2enr1; /* RCC_AHB2ENR1: SRAM3EN 31, SRAM2EN 30 (11.8.30, p. 556) */
  uint32_t ahb2enr2; /* RCC_AHB2ENR2: SRAM5EN 31, SRAM6EN 30 (11.8.31, p. 560) */
  uint32_t pwr_cr1;  /* PWR_CR1: SRAM6PD 13, SRAM5PD 12, SRAM3PD 10, SRAM2PD 9,
                        SRAM1PD 8 (10.10.1, pp. 443-444) */
} u2ps_dwc2_u5_ram_t;

/* STM32U5: true if every byte of [addr, addr + len) lies in SRAM1, SRAM2,
 * SRAM3, SRAM5 or SRAM6 at its 0x2000_0000 address, the SRAMs the OTG_HS
 * bus reaches (RM0456 2.1.6, p. 132), and each SRAM it touches has its
 * clock enabled and is not powered down, as at reset (RM0456 2.1.12, p.
 * 133: the SRAMs' clocks are on after reset; PWR_CR1 resets to 0). The
 * SRAMs follow each other from 0x2000_0000 on the STM32U59x/5Ax/5Fx/5Gx
 * (RM0456 2.3.3, Table 7, p. 149; Figures 5 and 6): SRAM1 768 KB, SRAM2
 * 64 KB, SRAM3 832 KB, SRAM5 832 KB, SRAM6 512 KB (U5Fx/5Gx only; on the
 * U5Ax, SRAM6EN is 0 at reset, so a range there is refused). Never SRAM4
 * (0x2800_0000) or the backup SRAM, which the OTG_HS bus does not reach.
 * A len of 0 needs addr within the SRAMs' span. */
bool u2ps_dwc2_u5_dma_ok(uintptr_t addr, uint32_t len, const u2ps_dwc2_u5_ram_t *ram);

/* STM32U5: the SRAMs of u2ps_dwc2_u5_dma_ok() that are clocked and
 * powered, adjacent ones joined, into r (U2PS_DWC2_RANGES_MAX); returns
 * how many */
unsigned u2ps_dwc2_u5_dma_ranges(const u2ps_dwc2_u5_ram_t *ram, u2ps_dwc2_range_t *r);

/* STM32F723: true if every byte of [addr, addr + len) lies in DTCM (64 KB
 * from 0x2000_0000), SRAM1 (176 KB) or SRAM2 (16 KB) after it, the
 * memories the OTG_HS DMA bus reaches (RM0431 2.1.10; 2.1.6: DTCM through
 * the CPU's AHBS), never ITCM RAM (2.1.6: not on AHBS), and, if it touches
 * DTCM, RCC_AHB1ENR.DTCMRAMEN (bit 20; 5.3.10, set at reset) is set in
 * ahb1enr. SRAM1 and SRAM2 have no clock enable of their own in Run mode
 * (5.3.10; their *LPEN bits act in Sleep mode only, 5.3.15). A len of 0
 * needs addr within the span. */
bool u2ps_dwc2_f723_dma_ok(uintptr_t addr, uint32_t len, uint32_t ahb1enr);

/* STM32F723: the memories of u2ps_dwc2_f723_dma_ok() as one range: from
 * DTCM if DTCMRAMEN is set, else from SRAM1, to the end of SRAM2 */
unsigned u2ps_dwc2_f723_dma_ranges(uint32_t ahb1enr, u2ps_dwc2_range_t *r);

/* STM32H7S3: the RAMs the OTG_HS DMA reaches (RM0477 Table 2, p. 123):
 * the AXI SRAMs and the AHB SRAMs; never ITCM, DTCM or the backup SRAM;
 * flash, XSPI and FMC are reached but are not RAM. The AXI SRAMs as the
 * option bytes map them, from FLASH_OBW2SR (5.9.34, p. 273):
 * - AXI SRAM1 by ITCM_AXI_SHARE [2:0] (Table 12, p. 162): 000 and 011
 *   0x2400_0000-0x2401_FFFF, 001 0x2401_0000-0x2401_FFFF, 010 none;
 * - AXI SRAM2 0x2402_0000-0x2403_FFFF, always (2.4, p. 161);
 * - AXI SRAM3 by DTCM_AXI_SHARE [6:4] (Table 13): 000 and 011
 *   0x2404_0000-0x2405_FFFF, 001 0x2404_0000-0x2404_FFFF, 010 none;
 * - AXI SRAM4 0x2406_0000-0x2407_1FFF only with ECC_ON_SRAM (bit 8) 0
 *   (Table 14); the factory value is 1 (Table 32, p. 227).
 * The share values with bit 2 set are undefined ("Bit 2 should be kept to
 * 0", 5.9.35, p. 274): they refuse that field's SRAM. The AHB SRAMs,
 * SRAM1 0x3000_0000-0x3000_3FFF and SRAM2 0x3000_4000-0x3000_7FFF (Figure
 * 3, p. 151), each only with its clock enable in RCC_AHB2ENR, SRAM1EN
 * (bit 29) and SRAM2EN (bit 30), both 0 at reset (7.8.46, p. 499). True if
 * every byte of [addr, addr + len) lies in such RAM, across adjacent ones
 * too; a len of 0 needs addr within one. */
bool u2ps_dwc2_h7s3_dma_ok(uintptr_t addr, uint32_t len, uint32_t obw2sr, uint32_t ahb2enr);

/* STM32H7S3: that RAM as ranges into r (U2PS_DWC2_RANGES_MAX); returns
 * how many. u2ps_dwc2_h7s3_dma_ok() is u2ps_dwc2_in_ranges() over them. */
unsigned u2ps_dwc2_h7s3_dma_ranges(uint32_t obw2sr, uint32_t ahb2enr, u2ps_dwc2_range_t *r);

#endif /* U2PS_DWC2_DMA_H */
