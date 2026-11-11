/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dwc2_dma.c: the DWC2 ports' DMA memory rule (u2ps_dwc2_dma.h), for
 * the STM32U5, the STM32F723 and the STM32H7S3: each SRAM's bounds, ranges
 * across two SRAMs, SRAM4, the backup SRAM, ITCM RAM and the wrap of the
 * address, and on the U5 each SRAM's clock enable and power-down bit, on
 * the F723 the DTCM clock enable; on the H7S3 the AXI SRAMs as the option
 * bytes map them (FLASH_OBW2SR: ITCM_AXI_SHARE, DTCM_AXI_SHARE,
 * ECC_ON_SRAM) and the AHB SRAMs' clock enables. The ranges each rule
 * gives the host stack's contract check (u2ps_dwc2_*_dma_ranges) against
 * the rule itself, at every bound.
 */
#include "check.h"
#include "u2ps_dwc2_dma.h"

/* Reset values on the STM32U5Ax (RM0456 11.8.29-11.8.31: RCC_AHB1ENR
 * 0xD020_0100, RCC_AHB2ENR1 0xC000_0000, RCC_AHB2ENR2 0x8000_0000;
 * 10.10.1: PWR_CR1 0) */
static const u2ps_dwc2_u5_ram_t u5a_reset = {0xD0200100u, 0xC0000000u, 0x80000000u, 0u};
/* On the STM32U5Fx/5Gx RCC_AHB2ENR2 resets to 0xC000_0000 (SRAM6EN set) */
static const u2ps_dwc2_u5_ram_t u5g_reset = {0xD0200100u, 0xC0000000u, 0xC0000000u, 0u};

typedef struct {
  const char *name;
  uint32_t start, end;
  int reg;        /* 0 ahb1enr, 1 ahb2enr1, 2 ahb2enr2 */
  uint32_t en, pd;
} sram_t;

static const sram_t u5[] = {
  {"SRAM1", 0x20000000u, 0x200C0000u, 0, 1u << 31, 1u << 8},
  {"SRAM2", 0x200C0000u, 0x200D0000u, 1, 1u << 30, 1u << 9},
  {"SRAM3", 0x200D0000u, 0x201A0000u, 1, 1u << 31, 1u << 10},
  {"SRAM5", 0x201A0000u, 0x20270000u, 2, 1u << 31, 1u << 12},
  {"SRAM6", 0x20270000u, 0x202F0000u, 2, 1u << 30, 1u << 13},
};

static bool u5_ok(uint32_t a, uint32_t len, const u2ps_dwc2_u5_ram_t *r)
{
  return u2ps_dwc2_u5_dma_ok((uintptr_t)a, len, r);
}

static u2ps_dwc2_u5_ram_t without(const u2ps_dwc2_u5_ram_t *r, const sram_t *s, bool clock)
{
  u2ps_dwc2_u5_ram_t x = *r;
  if (!clock) {
    x.pwr_cr1 |= s->pd;
  } else if (s->reg == 0) {
    x.ahb1enr &= ~s->en;
  } else if (s->reg == 1) {
    x.ahb2enr1 &= ~s->en;
  } else {
    x.ahb2enr2 &= ~s->en;
  }
  return x;
}

/* Every SRAM: its first and last bytes, the whole of it; refused with its
 * clock off or powered down, and only then */
static void test_u5_srams(void)
{
  for (unsigned i = 0; i < sizeof u5 / sizeof u5[0]; i++) {
    const sram_t *s = &u5[i];
    uint32_t size = s->end - s->start;
    CHECK(u5_ok(s->start, 1, &u5g_reset));
    CHECK(u5_ok(s->end - 1u, 1, &u5g_reset));
    CHECK(u5_ok(s->start, size, &u5g_reset));
    CHECK(u5_ok(s->start + 64u, 512, &u5g_reset));
    for (int clock = 0; clock <= 1; clock++) {
      u2ps_dwc2_u5_ram_t off = without(&u5g_reset, s, clock != 0);
      CHECK(!u5_ok(s->start, 1, &off));
      CHECK(!u5_ok(s->end - 1u, 1, &off));
      CHECK(!u5_ok(s->start + 64u, 512, &off));
      /* The other SRAMs do not care */
      for (unsigned j = 0; j < sizeof u5 / sizeof u5[0]; j++)
        if (j != i)
          CHECK(u5_ok(u5[j].start + 64u, 512, &off));
    }
  }
  /* SRAM6 is off at reset on the U5Ax */
  CHECK(u5_ok(0x2026FFC0u, 64, &u5a_reset));
  CHECK(!u5_ok(0x20270000u, 64, &u5a_reset));
}

