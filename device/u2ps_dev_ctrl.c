/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dev_ctrl.c: the control pipe on EP0: SETUP, data and status stages,
 * request errors, deferred replies, and the routing of class requests to the
 * audio function and vendor requests to the app. References are to the USB 2.0 Specification,
 * Revision 2.0.
 *
 * Every request ends in one of: a STALL (request error, 9.2.7), a data
 * stage followed by the status stage, or the status stage alone. A deferred
 * request waits in U2PS_CTRL_PENDING, with EP0 NAKing, until
 * u2ps_dev_reply(); SET_INTERFACE's status waits in U2PS_CTRL_HELD for the
 * stream's open (u2ps_dev_audio.c). A new SETUP or a bus reset cancels
 * either. Built with
 * U2PS_CONTROL_HOOK, the app hears of each answer the stack arms.
 */
#include <stddef.h>

#include "u2ps_ch9.h"
#include "u2ps_dev_priv.h"

void u2ps_dev_ctrl_reset(void)
{
  u2ps_dev.id++;
  u2ps_dev.stage = U2PS_CTRL_IDLE;
  u2ps_dev.after = U2PS_AFTER_NONE;
  u2ps_dev.rate_deferred = false;
}

/* A class or vendor request to an interface that exists: only in the
 * Configured state (9.4, 9.6.5) */
static bool is_itf_request(const u2ps_setup_t *s)
{
  uint8_t type = s->bmRequestType & U2PS_REQ_TYPE_MASK;
  if (type != U2PS_REQ_TYPE_CLASS && type != U2PS_REQ_TYPE_VENDOR)
    return false;
  if ((s->bmRequestType & U2PS_REQ_RCPT_MASK) != U2PS_REQ_RCPT_ITF ||
      u2ps_dev.state != U2PS_DEV_CONFIGURED)
    return false;
  return (s->wIndex & 0xFFu) < u2ps_dev_itf_count();
}

/* A class or vendor request to an interface: the audio function answers
 * class requests, the app vendor requests. A negative result of the app
 * other than STALL and DEFER is a request error too: it must never reach
 * the stack's own U2PS_REQ_HOLD. */
static int itf_request(uint16_t len)
{
  u2ps_request_t req = {
    .setup = u2ps_dev.setup,
    .buf = u2ps_dev_ep0_buf,
    .size = U2PS_EP0_BUF,
    .len = len,
    .id = u2ps_dev.id,
  };
  if ((req.setup.bmRequestType & U2PS_REQ_TYPE_MASK) == U2PS_REQ_TYPE_CLASS)
    return u2ps_dev_class_request(&req);
  int r = U2PS_DEV_REQUEST_RESULT(u2ps_app_on_request(&req));
  return r < 0 && r != U2PS_REQ_DEFER ? U2PS_REQ_STALL : r;
}

void u2ps_dev_ctrl_answer(int result)
{
  const u2ps_setup_t *s = &u2ps_dev.setup;

  if (result == U2PS_REQ_DEFER) {
    u2ps_dev.stage = U2PS_CTRL_PENDING;
    return;
  }
  if (result == U2PS_REQ_HOLD) {
    u2ps_dev.stage = U2PS_CTRL_HELD;
    u2ps_dev.held_timed = false;
    return;
  }
  if (result < 0) {
    u2ps_dev.stage = U2PS_CTRL_IDLE;
    u2ps_dev.after = U2PS_AFTER_NONE;
    u2ps_dport_ep0_stall();
    U2PS_DEV_ON_CONTROL(s, U2PS_REQ_STALL);
    return;
  }
  if ((s->bmRequestType & U2PS_REQ_DIR_IN) && s->wLength > 0u) {
    /* Never more than wLength (9.3.5). A shorter answer ends with a short
     * packet; if it is a whole number of packets, that is a zero-length
     * packet (9.4.3, 8.5.3.2). */
    uint16_t n = (uint16_t)result;
    if (n > s->wLength)
      n = s->wLength;
    if (n > U2PS_EP0_BUF)
      n = U2PS_EP0_BUF;
    bool zlp = n > 0u && n < s->wLength && n % U2PS_EP0_MPS == 0u;
    u2ps_dev.stage = U2PS_CTRL_DATA_IN;
    u2ps_dport_ep0_in(u2ps_dev_ep0_buf, n, zlp);
    U2PS_DEV_ON_CONTROL(s, n);
    return;
  }
  /* No data stage, or the host's data has arrived: the status stage. The
   * port takes a new address before this status stage and applies it on the
   * bus after it (9.4.6). */
  if (u2ps_dev.after == U2PS_AFTER_ADDRESS)
    u2ps_dport_set_address(u2ps_dev.after_arg);
  u2ps_dev.stage = U2PS_CTRL_STATUS;
  u2ps_dport_ep0_status(true);
  U2PS_DEV_ON_CONTROL(s, 0);
}

