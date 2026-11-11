/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2.h: DWC2 code that the device and host ports share, ARM builds
 * only: core reset, PHY start-up for the STM32U5, the STM32F723 and the
 * STM32H7S3, FIFO RAM set-up, FIFO flushes, the vector's NVIC mask, data
 * cache maintenance and the DMA memory rule. The chip's facts are in
 * u2ps_dwc2_chip.h, the registers in u2ps_dwc2_regs.h, which only the
 * ports include.
 *
 * Manuals: RM0456 Rev 7 (STM32U5), chapters 2, 10, 11, 15 and 73, cited by
 * section and page; RM0431 (STM32F72x/F73x; HTML copy, probably Rev 4,
 * without page numbers), chapters 5, 32 and 33, cited by section; RM0477
 * Rev 10 (STM32H7Rx/7Sx), chapters 2, 5, 6, 7 and 62, by section and page.
 * The OTG registers have the same offsets and the same bits in all three,
 * except where noted. OTG registers are accessed by words only (RM0456
 * 73.13, p. 3314).
 *
 * The build defines the chip, U2PS_CHIP_STM32U5A5, U2PS_CHIP_STM32F723 or
 * U2PS_CHIP_STM32H7S3, and U2PS_DWC2_HSE_HZ, the HSE frequency the PHY
 * takes its reference from; the app has started the HSE before
 * u2ps_dport_init() or u2ps_hport_init().
 */
#ifndef U2PS_DWC2_H
#define U2PS_DWC2_H

#include <stdbool.h>
#include <stdint.h>

#include "u2ps_dwc2_chip.h"
#include "u2ps_dwc2_dma.h"
#include "u2ps_dwc2_start.h"
#include "u2ps_types.h"

/* U2PS_DWC2_IRQ (u2ps_dwc2_chip.h) is the OTG_HS interrupt's IRQ number,
 * the number NVIC_SetPriority() and NVIC_EnableIRQ() take (its position in
 * the vector table, the exception number minus 16), not a vector function:
 * 73 on the U5, 77 on the F723, 91 on the H7S3 */

/* ---- Shared functions ------------------------------------------------------- */

/* Time is counted in core cycles of the DWT cycle counter, which
 * u2ps_dwc2_phy_start() starts. The build defines U2PS_DWC2_CORE_HZ,
 * the highest core clock the app runs at (160 MHz on the U5A5, 216 MHz on
 * the F723, 600 MHz on the H7S3 by default; above 30 MHz, the least AHB
 * clock of OTG_HS, and at most the chip's highest, U2PS_DWC2_CORE_HZ_MAX,
 * as u2ps_dwc2.c checks); at a lower clock every bound below is longer,
 * never shorter. */
#define U2PS_DWC2_CYCLES_PER_US (U2PS_DWC2_CORE_HZ / 1000000u)

/* The bound of each wait of a FIFO flush: a few PHY clocks are expected
 * (inferred), so 100 us, which a handler can afford (0.8 microframes) */
#define U2PS_DWC2_FLUSH_US 100u

/* Starts the DWT cycle counter (DEMCR.TRCENA, DWT_CTRL.CYCCNTENA). On a
 * Cortex-M7 (U2PS_DWC2_CM7) it unlocks the DWT first (DWT_LAR) and
 * returns false if the counter then does not advance within 16 reads; on
 * the U5 it returns true. */
bool u2ps_dwc2_cycles_start(void);

/* DWT_CYCCNT (Armv7-M and Armv8-M Architecture Reference Manuals, "DWT"),
 * inline: the packet path reads it */
static inline uint32_t u2ps_dwc2_cycles(void)
{
  return *(volatile uint32_t *)0xE0001004u;
}

/* Spins until (reg & mask) == value, for at most 'us' microseconds.
 * Returns false on the timeout. */
bool u2ps_dwc2_wait_us(volatile uint32_t *reg, uint32_t mask, uint32_t value, uint32_t us);