/* A range across two SRAMs needs both on */
static void test_u5_across(void)
{
  for (unsigned i = 0; i + 1u < sizeof u5 / sizeof u5[0]; i++) {
    uint32_t a = u5[i].end - 32u;
    CHECK(u5_ok(a, 64, &u5g_reset));
    for (int clock = 0; clock <= 1; clock++) {
      u2ps_dwc2_u5_ram_t lo = without(&u5g_reset, &u5[i], clock != 0);
      u2ps_dwc2_u5_ram_t hi = without(&u5g_reset, &u5[i + 1u], clock != 0);
      CHECK(!u5_ok(a, 64, &lo));
      CHECK(!u5_ok(a, 64, &hi));
      /* Ending right at the boundary touches only the lower one */
      CHECK(u5_ok(a, 32, &hi));
      CHECK(!u5_ok(a, 32, &lo));
      /* Starting right at it touches only the upper one */
      CHECK(u5_ok(u5[i].end, 32, &lo));
      CHECK(!u5_ok(u5[i].end, 32, &hi));
    }
  }
  /* All of them at once */
  CHECK(u5_ok(0x20000000u, 0x2F0000u, &u5g_reset));
}

/* Outside the SRAMs the OTG_HS bus reaches */
static void test_u5_outside(void)
{
  CHECK(!u5_ok(0x1FFFFFFFu, 1, &u5g_reset));
  CHECK(!u5_ok(0x1FFFFFF0u, 32, &u5g_reset));  /* reaches into SRAM1 */
  CHECK(!u5_ok(0x202F0000u, 1, &u5g_reset));   /* past SRAM6 */
  CHECK(!u5_ok(0x202EFFF0u, 32, &u5g_reset));
  CHECK(!u5_ok(0x28000000u, 64, &u5g_reset));  /* SRAM4 */
  CHECK(!u5_ok(0x40036400u, 64, &u5g_reset));  /* the backup SRAM */
  CHECK(!u5_ok(0x30000000u, 64, &u5g_reset));  /* SRAM1's secure alias */
  CHECK(!u5_ok(0x0A000000u, 64, &u5g_reset));  /* its C-bus alias */
  /* A length that would wrap a 32-bit address */
  CHECK(!u5_ok(0x20000000u, 0xFFFFFFFFu, &u5g_reset));
  CHECK(!u2ps_dwc2_u5_dma_ok(UINTPTR_MAX - 15u, 32, &u5g_reset));
  /* len 0: the address alone */
  CHECK(u5_ok(0x20001000u, 0, &u5g_reset));
  CHECK(!u5_ok(0x28000000u, 0, &u5g_reset));
}

static bool f723_ok(uint32_t a, uint32_t len, uint32_t ahb1enr)
{
  return u2ps_dwc2_f723_dma_ok((uintptr_t)a, len, ahb1enr);
}

/* RCC_AHB1ENR resets to 0x0010_0000 (RM0431 5.3.10): DTCMRAMEN set */
#define F723_RESET 0x00100000u
#define DTCMRAMEN  (1u << 20)

