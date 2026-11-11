/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_host_dma.c: the host stack's DMA memory at u2ps_host_init(). The
 * stack asks the port first, before anything else, whether its SETUP
 * packet, its control buffer and the port's memory lie where the port's
 * DMA reaches them (u2ps_hport_dma_check); if not, the init returns
 * U2PS_HOST_ERR_DMA_MEMORY and changes nothing: no port start, no end of
 * the session. The stack zeroes that memory at every init, so section
 * .bss.u2ps_dma needs no zeroing at startup. The fake port reports a
 * control transfer outside the memory checked as a port violation.
 */
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "host_support.h"

static bool all(const uint8_t *p, size_t n, uint8_t v)
{
  for (size_t i = 0; i < n; i++)
    if (p[i] != v)
      return false;
  return true;
}

/* Bytes in all the ranges the last init checked */
static uint32_t checked_bytes(void)
{
  uint32_t n = 0;
  for (unsigned i = 0; i < u2ps_hfake_dma_count; i++)
    n += u2ps_hfake_dma[i].len;
  return n;
}

static void fill_checked(uint8_t v)
{
  for (unsigned i = 0; i < u2ps_hfake_dma_count; i++)
    memset((void *)(uintptr_t)u2ps_hfake_dma[i].mem, v, u2ps_hfake_dma[i].len);
}

static bool checked_all(uint8_t v)
{
  for (unsigned i = 0; i < u2ps_hfake_dma_count; i++)
    if (!all(u2ps_hfake_dma[i].mem, u2ps_hfake_dma[i].len, v))
      return false;
  return true;
}

static bool stream_started(void)
{
  return happ_log.stream_starts > 0u || happ_log.stream_stops > 0u;
}

/* Before the first start: refused, nothing started; then started. The
 * memory checked holds a SETUP packet and the whole control buffer. */
static void test_first_init(void)
{
  u2ps_hfake_reset_all();
  hmodel_load("uac2-hs-default.txt");
  u2ps_hfake_device = u2ps_hmodel_answer;
  u2ps_host_vbus(true);
  happ_clear();
  u2ps_hfake_dma_ok = false;
  CHECK_EQ(u2ps_host_init(), U2PS_HOST_ERR_DMA_MEMORY);
  CHECK(u2ps_hfake_count(U2PS_HFAKE_DMA_CHECK) >= 1u);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_INIT), 0);
  CHECK_EQ(u2ps_hfake_get(U2PS_HFAKE_DMA_CHECK, 0).from, U2PS_HFAKE_FROM_THREAD);
  CHECK_EQ(u2ps_hfake_masks, 0);
  /* Not started: an attach does nothing, and is a contract violation */
  contract_expect(true);
  u2ps_host_connect(true);
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_HOST_CONNECT_BEFORE_INIT], 1);
  contract_expect(false);
  CHECK_EQ(u2ps_hfake_count(U2PS_HFAKE_RESET), 0);

  u2ps_hfake_dma_ok = true;
  u2ps_hfake_clear_log();
  CHECK_EQ(u2ps_host_init(), U2PS_HOST_OK);
  unsigned checks = u2ps_hfake_count(U2PS_HFAKE_DMA_CHECK);
  CHECK(checks >= 1u);
  /* Every check comes before the start */
  CHECK_EQ(u2ps_hfake_find(U2PS_HFAKE_INIT, 0), (int)checks);
  /* A SETUP packet and the control buffer, at least */
  CHECK(checked_bytes() >= 8u + U2PS_HOST_EP0_BUF);
  CHECK(checked_all(0));
  /* An enumeration's control transfers all lie in the memory checked:
   * the fake reports any other as a port violation */
  u2ps_host_connect(true);
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  CHECK(u2ps_hfake_count(U2PS_HFAKE_CONTROL) > 5u);
  CHECK_EQ(u2ps_hfake_violations, 0);
}

/* A re-init during a session: refused, nothing changes (no on_end, no
 * port call but the check, the memory as it was); accepted, it zeroes the
 * memory, ends the session and starts the port again */
static void test_reinit(void)
{
  CHECK(hstart("uac2-hs-default.txt"));
  CHECK(henumerate());
  CHECK_EQ(happ_log.devices, 1);
  fill_checked(0x5A);
  happ_clear();
  u2ps_hfake_clear_log();
  unsigned masks = u2ps_hfake_masks;
  u2ps_hfake_dma_ok = false;
  CHECK_EQ(u2ps_host_init(), U2PS_HOST_ERR_DMA_MEMORY);
  CHECK_EQ(u2ps_hfake_ncalls, u2ps_hfake_count(U2PS_HFAKE_DMA_CHECK));
  CHECK_EQ(u2ps_hfake_masks, masks);
  CHECK_EQ(happ_log.ends, 0);
  CHECK(checked_all(0x5A));
  /* The session goes on: a stream starts, its requests in the memory
   * checked */
  u2ps_hfake_dma_ok = true;
  CHECK(u2ps_host_stream_start(1, 48000) != 0u);
  CHECK(u2ps_hfake_wait(stream_started, 100));
  CHECK_EQ(happ_log.stream_starts, 1);

  u2ps_hfake_clear_log();
  fill_checked(0x5A);
  CHECK_EQ(u2ps_host_init(), U2PS_HOST_OK);
  CHECK_EQ(u2ps_hfake_find(U2PS_HFAKE_DMA_CHECK, 0), 0);
  CHECK_EQ(happ_log.ends, 1);
  CHECK_EQ(happ_log.end, U2PS_HOST_END_REINIT);
  CHECK(u2ps_hfake_find(U2PS_HFAKE_INIT, 0) > u2ps_hfake_find(U2PS_HFAKE_DMA_CHECK, 0));
  CHECK(checked_all(0));
  CHECK_EQ(u2ps_hfake_violations, 0);
}

int main(void)
{
  test_first_init();
  test_reinit();
  return check_done();
}
