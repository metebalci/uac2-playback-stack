/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_ch9.h: the USB 2.0 Chapter 9 constants that the stacks use.
 * All references are to the USB 2.0 Specification, Revision 2.0.
 */
#ifndef U2PS_CH9_H
#define U2PS_CH9_H

/* SETUP packet: 8 bytes (9.3, Table 9-2) */
#define U2PS_SETUP_LEN 8u

/* bmRequestType (9.3.1) */
#define U2PS_REQ_DIR_IN       0x80u /* D7: device-to-host */
#define U2PS_REQ_TYPE_MASK    0x60u /* D6..5 */
#define U2PS_REQ_TYPE_STD     0x00u
#define U2PS_REQ_TYPE_CLASS   0x20u
#define U2PS_REQ_TYPE_VENDOR  0x40u
#define U2PS_REQ_RCPT_MASK    0x1Fu /* D4..0 */
#define U2PS_REQ_RCPT_DEVICE  0x00u
#define U2PS_REQ_RCPT_ITF     0x01u
#define U2PS_REQ_RCPT_EP      0x02u

/* Standard request codes (9.4, Table 9-4) */
#define U2PS_GET_STATUS        0u
#define U2PS_CLEAR_FEATURE     1u
#define U2PS_SET_FEATURE       3u
#define U2PS_SET_ADDRESS       5u
#define U2PS_GET_DESCRIPTOR    6u
#define U2PS_SET_DESCRIPTOR    7u
#define U2PS_GET_CONFIGURATION 8u
#define U2PS_SET_CONFIGURATION 9u
#define U2PS_GET_INTERFACE     10u
#define U2PS_SET_INTERFACE     11u
#define U2PS_SYNCH_FRAME       12u

/* Descriptor types (9.4, Table 9-5) */
#define U2PS_DESC_DEVICE           1u
#define U2PS_DESC_CONFIGURATION    2u
#define U2PS_DESC_STRING           3u
#define U2PS_DESC_INTERFACE        4u
#define U2PS_DESC_ENDPOINT         5u
#define U2PS_DESC_DEVICE_QUALIFIER 6u
#define U2PS_DESC_OTHER_SPEED      7u

/* Descriptor lengths (9.6.1 Table 9-8, 9.6.2 Table 9-9, 9.6.3 Table 9-10,
 * 9.6.5, 9.6.6) */
#define U2PS_DEVICE_LEN    18u
#define U2PS_QUALIFIER_LEN 10u
#define U2PS_CONFIG_LEN    9u
#define U2PS_ITF_LEN       9u
#define U2PS_EP_LEN        7u

/* Configuration bmAttributes (9.6.3, Table 9-10) */
#define U2PS_CONFIG_SELF_POWERED  0x40u /* D6 */
#define U2PS_CONFIG_REMOTE_WAKEUP 0x20u /* D5 */

/* Feature selectors (9.4, Table 9-6) */
#define U2PS_FEATURE_ENDPOINT_HALT 0u
#define U2PS_FEATURE_REMOTE_WAKEUP 1u
#define U2PS_FEATURE_TEST_MODE     2u

/* GET_STATUS of a device (9.4.5, Figure 9-4) */
#define U2PS_STATUS_SELF_POWERED  0x01u
#define U2PS_STATUS_REMOTE_WAKEUP 0x02u

/* Test mode selectors (9.4.9, Table 9-7). Test_Force_Enable (5) applies to
 * downstream facing hub ports only (7.1.20). */
#define U2PS_TEST_J           1u
#define U2PS_TEST_K           2u
#define U2PS_TEST_SE0_NAK     3u
#define U2PS_TEST_PACKET      4u
#define U2PS_TEST_FORCE_ENABLE 5u

/* Highest device address (9.4.6) */
#define U2PS_ADDRESS_MAX 127u

#endif /* U2PS_CH9_H */
