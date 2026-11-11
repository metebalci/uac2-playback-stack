/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * fake_regs.h: register access of the DWC2 start-up code
 * (port/dwc2/common/u2ps_dwc2_phy.c) redirected to a register model, for
 * the PC test test_dwc2_phy.c. The test's build includes it ahead of every
 * source (-include), so u2ps_dwc2_regs.h keeps these definitions of
 * U2PS_DWC2_R32 and U2PS_DWC2_R16. Each access first calls the model with
 * its address and width; the access itself then goes to the model's
 * storage, so the model sees a write at the next access.
 */
#ifndef FAKE_REGS_H
#define FAKE_REGS_H

#include <stdint.h>

uint32_t *fake_reg32(uintptr_t addr);
uint16_t *fake_reg16(uintptr_t addr);

#define U2PS_DWC2_R32(addr) (*(volatile uint32_t *)fake_reg32((uintptr_t)(addr)))
#define U2PS_DWC2_R16(addr) (*(volatile uint16_t *)fake_reg16((uintptr_t)(addr)))

#endif /* FAKE_REGS_H */