/* A new SETUP or a reset has ended the hold if the stage is no longer
 * held: then the open arms nothing for the old request */
void u2ps_dev_ctrl_held_answer(int result)
{
  if (u2ps_dev.stage == U2PS_CTRL_HELD && result != U2PS_REQ_HOLD)
    u2ps_dev_ctrl_answer(result);
}

static void setup_request(const uint8_t setup[8])
{
  u2ps_setup_t *s = &u2ps_dev.setup;

  u2ps_dev_ctrl_reset();
  s->bmRequestType = setup[0];
  s->bRequest = setup[1];
  s->wValue = u2ps_dev_get16(setup + 2);
  s->wIndex = u2ps_dev_get16(setup + 4);
  s->wLength = u2ps_dev_get16(setup + 6);

  /* Host-to-device with data: only class and vendor requests to an
   * interface take data. The app sees the request once its data is in. */
  if (!(s->bmRequestType & U2PS_REQ_DIR_IN) && s->wLength > 0u) {
    if (!is_itf_request(s) || s->wLength > U2PS_EP0_BUF) {
      u2ps_dev_ctrl_answer(U2PS_REQ_STALL);
      return;
    }
    u2ps_dev.stage = U2PS_CTRL_DATA_OUT;
    u2ps_dport_ep0_out(u2ps_dev_ep0_buf, s->wLength);
    return;
  }

  int result;
  if ((s->bmRequestType & U2PS_REQ_TYPE_MASK) == U2PS_REQ_TYPE_STD)
    result = u2ps_dev_std_request(s);
  else if (is_itf_request(s))
    result = itf_request(0);
  else
    result = U2PS_REQ_STALL;
  u2ps_dev_ctrl_answer(result);
}

void u2ps_dev_on_setup(const uint8_t setup[8])
{
  if (!U2PS_DEV_VBUS_ON())
    return;
  u2ps_dev.in_upcall = true;
  setup_request(setup);
  u2ps_dev.in_upcall = false;
}

void u2ps_dev_on_ep0_data(uint16_t len)
{
  if (!U2PS_DEV_VBUS_ON())
    return;
  u2ps_dev.in_upcall = true;
  if (u2ps_dev.stage == U2PS_CTRL_DATA_IN) {
    /* Control read: accept the host's zero-length status packet */
    u2ps_dev.stage = U2PS_CTRL_STATUS;
    u2ps_dport_ep0_status(false);
  } else if (u2ps_dev.stage == U2PS_CTRL_DATA_OUT) {
    /* wLength is the exact length of the host's data (9.3.5): a shorter
     * data stage is a request error */
    u2ps_dev_ctrl_answer(len == u2ps_dev.setup.wLength ? itf_request(len) : U2PS_REQ_STALL);
  }
  u2ps_dev.in_upcall = false;
}

void u2ps_dev_on_ep0_status(void)
{
  if (!U2PS_DEV_VBUS_ON() || u2ps_dev.stage != U2PS_CTRL_STATUS)
    return;
  u2ps_after_t after = u2ps_dev.after;
  u2ps_dev.stage = U2PS_CTRL_IDLE;
  u2ps_dev.after = U2PS_AFTER_NONE;

  /* SET_ADDRESS changed the state at its SETUP already: this completion
   * may be reported only after the next SETUP, which discards 'after' */
  if (after == U2PS_AFTER_TEST_MODE) {
    /* Not before the status stage, and within 3 ms after it (9.4.9) */
    u2ps_dport_test_mode(u2ps_dev.after_arg);
  }
}
