/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_hport_cfg.h: the DWC2 host port's configuration header, with the
 * constants every host port's header defines (u2ps_hport.h), for the
 * STM32U5, the STM32H7S3 and the STM32F723, none of them run on a board
 * yet. Each value is inferred from RM0456 Rev 7; the
 * H7S3's OTG_HS registers are the same (RM0477 Rev 10, chapter 62). The
 * ARM build defines the chip's macro (u2ps_dwc2_chip.h).
 */
#ifndef U2PS_HPORT_CFG_H
#define U2PS_HPORT_CFG_H

/* Alignment of a queued packet, in bytes. RM0456 73.14.36 (OTG_HCDMAx,
 * p. 3363) states none; the DMA counts AHB transactions of 32 bits, so 4
 * (inferred; not yet measured on a board). The
 * same on the cached Cortex-M7 chips: the port only cleans a packet's
 * lines before its DMA reads them, which needs no alignment of its own
 * (inferred). */
#if defined(U2PS_CHIP_STM32U5A5) || defined(U2PS_CHIP_STM32F723) || defined(U2PS_CHIP_STM32H7S3)
#define U2PS_HPORT_ALIGN 4u
#else
#error "u2ps_hport_cfg.h (DWC2): none of U2PS_CHIP_STM32U5A5, U2PS_CHIP_STM32F723 and U2PS_CHIP_STM32H7S3 is defined: link u2ps_host or u2ps_dwc2_hcd from an ARM build with -DU2PS_CHIP=stm32u5a5, stm32f723 or stm32h7s3, which defines it"
#endif

/* The port arms a packet in microframe M for M+1: the channel's ODDFRM
 * selects the next microframe's parity (RM0456 pp. 3431-3432; inferred,
 * not yet measured on a board) */
#define U2PS_HPORT_LEAD 1u

/* on_sent's microframe numbers are exact: HFNUM.FRNUM (14 bits, p. 3355)
 * and the channel's ODDFRM, read at its halt (inferred) */
#define U2PS_HPORT_UFRAME_EXACT 1

/* The reach rule of u2ps_hport_dma_check() and of u2ps_hport_dma_ranges()
 * is u2ps_dwc2_dma_ok() (u2ps_dwc2.h), as for the device port
 * (u2ps_dport_cfg.h): on the STM32H7S3 the AXI SRAMs as the option bytes
 * map them and the AHB SRAMs with their clocks on, never DTCM, which is
 * the F723's default DMA memory */

#endif /* U2PS_HPORT_CFG_H */
