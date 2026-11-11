/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_dma.c: the DWC2 ports' DMA memory rule (u2ps_dwc2_dma.h), pure
 * logic: each chip's memories the OTG_HS DMA reaches, and the bits that
 * keep them mapped, clocked and powered; and the same as ranges.
 */
#include "u2ps_dwc2_dma.h"

/* One RAM of the rule: [start, end), and the bits that must be set (en,
 * in register reg of u2ps_dwc2_u5_ram_t) and clear (pd, in PWR_CR1) while
 * a range touches it */
typedef struct {
  uint32_t start, end;
  uint8_t reg; /* 0 RCC_AHB1ENR, 1 RCC_AHB2ENR1, 2 RCC_AHB2ENR2 */
  uint32_t en, pd;
} ram_t;

/* STM32U5 (u2ps_dwc2_dma.h): RM0456 2.3.3, Table 7, p. 149; 11.8.29-11.8.31;
 * 10.10.1 */
static const ram_t u5_ram[] = {
  {0x20000000u, 0x200C0000u, 0u, 1u << 31, 1u << 8},  /* SRAM1, 768 KB */
  {0x200C0000u, 0x200D0000u, 1u, 1u << 30, 1u << 9},  /* SRAM2, 64 KB */
  {0x200D0000u, 0x201A0000u, 1u, 1u << 31, 1u << 10}, /* SRAM3, 832 KB */
  {0x201A0000u, 0x20270000u, 2u, 1u << 31, 1u << 12}, /* SRAM5, 832 KB */
  {0x20270000u, 0x202F0000u, 2u, 1u << 30, 1u << 13}, /* SRAM6, 512 KB */
};

/* [addr, addr + len) within [start, end), without a wrap of the address;
 * *last is the range's end */
static bool within(uintptr_t addr, uint32_t len, uintptr_t start, uintptr_t end, uintptr_t *last)
{
  *last = addr + len;
  return addr >= start && *last >= addr && *last <= end;
}

bool u2ps_dwc2_in_ranges(uintptr_t addr, uint32_t len, const u2ps_dwc2_range_t *r, unsigned n)
{
  uintptr_t last;
  for (unsigned i = 0; i < n; i++)
    if (within(addr, len, r[i].start, r[i].end, &last))
      return true;
  return false;
}

/* Appends [start, end) to the n ranges of r, joined to the last one if it
 * ends there; returns the new count */
static unsigned add_range(u2ps_dwc2_range_t *r, unsigned n, uintptr_t start, uintptr_t end)
{
  if (n > 0u && r[n - 1u].end == start) {
    r[n - 1u].end = end;
    return n;
  }
  r[n].start = start;
  r[n].end = end;
  return n + 1u;
}

bool u2ps_dwc2_u5_dma_ok(uintptr_t addr, uint32_t len, const u2ps_dwc2_u5_ram_t *ram)
{
  uintptr_t last;
  if (!within(addr, len, u5_ram[0].start, u5_ram[4].end, &last))
    return false;
  const uint32_t reg[3] = {ram->ahb1enr, ram->ahb2enr1, ram->ahb2enr2};
  for (unsigned i = 0; i < sizeof u5_ram / sizeof u5_ram[0]; i++) {
    const ram_t *r = &u5_ram[i];
    bool touched = addr < r->end && last > r->start;
    if (touched && ((reg[r->reg] & r->en) == 0u || (ram->pwr_cr1 & r->pd) != 0u))
      return false;
  }
  return true;
}

unsigned u2ps_dwc2_u5_dma_ranges(const u2ps_dwc2_u5_ram_t *ram, u2ps_dwc2_range_t *r)
{
  const uint32_t reg[3] = {ram->ahb1enr, ram->ahb2enr1, ram->ahb2enr2};
  unsigned n = 0;
  for (unsigned i = 0; i < sizeof u5_ram / sizeof u5_ram[0]; i++) {
    const ram_t *m = &u5_ram[i];
    if ((reg[m->reg] & m->en) != 0u && (ram->pwr_cr1 & m->pd) == 0u)
      n = add_range(r, n, m->start, m->end);
  }
  return n;
}

