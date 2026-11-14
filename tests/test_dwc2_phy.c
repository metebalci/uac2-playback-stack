/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dwc2_phy.c: the DWC2 start-up's register code
 * (port/dwc2/common/u2ps_dwc2_phy.c) against a register model
 * (fake_regs.h), built once per chip (TEST_H7S3, at 24 MHz and, as
 * TEST_H7S3_16, at 16 MHz; TEST_F723; TEST_U5A5).
 *
 * STM32H7S3 (RM0477 Rev 10, the design's order): the cycle counter first
 * (the DWT unlocked through its lock access register, enabled, and seen to
 * count), then HSERDY (RCC_CR bit 17), then the upper half-word of
 * PWR_CSR2 only (USB33DEN and USBHSREGEN set, USBREGEN kept; the lower
 * byte is write-once, 6.8.4 p. 323, so no word access at all) and a wait
 * for USB33RDY of at least 10 ms, then RCC_CCIPR1's USBPHYCSEL and
 * USBREFCKSEL written while RCC_AHB1ENR.USBPHYCEN is 0 (7.5, p. 401), then
 * USBPHYCEN and OTGHSEN. Each failure returns its reason and touches
 * nothing after it.
 * STM32F723 (Cortex-M7): the same cycle counter start, before any RCC
 * access; with the HSE ready, the whole start: 0xF13 OR-ed into
 * USBPHYC_TUNE (RM0431 33.4.2), keeping what was there, after the PLL1
 * select and before PLL1_EN. STM32U5A5 (Cortex-M33): no lock access
 * register, no check.
 *
 * The model: the DWT ignores writes of DWT_CTRL until DWT_LAR holds the
 * key (a locked Cortex-M7 DWT, inferred from reports); CYCCNT advances by
 * 'step' at each read while TRCENA and CYCCNTENA are set, unless 'stuck';
 * USB33RDY sets 'rdy_after' PWR_CSR2 accesses after USB33DEN, or never.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "fake_regs.h"
#include "u2ps_dwc2.h"

#define DEMCR_A    0xE000EDFCu
#define DWT_CTRL_A 0xE0001000u
#define CYCCNT_A   0xE0001004u
#define DWT_LAR_A  0xE0001FB0u
#define LAR_KEY    0xC5ACCE55u
#define TRCENA     (1u << 24)

#define H7_RCC_CR   0x58024400u
#define H7_CCIPR1   0x5802444Cu
#define H7_AHB1ENR  0x58024538u
#define H7_CSR2     0x5802480Cu
#define H7_CSR2_HI  0x5802480Eu
#define USB33DEN    (1u << 24)
#define USBREGEN    (1u << 25)
#define USB33RDY    (1u << 26)
#define USBHSREGEN  (1u << 27)
#define USBPHYCEN   (1u << 26)
#define OTGHSEN     (1u << 25)
#define HSERDY      (1u << 17)

#define F7_RCC_CR   0x40023800u
#define U5_RCC_CR   0x46020C00u

/* ---- The register model -------------------------------------------------------- */

static struct {
  uintptr_t addr;
  uint32_t v;
} regs[64];
static unsigned nregs;

static struct {
  bool m7_lock;  /* DWT_CTRL writes ignored until DWT_LAR holds the key */
  bool stuck;    /* CYCCNT never advances */
  uint32_t step; /* cycles per CYCCNT read */
  int rdy_after; /* USB33RDY: PWR_CSR2 accesses after USB33DEN; -1 never */
} model;

#define ACC_MAX 4096u
typedef struct {
  uintptr_t addr;
  unsigned width;
} access_t;
static access_t acc[ACC_MAX];
static unsigned nacc;
static unsigned violations;
static uint32_t last_ccipr1, last_dwt_ctrl, csr2_lo0;
static int csr2_since_den;

static uint32_t *slot(uintptr_t a)
{
  a &= ~(uintptr_t)3u;
  for (unsigned i = 0; i < nregs; i++)
    if (regs[i].addr == a)
      return &regs[i].v;
  if (nregs == sizeof regs / sizeof regs[0]) {
    printf("  model: too many registers\n");
    violations++;
    return &regs[0].v;
  }
  regs[nregs].addr = a;
  regs[nregs].v = 0;
  return &regs[nregs++].v;
}

static void violation(const char *what)
{
  printf("  model: %s\n", what);
  violations++;
}

/* What the writes since the last access did, then the access itself */
static void model_step(uintptr_t a, unsigned width)
{
  uint32_t c = *slot(H7_CCIPR1);
  if (((c ^ last_ccipr1) & 0x3F00u) != 0u && (*slot(H7_AHB1ENR) & USBPHYCEN) != 0u)
    violation("RCC_CCIPR1's USB PHY fields changed with USBPHYCEN set");
  last_ccipr1 = c;
  if (model.m7_lock && *slot(DWT_LAR_A) != LAR_KEY)
    *slot(DWT_CTRL_A) = last_dwt_ctrl;
  last_dwt_ctrl = *slot(DWT_CTRL_A);
  if ((*slot(H7_CSR2) & 0xFFFFu) != csr2_lo0)
    violation("PWR_CSR2's lower half-word changed");
  if ((*slot(H7_CSR2) & USB33DEN) != 0u && model.rdy_after >= 0) {
    if (csr2_since_den++ >= model.rdy_after)
      *slot(H7_CSR2) |= USB33RDY;
  }
  if (width == 0u)
    return;
  if (nacc < ACC_MAX)
    acc[nacc++] = (access_t){a, width};
  if (a == H7_CSR2)
    violation(width == 4u ? "a word access of PWR_CSR2" : "a half-word access of PWR_CSR2's lower half");
  if (a == CYCCNT_A && !model.stuck && (*slot(DEMCR_A) & TRCENA) != 0u &&
      (*slot(DWT_CTRL_A) & 1u) != 0u)
    *slot(CYCCNT_A) += model.step;
}

uint32_t *fake_reg32(uintptr_t addr)
{
  if (addr % 4u != 0u)
    violation("a word access not on a word");
  model_step(addr, 4);
  return slot(addr);
}

uint16_t *fake_reg16(uintptr_t addr)
{
  if (addr % 2u != 0u)
    violation("a half-word access not on a half-word");
  model_step(addr, 2);
  return (uint16_t *)((uint8_t *)slot(addr) + (addr & 2u)); /* little-endian host */
}

static void model_reset(void)
{
  memset(regs, 0, sizeof regs);
  nregs = nacc = violations = 0;
  model.m7_lock = true;
  model.stuck = false;
  model.step = 1000;
  model.rdy_after = 3;
  last_ccipr1 = last_dwt_ctrl = csr2_lo0 = 0;
  csr2_since_den = 0;
}

/* Preset a register before the start, outside the access log */
static inline void preset(uintptr_t a, uint32_t v)
{
  *slot(a) = v;
  if (a == H7_CCIPR1)
    last_ccipr1 = v;
  if (a == H7_CSR2)
    csr2_lo0 = v & 0xFFFFu;
}

/* Evaluate the writes after the last access */
static void flush(void)
{
  model_step(0, 0);
}

static uint32_t reg(uintptr_t a)
{
  return *slot(a);
}

/* The first and the last access to an address in the log; -1 if none
 * (inline: not every chip's test uses every helper) */
static int first(uintptr_t a)
{
  for (unsigned i = 0; i < nacc; i++)
    if (acc[i].addr == a)
      return (int)i;
  return -1;
}

static inline int last(uintptr_t a)
{
  for (unsigned i = nacc; i-- > 0;)
    if (acc[i].addr == a)
      return (int)i;
  return -1;
}

/* Accesses to [lo, hi) */
static inline unsigned accesses(uintptr_t lo, uintptr_t hi)
{
  unsigned n = 0;
  for (unsigned i = 0; i < nacc; i++)
    n += acc[i].addr >= lo && acc[i].addr < hi;
  return n;
}

/* ---- What u2ps_dwc2.c supplies, for the F723's and the U5's starts ----------- */

static unsigned waits;

bool u2ps_dwc2_wait_us(volatile uint32_t *r, uint32_t mask, uint32_t value, uint32_t us)
{
  (void)us;
  waits++;
  return (*r & mask) == value;
}

void u2ps_dwc2_delay_us(uint32_t us)
{
  (void)us;
}

void u2ps_dwc2_pwr_clock(void)
{
}

/* ---- STM32H7S3 ---------------------------------------------------------------- */

#if defined(TEST_H7S3) || defined(TEST_H7S3_16)
#if defined(TEST_H7S3)
#define REFSEL 0xAu /* 24 MHz */
#else
#define REFSEL 0x3u /* 16 MHz */
#endif

/* The app's state before the start: the supply written (lower byte
 * 0x02, LDOEN, Table 44 p. 286), the PHY clock already enabled by other
 * code, USBPHYCSEL at a reserved value, another kernel clock selection */
static void h7_app(uint32_t csr2)
{
  model_reset();
  preset(H7_RCC_CR, HSERDY | 0x1u);
  preset(H7_CSR2, csr2);
  preset(H7_AHB1ENR, USBPHYCEN | 0x1u);
  preset(H7_CCIPR1, 0x00003540u);
}

static void test_h7s3_ok(void)
{
  for (int regen = 0; regen <= 1; regen++) {
    uint32_t keep = regen ? USBREGEN : 0u;
    h7_app(0x00000002u | keep);
    CHECK_EQ(u2ps_dwc2_phy_start(), U2PS_DWC2_OK);
    flush();
    CHECK_EQ(violations, 0);
    /* The end state */
    CHECK_EQ(reg(H7_CSR2), 0x00000002u | keep | USB33DEN | USB33RDY | USBHSREGEN);
    CHECK_EQ(reg(H7_CCIPR1), 0x00000040u | REFSEL << 8);
    CHECK_EQ(reg(H7_AHB1ENR), USBPHYCEN | OTGHSEN | 0x1u);
    CHECK_EQ(reg(DWT_LAR_A), LAR_KEY);
    CHECK((reg(DEMCR_A) & TRCENA) != 0u);
    CHECK((reg(DWT_CTRL_A) & 1u) != 0u);
    /* The order: counter, HSERDY, PWR, then the clock selection with the
     * PHY clock off, then the enables */
    int dwt = first(DWT_CTRL_A), hse = first(H7_RCC_CR), pwr = first(H7_CSR2_HI);
    int sel = first(H7_CCIPR1), sel_end = last(H7_CCIPR1), en = last(H7_AHB1ENR);
    CHECK(first(DEMCR_A) >= 0 && first(DEMCR_A) < hse);
    CHECK(dwt >= 0 && dwt < hse);
    CHECK(first(DWT_LAR_A) >= 0 && first(DWT_LAR_A) < dwt);
    CHECK(first(CYCCNT_A) >= 0 && first(CYCCNT_A) < hse);
    CHECK(hse >= 0 && hse < pwr);
    CHECK(pwr >= 0 && last(H7_CSR2_HI) < sel);
    CHECK(first(H7_AHB1ENR) > last(H7_CSR2_HI) && first(H7_AHB1ENR) < sel);
    CHECK(sel >= 0 && sel_end < en);
    /* Only the upper half-word of PWR_CSR2, never its word */
    CHECK_EQ(accesses(H7_CSR2, H7_CSR2 + 2u), 0);
  }
}

/* USBPHYCSEL and USBREFCKSEL as reset (00, 1010) with the PHY clock off:
 * the same end */
static void test_h7s3_reset_state(void)
{
  model_reset();
  preset(H7_RCC_CR, HSERDY);
  preset(H7_CSR2, 0x00000002u);
  preset(H7_CCIPR1, 0x00000A00u);
  CHECK_EQ(u2ps_dwc2_phy_start(), U2PS_DWC2_OK);
  flush();
  CHECK_EQ(violations, 0);
  CHECK_EQ(reg(H7_CCIPR1), REFSEL << 8);
  CHECK_EQ(reg(H7_AHB1ENR), USBPHYCEN | OTGHSEN);
}

/* The counter does not count: CYCLES, before any RCC or PWR access */
static void test_h7s3_stuck(void)
{
  h7_app(0x00000002u);
  model.stuck = true;
  CHECK_EQ(u2ps_dwc2_phy_start(), U2PS_DWC2_ERR_CYCLES);
  flush();
  CHECK_EQ(accesses(0x58024400u, 0x58024C00u), 0);
  CHECK_EQ(violations, 0);
}

/* No HSE: CLOCK, with PWR_CSR2, RCC_CCIPR1 and RCC_AHB1ENR untouched */
static void test_h7s3_no_hse(void)
{
  h7_app(0x00000002u);
  *slot(H7_RCC_CR) = 0x1u;
  CHECK_EQ(u2ps_dwc2_phy_start(), U2PS_DWC2_ERR_CLOCK);
  flush();
  CHECK_EQ(first(H7_CSR2_HI), -1);
  CHECK_EQ(first(H7_CCIPR1), -1);
  CHECK_EQ(first(H7_AHB1ENR), -1);
  CHECK_EQ(reg(H7_CSR2), 0x00000002u);
  CHECK_EQ(violations, 0);
}

/* USB33RDY never comes: PHY after at least 10 ms (CYCCNT at the core
 * clock U2PS_DWC2_CORE_HZ), the clocks untouched */
static void test_h7s3_no_usb33(void)
{
  h7_app(0x00000002u);
  model.rdy_after = -1;
  CHECK_EQ(u2ps_dwc2_phy_start(), U2PS_DWC2_ERR_PHY);
  flush();
  uint32_t spent = reg(CYCCNT_A);
  printf("  USB33RDY wait: %lu cycles\n", (unsigned long)spent);
  CHECK(spent >= 10000u * (U2PS_DWC2_CORE_HZ / 1000000u));
  CHECK(spent <= 11000u * (U2PS_DWC2_CORE_HZ / 1000000u));
  CHECK_EQ(first(H7_CCIPR1), -1);
  CHECK_EQ(first(H7_AHB1ENR), -1);
  CHECK_EQ(reg(H7_CSR2), 0x00000002u | USB33DEN | USBHSREGEN);
  CHECK_EQ(violations, 0);
}
#endif

/* ---- STM32F723 ---------------------------------------------------------------- */

#if defined(TEST_F723)
/* The counter, unlocked first, then HSERDY (here off: CLOCK) */
static void test_f723_cycles(void)
{
  model_reset();
  CHECK_EQ(u2ps_dwc2_phy_start(), U2PS_DWC2_ERR_CLOCK);
  flush();
  CHECK_EQ(reg(DWT_LAR_A), LAR_KEY);
  CHECK((reg(DWT_CTRL_A) & 1u) != 0u);
  CHECK(first(DWT_LAR_A) >= 0 && first(DWT_LAR_A) < first(DWT_CTRL_A));
  CHECK(first(CYCCNT_A) >= 0 && first(CYCCNT_A) < first(F7_RCC_CR));
  CHECK_EQ(violations, 0);
  /* A counter that does not count: CYCLES, no RCC access */
  model_reset();
  model.stuck = true;
  CHECK_EQ(u2ps_dwc2_phy_start(), U2PS_DWC2_ERR_CYCLES);
  CHECK_EQ(accesses(0x40023800u, 0x40023C00u), 0);
  CHECK_EQ(waits, 0);
}

/* The whole start with the HSE ready: the USBPHYC tuning OR-ed into
 * USBPHYC_TUNE, 0xF13 (RM0431 33.4.2: INCURREN, INCURRINT, HSDRVDCCUR,
 * HSDRVRFRED, HSDRVCHKITRM 0111), keeping what was there (the reset
 * value 0x4, LFSCAPEN: 0xF17), written after the PLL1 select and before
 * PLL1_EN */
#define F7_PHYC_PLL1 0x40017C00u
#define F7_PHYC_TUNE 0x40017C0Cu
#define F7_PHYC_LDO  0x40017C18u
static void test_f723_tune(void)
{
  /* the reset value; with SQLBYP and SHTCCTCLPROT; HSDRVDCCUR set and
   * LFSCAPEN clear */
  static const uint32_t pres[] = {0x00000004u, 0x00C00004u, 0x00C00010u};
  for (unsigned k = 0; k < sizeof pres / sizeof pres[0]; k++) {
    uint32_t pre = pres[k];
    model_reset();
    preset(F7_RCC_CR, 1u << 17); /* HSERDY */
    preset(F7_PHYC_TUNE, pre);
    preset(F7_PHYC_LDO, 1u << 1); /* LDO_STATUS: ready */
    CHECK_EQ(u2ps_dwc2_phy_start(), U2PS_DWC2_OK);
    flush();
    CHECK_EQ(reg(F7_PHYC_TUNE), pre | 0x00000F13u);
    CHECK_EQ(reg(F7_PHYC_PLL1) & 1u, 1u); /* PLL1_EN */
    /* the select, the tuning, then the enable */
    int sel = first(F7_PHYC_PLL1), tune = first(F7_PHYC_TUNE), en = last(F7_PHYC_PLL1);
    CHECK(sel >= 0 && tune > sel && en > tune);
    CHECK_EQ(last(F7_PHYC_TUNE) < en, 1);
    CHECK_EQ(violations, 0);
    /* A second start (a re-init) leaves the value as it is */
    CHECK_EQ(u2ps_dwc2_phy_start(), U2PS_DWC2_OK);
    flush();
    CHECK_EQ(reg(F7_PHYC_TUNE), pre | 0x00000F13u);
  }
}
#endif

/* ---- STM32U5A5 ---------------------------------------------------------------- */

#if defined(TEST_U5A5)
/* The Cortex-M33: no lock access register, no check of the counter; the
 * start goes on to TZEN and HSERDY (here off: CLOCK) */
static void test_u5_cycles(void)
{
  model_reset();
  model.m7_lock = false;
  model.stuck = true;
  CHECK_EQ(u2ps_dwc2_phy_start(), U2PS_DWC2_ERR_CLOCK);
  flush();
  CHECK_EQ(first(DWT_LAR_A), -1);
  CHECK((reg(DWT_CTRL_A) & 1u) != 0u);
  CHECK(first(U5_RCC_CR) >= 0);
  CHECK_EQ(violations, 0);
}
#endif

int main(void)
{
#if defined(TEST_H7S3) || defined(TEST_H7S3_16)
  test_h7s3_ok();
  test_h7s3_reset_state();
  test_h7s3_stuck();
  test_h7s3_no_hse();
  test_h7s3_no_usb33();
#elif defined(TEST_F723)
  test_f723_cycles();
  test_f723_tune();
#elif defined(TEST_U5A5)
  test_u5_cycles();
#endif
  return check_done();
}