static void test_f723(void)
{
  /* DTCM, SRAM1, SRAM2: their bounds */
  CHECK(f723_ok(0x20000000u, 1, F723_RESET));
  CHECK(f723_ok(0x2000FFFFu, 1, F723_RESET));
  CHECK(f723_ok(0x20010000u, 1, F723_RESET));
  CHECK(f723_ok(0x2003BFFFu, 1, F723_RESET));
  CHECK(f723_ok(0x2003C000u, 1, F723_RESET));
  CHECK(f723_ok(0x2003FFFFu, 1, F723_RESET));
  CHECK(f723_ok(0x20000000u, 0x40000u, F723_RESET));
  /* DTCM's clock off: DTCM refused, SRAM1 and SRAM2 not */
  uint32_t off = F723_RESET & ~DTCMRAMEN;
  CHECK(!f723_ok(0x20000000u, 64, off));
  CHECK(!f723_ok(0x2000FFC0u, 64, off));
  CHECK(!f723_ok(0x2000FFE0u, 64, off)); /* across into SRAM1 */
  CHECK(f723_ok(0x20010000u, 64, off));
  CHECK(f723_ok(0x2003FFC0u, 64, off));
  /* Other bits of RCC_AHB1ENR do not matter */
  CHECK(f723_ok(0x20000000u, 64, 0xFFFFFFFFu));
  CHECK(!f723_ok(0x20000000u, 64, ~DTCMRAMEN));
  /* Outside: ITCM RAM, the flash, below DTCM, past SRAM2, a wrap */
  CHECK(!f723_ok(0x00000000u, 64, F723_RESET));
  CHECK(!f723_ok(0x00003FC0u, 64, F723_RESET));
  CHECK(!f723_ok(0x08000000u, 64, F723_RESET));
  CHECK(!f723_ok(0x1FFFFFE0u, 64, F723_RESET));
  CHECK(!f723_ok(0x20040000u, 1, F723_RESET));
  CHECK(!f723_ok(0x2003FFE0u, 64, F723_RESET));
  CHECK(!f723_ok(0x20000000u, 0xFFFFFFFFu, F723_RESET));
  CHECK(f723_ok(0x20020000u, 0, F723_RESET));
  CHECK(!f723_ok(0x20040001u, 0, F723_RESET));
}

/* ---- STM32H7S3 (RM0477 Rev 10) ---------------------------------------------- */

static bool h7_ok(uint32_t a, uint32_t len, uint32_t obw2sr, uint32_t ahb2enr)
{
  return u2ps_dwc2_h7s3_dma_ok((uintptr_t)a, len, obw2sr, ahb2enr);
}

/* FLASH_OBW2SR (5.9.34, p. 273): ITCM_AXI_SHARE [2:0], DTCM_AXI_SHARE
 * [6:4], ECC_ON_SRAM 8; factory value 0x100: shares 000, ECC on (Table 32,
 * p. 227). RCC_AHB2ENR (7.8.46, p. 499): SRAM1EN 29, SRAM2EN 30, both 0 at
 * reset. */
#define H7_FACTORY 0x100u
#define H7_ECC     (1u << 8)
#define H7_ITCM(v) ((uint32_t)(v) << 0)
#define H7_DTCM(v) ((uint32_t)(v) << 4)
#define H7_AHB1    (1u << 29)
#define H7_AHB2    (1u << 30)
#define H7_AHB     (H7_AHB1 | H7_AHB2)

/* AXI SRAM1 (Table 12, p. 162), SRAM2 (always), SRAM3 (Table 13), SRAM4
 * (Table 14), at the addresses of the memory map (Figure 3, p. 151) */
#define SRAM1   0x24000000u
#define SRAM1_HI 0x24010000u /* ITCM_AXI_SHARE 001: only 64 KB, from here */
#define SRAM2   0x24020000u
#define SRAM3   0x24040000u
#define SRAM3_HI 0x24050000u /* DTCM_AXI_SHARE 001: only up to here */
#define SRAM4   0x24060000u
#define SRAM4_END 0x24072000u
#define AHB1    0x30000000u
#define AHB2    0x30004000u
#define AHB_END 0x30008000u

