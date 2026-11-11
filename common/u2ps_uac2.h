/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_uac2.h: the USB Audio Class 2.0 constants that the stacks use.
 * References: "UAC2" is the USB Device Class Definition for Audio Devices,
 * Release 2.0 (May 31, 2006); "Formats" is USB Device Class Definition for
 * Audio Data Formats, Release 2.0; "Terminal Types" is USB Device Class
 * Definition for Terminal Types, Release 2.0; "IAD" is the Interface
 * Association Descriptor ECN to USB 2.0.
 */
#ifndef U2PS_UAC2_H
#define U2PS_UAC2_H

/* Interface association descriptor: type INTERFACE_ASSOCIATION, 8 bytes
 * (IAD, its addition to the descriptor types and Table 9-Z) */
#define U2PS_DESC_IAD 11u
#define U2PS_IAD_LEN  8u

/* Device class codes of a device whose function sits at the interface level
 * and is described by an IAD (UAC2 4.2, 4.3) */
#define U2PS_DEVICE_CLASS_MISC     0xEFu
#define U2PS_DEVICE_SUBCLASS_IAD   0x02u
#define U2PS_DEVICE_PROTOCOL_IAD   0x01u

/* Function, interface class, subclass and protocol codes (UAC2 A.1-A.6).
 * An audio interface of USB Audio Class 1.0 has protocol 0 (UAC2 A.6:
 * INTERFACE_PROTOCOL_UNDEFINED). */
#define U2PS_AUDIO                 0x01u /* AUDIO_FUNCTION and AUDIO */
#define U2PS_FUNCTION_SUBCLASS_UNDEFINED 0x00u
#define U2PS_SUBCLASS_AUDIOCONTROL   0x01u
#define U2PS_SUBCLASS_AUDIOSTREAMING 0x02u
#define U2PS_IP_UNDEFINED          0x00u
#define U2PS_IP_VERSION_02_00      0x20u /* also AF_VERSION_02_00 */

/* Class-specific descriptor types (UAC2 A.8) */
#define U2PS_CS_INTERFACE 0x24u
#define U2PS_CS_ENDPOINT  0x25u

/* AudioControl interface descriptor subtypes (UAC2 A.9) */
#define U2PS_AC_HEADER                0x01u
#define U2PS_AC_INPUT_TERMINAL        0x02u
#define U2PS_AC_OUTPUT_TERMINAL       0x03u
#define U2PS_AC_MIXER_UNIT            0x04u
#define U2PS_AC_SELECTOR_UNIT         0x05u
#define U2PS_AC_FEATURE_UNIT          0x06u
#define U2PS_AC_EFFECT_UNIT           0x07u
#define U2PS_AC_PROCESSING_UNIT       0x08u
#define U2PS_AC_EXTENSION_UNIT        0x09u
#define U2PS_AC_CLOCK_SOURCE          0x0Au
#define U2PS_AC_CLOCK_SELECTOR        0x0Bu
#define U2PS_AC_CLOCK_MULTIPLIER      0x0Cu
#define U2PS_AC_SAMPLE_RATE_CONVERTER 0x0Du

/* AudioStreaming interface descriptor subtypes (UAC2 A.10) */
#define U2PS_AS_GENERAL     0x01u
#define U2PS_AS_FORMAT_TYPE 0x02u

/* Class-specific endpoint descriptor subtype (UAC2 A.13) */
#define U2PS_EP_GENERAL 0x01u

/* Descriptor lengths (UAC2 Tables 4-5, 4-6, 4-9, 4-10, 4-27, 4-34;
 * Formats Table 2-2). A Clock Selector has 7 bytes plus one per input pin
 * (Table 4-7); a Feature Unit 6 + (ch + 1) * 4, so at least 10 (Table
 * 4-13). */
#define U2PS_AC_HEADER_LEN      9u
#define U2PS_CLOCK_SOURCE_LEN   8u
#define U2PS_CLOCK_SELECTOR_LEN 7u
#define U2PS_INPUT_TERM_LEN     17u
#define U2PS_OUTPUT_TERM_LEN    12u
#define U2PS_FEATURE_UNIT_LEN   10u
#define U2PS_AS_GENERAL_LEN     16u
#define U2PS_FORMAT_I_LEN       6u
#define U2PS_CS_EP_LEN          8u

/* bcdADC of this release (UAC2 Table 4-5) */
#define U2PS_BCD_ADC 0x0200u

/* Clock Source bmAttributes D1..0 and bmControls bit pairs (UAC2 4.7.2.1,
 * Table 4-6): a pair is 0b01 for a read-only Control, 0b11 for a Host
 * programmable one */
#define U2PS_CLOCK_INTERNAL_PROGRAMMABLE 0x03u
#define U2PS_CTRL_READ        0x1u
#define U2PS_CTRL_READ_WRITE  0x3u
#define U2PS_CLOCK_FREQ_POS   0u /* D1..0: Clock Frequency Control */
#define U2PS_CLOCK_VALID_POS  2u /* D3..2: Clock Validity Control */

/* Terminal type of a USB OUT endpoint (Terminal Types 2.1, Table 2-1) */
#define U2PS_TERMINAL_USB_STREAMING 0x0101u

/* Format type and Type I format bits (Formats A.1, A.2.1, Table A-2) */
#define U2PS_FORMAT_TYPE_I     0x01u
#define U2PS_FORMAT_PCM        0x00000001u /* bmFormats D0 */
#define U2PS_FORMAT_IEEE_FLOAT 0x00000004u /* bmFormats D2 */

/* Endpoint bmAttributes (USB 2.0 Table 9-13; UAC2 Tables 4-33, 4-35):
 * D1..0 transfer type, D3..2 synchronization type, D5..4 usage type */
#define U2PS_EP_ISO          0x01u /* D1..0 */
#define U2PS_EP_ASYNC        0x04u /* D3..2 = 01 */
#define U2PS_EP_USAGE_FB     0x10u /* D5..4 = 01 */
#define U2PS_EP_TYPE_MASK    0x03u
#define U2PS_EP_SYNC_POS     2u
#define U2PS_EP_USAGE_POS    4u
#define U2PS_EP_USAGE_DATA     0u
#define U2PS_EP_USAGE_FEEDBACK 1u
#define U2PS_EP_USAGE_IMPLICIT 2u /* implicit feedback data endpoint */
#define U2PS_EP_DIR_IN       0x80u /* bEndpointAddress D7 */

/* Class-specific AS isochronous audio data endpoint bmAttributes (UAC2
 * Table 4-34) */
#define U2PS_EP_MAX_PACKETS_ONLY 0x80u /* D7 */

/* Class-specific request codes (UAC2 A.14) */
#define U2PS_REQ_CUR   0x01u
#define U2PS_REQ_RANGE 0x02u

/* Clock Source Control Selectors (UAC2 A.17.1) */
#define U2PS_CS_SAM_FREQ_CONTROL    0x01u
#define U2PS_CS_CLOCK_VALID_CONTROL 0x02u

#endif /* U2PS_UAC2_H */