/* STM32F723 (u2ps_dwc2_dma.h): DTCM 64 KB, then SRAM1 176 KB and SRAM2
 * 16 KB, from 0x2000_0000 (RM0431 2.1, 2.1.6, 2.1.10) */
#define F723_START     0x20000000u
#define F723_DTCM_END  0x20010000u
#define F723_END       0x20040000u
#define F723_DTCMRAMEN (1u << 20) /* RCC_AHB1ENR, RM0431 5.3.10 */

bool u2ps_dwc2_f723_dma_ok(uintptr_t addr, uint32_t len, uint32_t ahb1enr)
{
  uintptr_t last;
  if (!within(addr, len, F723_START, F723_END, &last))
    return false;
  return addr >= F723_DTCM_END || (ahb1enr & F723_DTCMRAMEN) != 0u;
}

unsigned u2ps_dwc2_f723_dma_ranges(uint32_t ahb1enr, u2ps_dwc2_range_t *r)
{
  r[0].start = (ahb1enr & F723_DTCMRAMEN) != 0u ? F723_START : F723_DTCM_END;
  r[0].end = F723_END;
  return 1;
}

/* STM32H7S3 (u2ps_dwc2_dma.h): RM0477 Tables 12-14, p. 162; Figure 3,
 * p. 151; FLASH_OBW2SR 5.9.34, p. 273; RCC_AHB2ENR 7.8.46, p. 499 */
#define H7_SRAM1     0x24000000u
#define H7_SRAM1_HI  0x24010000u
#define H7_SRAM2     0x24020000u
#define H7_SRAM3     0x24040000u
#define H7_SRAM3_HI  0x24050000u
#define H7_SRAM4     0x24060000u
#define H7_SRAM4_END 0x24072000u
#define H7_AHB1      0x30000000u
#define H7_AHB2      0x30004000u
#define H7_AHB_END   0x30008000u
#define H7_ITCM_SHARE(o) ((o) & 0x7u)
#define H7_DTCM_SHARE(o) (((o) >> 4) & 0x7u)
#define H7_ECC_ON_SRAM   (1u << 8)
#define H7_SRAM1EN       (1u << 29)
#define H7_SRAM2EN       (1u << 30)

unsigned u2ps_dwc2_h7s3_dma_ranges(uint32_t obw2sr, uint32_t ahb2enr, u2ps_dwc2_range_t *r)
{
  unsigned n = 0;
  /* AXI SRAM1, the part ITCM leaves: 000/011 all, 001 the upper half */
  uint32_t itcm = H7_ITCM_SHARE(obw2sr);
  if (itcm == 0u || itcm == 3u)
    n = add_range(r, n, H7_SRAM1, H7_SRAM2);
  else if (itcm == 1u)
    n = add_range(r, n, H7_SRAM1_HI, H7_SRAM2);
  n = add_range(r, n, H7_SRAM2, H7_SRAM3);
  /* AXI SRAM3, the part DTCM leaves: 000/011 all, 001 the lower half */
  uint32_t dtcm = H7_DTCM_SHARE(obw2sr);
  if (dtcm == 0u || dtcm == 3u)
    n = add_range(r, n, H7_SRAM3, H7_SRAM4);
  else if (dtcm == 1u)
    n = add_range(r, n, H7_SRAM3, H7_SRAM3_HI);
  /* AXI SRAM4, when the ECC does not take it */
  if ((obw2sr & H7_ECC_ON_SRAM) == 0u)
    n = add_range(r, n, H7_SRAM4, H7_SRAM4_END);
  /* The AHB SRAMs with their clocks on */
  if ((ahb2enr & H7_SRAM1EN) != 0u)
    n = add_range(r, n, H7_AHB1, H7_AHB2);
  if ((ahb2enr & H7_SRAM2EN) != 0u)
    n = add_range(r, n, H7_AHB2, H7_AHB_END);
  return n;
}

bool u2ps_dwc2_h7s3_dma_ok(uintptr_t addr, uint32_t len, uint32_t obw2sr, uint32_t ahb2enr)
{
  u2ps_dwc2_range_t r[U2PS_DWC2_RANGES_MAX];
  unsigned n = u2ps_dwc2_h7s3_dma_ranges(obw2sr, ahb2enr, r);
  return u2ps_dwc2_in_ranges(addr, len, r, n);
}