static void test_h7s3_factory(void)
{
  uint32_t f = H7_FACTORY;
  /* SRAM1, SRAM2, SRAM3: their bounds and all of them at once */
  CHECK(h7_ok(SRAM1, 1, f, 0));
  CHECK(h7_ok(SRAM2 - 1u, 1, f, 0));
  CHECK(h7_ok(SRAM2, 1, f, 0));
  CHECK(h7_ok(SRAM3 - 1u, 1, f, 0));
  CHECK(h7_ok(SRAM3, 1, f, 0));
  CHECK(h7_ok(SRAM4 - 1u, 1, f, 0));
  CHECK(h7_ok(SRAM1, SRAM4 - SRAM1, f, 0));
  /* Across SRAM1 to SRAM2 and SRAM2 to SRAM3 */
  CHECK(h7_ok(SRAM2 - 32u, 64, f, 0));
  CHECK(h7_ok(SRAM3 - 32u, 64, f, 0));
  /* SRAM4 is the ECC's with ECC_ON_SRAM = 1: refused, and a range into it */
  CHECK(!h7_ok(SRAM4, 1, f, 0));
  CHECK(!h7_ok(SRAM4 - 32u, 64, f, 0));
  CHECK(!h7_ok(SRAM4_END - 1u, 1, f, 0));
  /* The AHB SRAMs with their clocks off, as at reset */
  CHECK(!h7_ok(AHB1, 1, f, 0));
  CHECK(!h7_ok(AHB2, 1, f, 0));
  /* Never: DTCM, ITCM, the backup SRAM, flash, the GFXMMU range, below
   * SRAM1, past SRAM4, past the AHB SRAMs (Table 2, p. 123: OTG_HS reaches
   * no TCM nor the backup RAM; flash is not RAM) */
  CHECK(!h7_ok(0x20000000u, 64, f, H7_AHB));
  CHECK(!h7_ok(0x2002FFC0u, 64, f, H7_AHB));
  CHECK(!h7_ok(0x00000000u, 64, f, H7_AHB));
  CHECK(!h7_ok(0x0002FFC0u, 64, f, H7_AHB));
  CHECK(!h7_ok(0x38800000u, 64, f, H7_AHB));
  CHECK(!h7_ok(0x08000000u, 64, f, H7_AHB));
  CHECK(!h7_ok(0x25000000u, 64, f, H7_AHB));
  CHECK(!h7_ok(SRAM1 - 1u, 1, f, H7_AHB));
  CHECK(!h7_ok(SRAM1 - 32u, 64, f, H7_AHB));
  CHECK(!h7_ok(SRAM4_END, 1, 0u, H7_AHB));
  CHECK(!h7_ok(AHB_END, 1, f, H7_AHB));
  CHECK(!h7_ok(AHB1 - 1u, 1, f, H7_AHB));
  /* A wrap of the address, and len 0 */
  CHECK(!h7_ok(SRAM1, 0xFFFFFFFFu, f, H7_AHB));
  CHECK(!u2ps_dwc2_h7s3_dma_ok(UINTPTR_MAX - 15u, 32, f, H7_AHB));
  CHECK(h7_ok(SRAM2 + 0x100u, 0, f, 0));
  CHECK(!h7_ok(0x20000100u, 0, f, 0));
}

/* ECC_ON_SRAM = 0: SRAM4 joins, right after SRAM3 */
static void test_h7s3_ecc(void)
{
  CHECK(h7_ok(SRAM4, 1, 0u, 0));
  CHECK(h7_ok(SRAM4_END - 1u, 1, 0u, 0));
  CHECK(h7_ok(SRAM4 - 32u, 64, 0u, 0));
  CHECK(h7_ok(SRAM1, SRAM4_END - SRAM1, 0u, 0));
  CHECK(!h7_ok(SRAM4_END - 32u, 64, 0u, 0));
  CHECK(!h7_ok(SRAM4, 64, H7_ECC, 0));
  /* Other bits of FLASH_OBW2SR do not matter (I2C_NI3C 9, reserved) */
  CHECK(h7_ok(SRAM4, 64, 0xFFFFFE00u, 0));
  CHECK(!h7_ok(SRAM4, 64, 0xFFFFFE00u | H7_ECC, 0));
}

/* ITCM_AXI_SHARE (Table 12) for SRAM1 and DTCM_AXI_SHARE (Table 13) for
 * SRAM3: 000 and 011 all of it, 001 the half, 010 none; the values with
 * bit 2 set are undefined ("Bit 2 should be kept to 0", 5.9.35, p. 274)
 * and refuse the SRAM. Each field decides its SRAM alone. */