/* Spins for at least 'us' microseconds */
void u2ps_dwc2_delay_us(uint32_t us);

/* Why the controller did not start: u2ps_dwc2_err_t (u2ps_dwc2_start.h) */

/* Starts the DWT cycle counter, powers the PHY and starts its clocks (U5:
 * PWR, RCC, SYSCFG; F723: RCC and USBPHYC; H7S3: PWR_CSR2's upper
 * half-word and RCC). Returns the reason if a wait timed out or a
 * precondition (the cycle counter counting, HSE running, voltage range,
 * TrustZone off) does not hold (u2ps_dwc2_phy.c). */
u2ps_dwc2_err_t u2ps_dwc2_phy_start(void);

/* Core soft reset: on the F723 (U2PS_DWC2_F7_PHYSEL) selects and enables
 * the internal HS PHY first; sets SDIS, which the reset keeps; waits for AHBIDL, sets CSRST,
 * waits for it to clear and for AHBIDL again (RM0456 73.14.5, p. 3327).
 * Returns false on a timeout. */
bool u2ps_dwc2_core_reset(void);

/* Flushes Tx FIFO 'fifo' (0..15, or 0x10 for all), each wait bounded by
 * U2PS_DWC2_FLUSH_US, or the Rx FIFO (bounded by 6 ms in all), each after
 * AHBIDL (RM0456 73.14.5, p. 3326). */
bool u2ps_dwc2_flush_tx(uint32_t fifo);
bool u2ps_dwc2_flush_rx(void);

/* The FIFO RAM for EP0, one isochronous OUT endpoint of out_mps bytes and
 * the feedback endpoint of fb_mps bytes on Tx FIFO 1 (u2ps_dwc2_fifo.h),
 * written to OTG_GRXFSIZ, OTG_DIEPTXF0 and OTG_DIEPTXF1, then every FIFO
 * flushed. Returns false if the plan exceeds the RAM or a flush timed
 * out. */
bool u2ps_dwc2_fifo_setup(uint16_t out_mps, uint16_t fb_mps);

/* The OTG_HS vector in the NVIC: masked (true) or unmasked (false). The
 * mask is followed by DSB and ISB, so no handler runs and none has a write
 * in flight when it returns; the unmask by a compiler barrier first. NVIC
 * ICER and ISER: Armv7-M and Armv8-M Architecture Reference Manuals,
 * "NVIC". */
void u2ps_dwc2_irq_mask(bool masked);

/* Data cache maintenance by address for DMA memory, whole 32-byte lines,
 * inline: the packet path calls them twice per packet. Empty without a
 * data cache on the DMA's RAM (U2PS_DWC2_DCACHE 0: the U5, RM0456 9.3,
 * p. 383). With one (the F723 and the H7S3, Cortex-M7) they do nothing
 * when the data cache is off (CCR.DC clear) or the address lies in DTCM,
 * which is not cached (U2PS_DWC2_DTCM_START, U2PS_DWC2_DTCM_END; Cortex-M7
 * facts that RM0431 does not state); otherwise u2ps_dwc2_dcache_lines()
 * writes the address of each line to the operation's register: DCCMVAC
 * (clean), DCIMVAC (invalidate) or DCCIMVAC (both), in the System Control
 * Space (Armv7-M Architecture Reference Manual). */
#if U2PS_DWC2_DCACHE == 1
#define U2PS_DWC2_SCB_CCR      0xE000ED14u
#define U2PS_DWC2_SCB_CCR_DC   (1u << 16)
#define U2PS_DWC2_SCB_DCIMVAC  0xE000EF5Cu
#define U2PS_DWC2_SCB_DCCMVAC  0xE000EF68u
#define U2PS_DWC2_SCB_DCCIMVAC 0xE000EF70u

void u2ps_dwc2_dcache_lines(uint32_t op, uintptr_t addr, uint32_t len);

