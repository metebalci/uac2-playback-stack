/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_chipidea_dcd.c: ChipIdea device port.
 * No register code yet: this file defines none of the port interface's
 * functions (u2ps_dport.h), so an app for the i.MX RT1062 that links the
 * device library does not link yet. The port's decision logic is in
 * u2ps_chipidea_iso.c; this file checks that the port's configuration
 * header agrees with it.
 *
 * For the register code to come: u2ps_dport_connect(false) ends test mode
 * as well as clearing USBCMD.RS (u2ps_dport.h): PORTSC1.PTC = 0000b,
 * TEST_MODE_DISABLE (RM 42.7.31, p. 2474; RM is IMXRT1060RM Rev. 3).
 * u2ps_dport_report_holes() reports u2ps_chipidea_out_tail() while the
 * stream is open, and writes no register (u2ps_chipidea_iso.h).
 */
#include "u2ps_chipidea_iso.h"
#include "u2ps_dport_cfg.h"

_Static_assert(U2PS_DPORT_ISO_OUT_MIN == U2PS_CHIPIDEA_ISO_OUT_MIN &&
                   U2PS_DPORT_ISO_OUT_RESERVE == U2PS_CHIPIDEA_ISO_OUT_RESERVE,
               "the least buffer count and the reserve are the logic's");
_Static_assert(U2PS_DPORT_FB_INTERVAL_MIN == U2PS_CHIPIDEA_FB_INTERVAL_MIN &&
                   U2PS_DPORT_FB_INTERVAL_MAX == U2PS_CHIPIDEA_FB_INTERVAL_MAX,
               "the feedback interval limits are the logic's");
_Static_assert(U2PS_CHIPIDEA_FN_UNIT == U2PS_CHIPIDEA_FN_NONE ||
                   U2PS_CHIPIDEA_FN_UNIT == U2PS_CHIPIDEA_FN_UFRAMES ||
                   U2PS_CHIPIDEA_FN_UNIT == U2PS_CHIPIDEA_FN_FRAMES,
               "U2PS_CHIPIDEA_FN_UNIT must be 0, 1 or 2: set the CMake option "
               "-DU2PS_CHIPIDEA_FN_UNIT=none, uframes or frames");
_Static_assert(U2PS_DPORT_UFRAME_EXACT == (U2PS_CHIPIDEA_FN_UNIT == U2PS_CHIPIDEA_FN_UFRAMES),
               "microframe numbers are exact only from a Frame Number in microframes");
_Static_assert(U2PS_ISO_OUT_COUNT_MAX >= U2PS_DPORT_ISO_OUT_MIN,
               "U2PS_ISO_OUT_COUNT_MAX must be at least U2PS_DPORT_ISO_OUT_MIN (3 on ChipIdea), "
               "or no stream opens: set -DU2PS_ISO_OUT_COUNT_MAX=3 or more (default 16)");
_Static_assert(U2PS_DPORT_STRIDE(1024u) >= 1024u && U2PS_DPORT_STRIDE(1u) == U2PS_DPORT_ALIGN,
               "a stride holds a packet and keeps the alignment");