static void test_h7s3_share(void)
{
  for (uint32_t v = 0; v < 8u; v++) {
    bool all = v == 0u || v == 3u, half = v == 1u;
    /* SRAM1 */
    uint32_t o = H7_ECC | H7_ITCM(v);
    CHECK_EQ(h7_ok(SRAM1, 1, o, 0), all);
    CHECK_EQ(h7_ok(SRAM1_HI - 1u, 1, o, 0), all);
    CHECK_EQ(h7_ok(SRAM1_HI, 1, o, 0), all || half);
    CHECK_EQ(h7_ok(SRAM2 - 1u, 1, o, 0), all || half);
    CHECK_EQ(h7_ok(SRAM1_HI - 32u, 64, o, 0), all); /* into the unmapped half */
    CHECK_EQ(h7_ok(SRAM2 - 32u, 64, o, 0), all || half);
    /* SRAM2 and SRAM3 do not care */
    CHECK(h7_ok(SRAM2, 64, o, 0));
    CHECK(h7_ok(SRAM3, 64, o, 0));
    CHECK(h7_ok(SRAM4 - 64u, 64, o, 0));
    /* SRAM3 */
    o = H7_ECC | H7_DTCM(v);
    CHECK_EQ(h7_ok(SRAM3, 1, o, 0), all || half);
    CHECK_EQ(h7_ok(SRAM3_HI - 1u, 1, o, 0), all || half);
    CHECK_EQ(h7_ok(SRAM3_HI, 1, o, 0), all);
    CHECK_EQ(h7_ok(SRAM4 - 1u, 1, o, 0), all);
    CHECK_EQ(h7_ok(SRAM3_HI - 32u, 64, o, 0), all);
    CHECK_EQ(h7_ok(SRAM3 - 32u, 64, o, 0), all || half); /* SRAM2 to SRAM3 */
    CHECK(h7_ok(SRAM1, 64, o, 0));
    CHECK(h7_ok(SRAM3 - 64u, 64, o, 0));
    /* SRAM4, after SRAM3, with ECC off: reached in every case; a range
     * from SRAM3 into it only with SRAM3 whole */
    o = H7_DTCM(v);
    CHECK(h7_ok(SRAM4, 64, o, 0));
    CHECK_EQ(h7_ok(SRAM4 - 32u, 64, o, 0), all);
  }
}

/* AHB SRAM1 and SRAM2 (Figure 3, p. 151): each with its clock enable */
static void test_h7s3_ahb(void)
{
  uint32_t f = H7_FACTORY;
  CHECK(h7_ok(AHB1, 1, f, H7_AHB1));
  CHECK(h7_ok(AHB2 - 1u, 1, f, H7_AHB1));
  CHECK(!h7_ok(AHB2, 1, f, H7_AHB1));
  CHECK(!h7_ok(AHB1, 1, f, H7_AHB2));
  CHECK(h7_ok(AHB2, 1, f, H7_AHB2));
  CHECK(h7_ok(AHB_END - 1u, 1, f, H7_AHB2));
  /* Across the two: both enables */
  CHECK(h7_ok(AHB2 - 32u, 64, f, H7_AHB));
  CHECK(!h7_ok(AHB2 - 32u, 64, f, H7_AHB1));
  CHECK(!h7_ok(AHB2 - 32u, 64, f, H7_AHB2));
  CHECK(h7_ok(AHB1, AHB_END - AHB1, f, H7_AHB));
  /* Other bits of RCC_AHB2ENR do not matter */
  CHECK(h7_ok(AHB1, 64, f, 0xFFFFFFFFu));
  CHECK(!h7_ok(AHB1, 64, f, ~H7_AHB1));
  CHECK(!h7_ok(AHB2, 64, f, ~H7_AHB2));
  /* The AXI SRAMs do not need them */
  CHECK(h7_ok(SRAM2, 64, f, ~H7_AHB));
}

/* ---- The ranges of each rule (u2ps_dwc2_*_dma_ranges) ----------------------- */

/* The bounds to probe: each start and end of every RAM of the three chips
 * and the 32 bytes around each */
static const uint32_t probe[] = {
  0x00000000u, 0x00010000u, 0x00030000u, 0x08000000u, 0x1FFFFFE0u, 0x20000000u, 0x20010000u,
  0x20030000u, 0x2003C000u, 0x20040000u, 0x200C0000u, 0x200D0000u, 0x201A0000u, 0x20270000u,
  0x202F0000u, SRAM1,       SRAM1_HI,    SRAM2,       SRAM3,       SRAM3_HI,    SRAM4,
  SRAM4_END,   0x25000000u, 0x28000000u, AHB1,        AHB2,        AHB_END,     0x38800000u};