static inline bool u2ps_dwc2_dcache_on(uintptr_t a)
{
  return (*(volatile uint32_t *)U2PS_DWC2_SCB_CCR & U2PS_DWC2_SCB_CCR_DC) != 0u &&
         !(a >= U2PS_DWC2_DTCM_START && a < U2PS_DWC2_DTCM_END);
}

static inline void u2ps_dwc2_dcache_clean(const void *addr, uint32_t len)
{
  if (u2ps_dwc2_dcache_on((uintptr_t)addr))
    u2ps_dwc2_dcache_lines(U2PS_DWC2_SCB_DCCMVAC, (uintptr_t)addr, len);
}

static inline void u2ps_dwc2_dcache_invalidate(void *addr, uint32_t len)
{
  if (u2ps_dwc2_dcache_on((uintptr_t)addr))
    u2ps_dwc2_dcache_lines(U2PS_DWC2_SCB_DCIMVAC, (uintptr_t)addr, len);
}

static inline void u2ps_dwc2_dcache_clean_invalidate(void *addr, uint32_t len)
{
  if (u2ps_dwc2_dcache_on((uintptr_t)addr))
    u2ps_dwc2_dcache_lines(U2PS_DWC2_SCB_DCCIMVAC, (uintptr_t)addr, len);
}
#elif U2PS_DWC2_DCACHE == 0
static inline void u2ps_dwc2_dcache_clean(const void *addr, uint32_t len)
{
  (void)addr;
  (void)len;
}

static inline void u2ps_dwc2_dcache_invalidate(void *addr, uint32_t len)
{
  (void)addr;
  (void)len;
}

static inline void u2ps_dwc2_dcache_clean_invalidate(void *addr, uint32_t len)
{
  (void)addr;
  (void)len;
}
#else
#error "u2ps_dwc2.h: U2PS_DWC2_DCACHE must be 0 or 1 (u2ps_dwc2_chip.h)"
#endif

/* True if 'len' bytes at addr lie in memory the OTG_HS DMA reaches and the
 * port uses as DMA memory, with that memory clocked and powered: the rule
 * of u2ps_dwc2_dma.h over the registers' values now. On the U5 SRAM1,
 * SRAM2, SRAM3, SRAM5 and SRAM6 at their 0x2000_0000 addresses, never
 * SRAM4 or BKPSRAM (RM0456 2.1.6, p. 132; 2.3.3, Table 7), each with its
 * RCC clock enable set and its PWR_CR1.SRAMxPD clear (read only while the
 * PWR clock runs); on the F723 DTCM, with RCC_AHB1ENR.DTCMRAMEN set, SRAM1
 * and SRAM2 (RM0431 2.1.6, 2.1.10, 5.3.10); on the H7S3 the AXI SRAMs as
 * the option bytes map them (FLASH_OBW2SR) and the AHB SRAMs with their
 * clocks on (RCC_AHB2ENR), never DTCM, ITCM, the backup SRAM or flash
 * (RM0477 Table 2, p. 123; Tables 12-14, p. 162). Any context: it only
 * reads. The ports apply it to class memory at a stream's open and,
 * through u2ps_dport_dma_check() and u2ps_hport_dma_check(), to the
 * stacks' DMA memory and their own at every init. */
bool u2ps_dwc2_dma_ok(const void *addr, uint32_t len);

/* The same rule as ranges of memory, in address order and none touching
 * the next, at most U2PS_DWC2_RANGES_MAX, written to r; returns how many.
 * Thread, as u2ps_dwc2_pwr_clock() for the U5 (the host port's
 * u2ps_hport_dma_ranges() at init). */
unsigned u2ps_dwc2_dma_ranges(u2ps_dwc2_range_t *r);

/* U5: enables the PWR clock (RCC_AHB3ENR.PWREN, RM0456 11.8.32), which
 * reading PWR_CR1 needs and the PHY's start enables too; F723 and H7S3:
 * nothing (no register of their rule needs a clock of its own).
 * Thread, before the port's start (a read-modify-write of RCC). */
void u2ps_dwc2_pwr_clock(void);

#endif /* U2PS_DWC2_H */
