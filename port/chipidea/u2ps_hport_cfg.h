/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_hport_cfg.h: the ChipIdea host port's configuration header, with the
 * constants every host port's header defines (u2ps_hport.h), for the
 * i.MX RT1060. The host port itself is not built yet; these constants let
 * the host library build. Each value is inferred from IMXRT1060RM Rev. 3,
 * chapter 42.
 */
#ifndef U2PS_HPORT_CFG_H
#define U2PS_HPORT_CFG_H

/* Alignment of a queued packet, in bytes: an iTD transaction addresses its
 * data by a page pointer and a 12-bit offset, so any byte (42.5.2.3;
 * inferred) */
#define U2PS_HPORT_ALIGN 1u

/* Microframes ahead the port arms a packet: the Isochronous Scheduling
 * Threshold, IST 0 in HCCPARAMS (p. 2451), gives 2 (inferred) */
#define U2PS_HPORT_LEAD 2u

/* on_sent's microframe numbers are exact: the frame list index and the
 * iTD's transaction slot (inferred) */
#define U2PS_HPORT_UFRAME_EXACT 1

#endif /* U2PS_HPORT_CFG_H */
