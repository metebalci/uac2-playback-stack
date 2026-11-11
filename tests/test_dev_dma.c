/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * test_dev_dma.c: the device stack's DMA memory at u2ps_dev_init(). The
 * stack asks the port first, before anything else, whether its control
 * buffer and the port's memory lie where the port's DMA reaches them
 * (u2ps_dport_dma_check); if not, the init returns U2PS_DEV_ERR_DMA_MEMORY
 * and changes nothing: no port start, no detach, the session goes on. The
 * stack zeroes its control buffer at every init, so section
 * .bss.u2ps_dma needs no zeroing at startup.
 */
#include <string.h>

#include "check.h"
#include "contract_log.h"
#include "dev_support.h"

extern uint8_t u2ps_dev_ep0_buf[U2PS_EP0_BUF];

static bool all(const uint8_t *p, size_t n, uint8_t v)
{
  for (size_t i = 0; i < n; i++)
    if (p[i] != v)
      return false;
  return true;
}

/* The check names exactly the control buffer, every byte of it */
static void check_range(void)
{
  CHECK(u2ps_fake_dma_mem == (const void *)u2ps_dev_ep0_buf);
  CHECK_EQ(u2ps_fake_dma_len, U2PS_EP0_BUF);
}

/* Before the first start: refused, nothing started, the buffer untouched;
 * then started, the buffer zeroed */
static void test_first_init(void)
{
  u2ps_fake_reset_all();
  memset(u2ps_dev_ep0_buf, 0xA5, U2PS_EP0_BUF);
  u2ps_fake_dma_ok = false;
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_ERR_DMA_MEMORY);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_DMA_CHECK), 1);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_DMA_CHECK, 0).from, U2PS_FAKE_FROM_THREAD);
  check_range();
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_INIT), 0);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_IRQ_MASK), 0);
  CHECK_EQ(u2ps_fake_ncalls, 1);
  CHECK_EQ(u2ps_dev_description_error(), U2PS_DESCRIPTION_OK);
  CHECK(all(u2ps_dev_ep0_buf, U2PS_EP0_BUF, 0xA5));
  /* Not started: an attach does nothing, and is a contract violation */
  contract_expect(true);
  u2ps_dev_connect(true);
  CHECK_EQ(contract_log.count[U2PS_CONTRACT_DEV_CONNECT_BEFORE_INIT], 1);
  contract_expect(false);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_CONNECT), 0);
  CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_NONE);

  /* A description error comes first: the memory is not asked about */
  u2ps_fake_reset_all();
  u2ps_fake_dma_ok = false;
  u2ps_dev_description_t bad = test_desc;
  bad.format_count = 0;
  CHECK_EQ(u2ps_dev_init(&bad), U2PS_DEV_ERR_DESCRIPTION);
  CHECK_EQ(u2ps_fake_ncalls, 0);

  u2ps_fake_reset_all();
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_DMA_CHECK), 1);
  check_range();
  /* The check is the first port call, the start comes after it */
  CHECK_EQ(u2ps_fake_find(U2PS_FAKE_DMA_CHECK, 0), 0);
  CHECK_EQ(u2ps_fake_find(U2PS_FAKE_INIT, 0), 1);
  CHECK(all(u2ps_dev_ep0_buf, U2PS_EP0_BUF, 0));
  CHECK_EQ(u2ps_fake_violations, 0);
}

/* A re-init while configured: refused, nothing changes (still attached,
 * configured, the buffer as it was); accepted, it detaches after the
 * check, zeroes the buffer and re-inits the port */
static void test_reinit(void)
{
  dev_start(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(app_log.configured, 1);
  static uint8_t before[U2PS_EP0_BUF];
  memset(u2ps_dev_ep0_buf, 0x5A, 64);
  memcpy(before, u2ps_dev_ep0_buf, sizeof before);
  u2ps_fake_clear_log();
  u2ps_fake_dma_ok = false;
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_ERR_DMA_MEMORY);
  CHECK_EQ(u2ps_fake_ncalls, 1);
  CHECK_EQ(u2ps_fake_get(U2PS_FAKE_DMA_CHECK, 0).from, U2PS_FAKE_FROM_THREAD);
  check_range();
  CHECK(u2ps_fake_attached);
  CHECK_EQ(app_log.unconfigured, 0);
  CHECK_EQ(u2ps_dev_speed(), U2PS_SPEED_HIGH);
  CHECK(memcmp(before, u2ps_dev_ep0_buf, sizeof before) == 0);
  /* The session goes on: the host still reads its configuration */
  uint8_t cfg[2];
  uint16_t n = 0;
  CHECK_EQ(u2ps_fake_host_control(0x80, 8, 0, 0, 2, cfg, &n), U2PS_FAKE_HOST_OK);
  CHECK_EQ(n, 1);
  CHECK_EQ(cfg[0], 1);

  u2ps_fake_dma_ok = true;
  u2ps_fake_clear_log();
  memset(u2ps_dev_ep0_buf, 0x5A, U2PS_EP0_BUF);
  CHECK_EQ(u2ps_dev_init(&test_desc), U2PS_DEV_OK);
  CHECK_EQ(u2ps_fake_find(U2PS_FAKE_DMA_CHECK, 0), 0);
  int detach = u2ps_fake_find(U2PS_FAKE_CONNECT, 0);
  CHECK(detach > 0);
  CHECK(u2ps_fake_find(U2PS_FAKE_REINIT, 0) > detach);
  CHECK_EQ(u2ps_fake_count(U2PS_FAKE_INIT), 0);
  CHECK_EQ(app_log.unconfigured, 1);
  CHECK(!u2ps_fake_attached);
  CHECK(all(u2ps_dev_ep0_buf, U2PS_EP0_BUF, 0));
  /* Every EP0 stage of the next session lies in the memory checked */
  u2ps_dev_connect(true);
  u2ps_fake_bus_reset(U2PS_SPEED_HIGH);
  dev_configure();
  CHECK_EQ(u2ps_fake_violations, 0);
}

int main(void)
{
  if (!test_desc_load())
    return EXIT_FAILURE;
  u2ps_fake_vbus_set(true);
  test_first_init();
  test_reinit();
  return check_done();
}