static const uint32_t lens[] = {0, 1, 32, 64, 0x10000u, 0x20000u, 0xFFFFFFFFu};

/* The ranges in address order, none touching the next */
static bool ranges_sorted(const u2ps_dwc2_range_t *r, unsigned n)
{
  for (unsigned i = 0; i < n; i++) {
    if (r[i].start >= r[i].end)
      return false;
    if (i > 0u && r[i].start <= r[i - 1u].end)
      return false;
  }
  return true;
}

/* rule(a, len) against the ranges at every probe, its neighbours and
 * length; from lens[first] on (the U5's rule takes len 0 anywhere in the
 * SRAMs' span, its ranges only within one: a length the host stack never
 * checks) */
#define SAME_AT_PROBES(rule, r, n, bad, first)                                              \
  do {                                                                                      \
    for (unsigned p = 0; p < sizeof probe / sizeof probe[0]; p++)                           \
      for (int d = -32; d <= 32; d += 16)                                                   \
        for (unsigned l = (first); l < sizeof lens / sizeof lens[0]; l++) {                 \
          uintptr_t a = (uintptr_t)(uint32_t)((int64_t)probe[p] + d);                       \
          if ((rule) != u2ps_dwc2_in_ranges(a, lens[l], (r), (n)))                          \
            (bad)++;                                                                        \
        }                                                                                   \
  } while (0)

static void test_ranges_h7s3(void)
{
  u2ps_dwc2_range_t r[U2PS_DWC2_RANGES_MAX];
  /* The factory option bytes, AHB clocks off: SRAM1 to SRAM3 */
  unsigned n = u2ps_dwc2_h7s3_dma_ranges(H7_FACTORY, 0, r);
  CHECK_EQ(n, 1);
  CHECK_EQ(r[0].start, SRAM1);
  CHECK_EQ(r[0].end, SRAM4);
  /* ECC off and both AHB SRAMs: SRAM1 to SRAM4, and the AHB SRAMs */
  n = u2ps_dwc2_h7s3_dma_ranges(0u, H7_AHB, r);
  CHECK_EQ(n, 2);
  CHECK_EQ(r[0].start, SRAM1);
  CHECK_EQ(r[0].end, SRAM4_END);
  CHECK_EQ(r[1].start, AHB1);
  CHECK_EQ(r[1].end, AHB_END);
  /* Both shares 001, ECC off, AHB SRAM2 only: a gap before SRAM4 */
  n = u2ps_dwc2_h7s3_dma_ranges(H7_ITCM(1) | H7_DTCM(1), H7_AHB2, r);
  CHECK_EQ(n, 3);
  CHECK_EQ(r[0].start, SRAM1_HI);
  CHECK_EQ(r[0].end, SRAM3_HI);
  CHECK_EQ(r[1].start, SRAM4);
  CHECK_EQ(r[1].end, SRAM4_END);
  CHECK_EQ(r[2].start, AHB2);
  CHECK_EQ(r[2].end, AHB_END);
  /* Every option byte value and clock enable: the ranges are the rule */
  static const uint32_t enr[] = {0, H7_AHB1, H7_AHB2, H7_AHB, 0xFFFFFFFFu, ~H7_AHB};
  unsigned bad = 0, unsorted = 0;
  for (uint32_t o = 0; o < 0x200u; o++) {
    if ((o & 0x88u) != 0u)
      continue; /* reserved bits 7 and 3 */
    for (unsigned e = 0; e < sizeof enr / sizeof enr[0]; e++) {
      n = u2ps_dwc2_h7s3_dma_ranges(o, enr[e], r);
      if (n > U2PS_DWC2_RANGES_MAX || !ranges_sorted(r, n))
        unsorted++;
      SAME_AT_PROBES(u2ps_dwc2_h7s3_dma_ok(a, lens[l], o, enr[e]), r, n, bad, 0u);
    }
  }
  CHECK_EQ(unsorted, 0);
  CHECK_EQ(bad, 0);
}

static void test_ranges_u5(void)
{
  u2ps_dwc2_range_t r[U2PS_DWC2_RANGES_MAX];
  unsigned n = u2ps_dwc2_u5_dma_ranges(&u5g_reset, r);
  CHECK_EQ(n, 1);
  CHECK_EQ(r[0].start, 0x20000000u);
  CHECK_EQ(r[0].end, 0x202F0000u);
  n = u2ps_dwc2_u5_dma_ranges(&u5a_reset, r);
  CHECK_EQ(n, 1);
  CHECK_EQ(r[0].end, 0x20270000u);
  /* Every combination of the SRAMs on and off, by clock and by power */
  unsigned bad = 0, unsorted = 0;
  for (unsigned m = 0; m < 32u; m++) {
    for (int clock = 0; clock <= 1; clock++) {
      u2ps_dwc2_u5_ram_t x = u5g_reset;
      for (unsigned i = 0; i < 5u; i++)
        if ((m & (1u << i)) != 0u)
          x = without(&x, &u5[i], clock != 0);
      n = u2ps_dwc2_u5_dma_ranges(&x, r);
      if (n > U2PS_DWC2_RANGES_MAX || !ranges_sorted(r, n))
        unsorted++;
      SAME_AT_PROBES(u2ps_dwc2_u5_dma_ok(a, lens[l], &x), r, n, bad, 1u);
    }
  }
  CHECK_EQ(unsorted, 0);
  CHECK_EQ(bad, 0);
}

static void test_ranges_f723(void)
{
  u2ps_dwc2_range_t r[U2PS_DWC2_RANGES_MAX];
  unsigned n = u2ps_dwc2_f723_dma_ranges(F723_RESET, r);
  CHECK_EQ(n, 1);
  CHECK_EQ(r[0].start, 0x20000000u);
  CHECK_EQ(r[0].end, 0x20040000u);
  n = u2ps_dwc2_f723_dma_ranges(F723_RESET & ~DTCMRAMEN, r);
  CHECK_EQ(n, 1);
  CHECK_EQ(r[0].start, 0x20010000u);
  unsigned bad = 0;
  static const uint32_t enr[] = {F723_RESET, 0u, 0xFFFFFFFFu, ~DTCMRAMEN};
  for (unsigned e = 0; e < sizeof enr / sizeof enr[0]; e++) {
    n = u2ps_dwc2_f723_dma_ranges(enr[e], r);
    CHECK(ranges_sorted(r, n));
    SAME_AT_PROBES(u2ps_dwc2_f723_dma_ok(a, lens[l], enr[e]), r, n, bad, 0u);
  }
  CHECK_EQ(bad, 0);
}

/* u2ps_dwc2_in_ranges() itself: within one range only, no wrap */
static void test_in_ranges(void)
{
  static const u2ps_dwc2_range_t r[] = {{0x1000u, 0x2000u}, {0x3000u, 0x4000u}};
  CHECK(u2ps_dwc2_in_ranges(0x1000u, 0x1000u, r, 2));
  CHECK(u2ps_dwc2_in_ranges(0x3FFFu, 1, r, 2));
  CHECK(!u2ps_dwc2_in_ranges(0x1FFFu, 2, r, 2));
  CHECK(!u2ps_dwc2_in_ranges(0x1800u, 0x2000u, r, 2)); /* over the gap */
  CHECK(!u2ps_dwc2_in_ranges(0x0FFFu, 1, r, 2));
  CHECK(!u2ps_dwc2_in_ranges(0x4000u, 1, r, 2));
  CHECK(!u2ps_dwc2_in_ranges(0x3000u, 1, r, 1)); /* only the first counted */
  CHECK(!u2ps_dwc2_in_ranges(0x1000u, 1, r, 0));
  static const u2ps_dwc2_range_t top[] = {{0x1000u, UINTPTR_MAX}};
  CHECK(!u2ps_dwc2_in_ranges(UINTPTR_MAX - 15u, 32, top, 1));
  CHECK(u2ps_dwc2_in_ranges(UINTPTR_MAX - 15u, 15, top, 1));
}

int main(void)
{
  test_u5_srams();
  test_u5_across();
  test_u5_outside();
  test_f723();
  test_h7s3_factory();
  test_h7s3_ecc();
  test_h7s3_share();
  test_h7s3_ahb();
  test_in_ranges();
  test_ranges_h7s3();
  test_ranges_u5();
  test_ranges_f723();
  return check_done();
}
