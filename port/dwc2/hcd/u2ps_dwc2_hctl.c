/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_dwc2_hctl.c: the DWC2 host port's root port and control channel
 * logic (u2ps_dwc2_hctl.h).
 *
 * Manual: RM0456 Rev 7, chapter 73.
 */
#include "u2ps_dwc2_hctl.h"

#include "u2ps_types.h"

#define HPRT_PTCTL_POS 13u
#define HPRT_PSPD_POS  17u

#define HCCHAR_DAD_POS  22u
#define HCCHAR_MCNT_1   (1u << 20)
#define HCCHAR_LSDEV    (1u << 17)
#define HCCHAR_EPDIR_IN (1u << 15)

#define HCTSIZ_DPID_DATA1 (2u << 29)
#define HCTSIZ_DPID_SETUP (3u << 29)
#define HCTSIZ_PKTCNT_POS 19u

uint32_t u2ps_dwc2_hprt_write(uint32_t hprt, uint32_t set, uint32_t clear)
{
  return (hprt & ~(U2PS_DWC2_HPRT_W1C | clear)) | set;
}

uint32_t u2ps_dwc2_hprt_ptctl(uint8_t selector)
{
  return selector >= 1u && selector <= 5u ? (uint32_t)selector << HPRT_PTCTL_POS : 0u;
}

u2ps_speed_t u2ps_dwc2_hprt_speed(uint32_t hprt)
{
  switch ((hprt & U2PS_DWC2_HPRT_PSPD_MSK) >> HPRT_PSPD_POS) {
  case 0u:
    return U2PS_SPEED_HIGH;
  case 1u:
    return U2PS_SPEED_FULL;
  case 2u:
    return U2PS_SPEED_LOW;
  default:
    return U2PS_SPEED_NONE;
  }
}

bool u2ps_dwc2_hprt_resumed(uint32_t hprt)
{
  return (hprt & U2PS_DWC2_HPRT_PRES) == 0u;
}

uint32_t u2ps_dwc2_hcchar_ep0(uint8_t addr, uint8_t mps0, bool in, u2ps_speed_t speed)
{
  return (uint32_t)(addr & 0x7Fu) << HCCHAR_DAD_POS | HCCHAR_MCNT_1 |
         (speed == U2PS_SPEED_LOW ? HCCHAR_LSDEV : 0u) | (in ? HCCHAR_EPDIR_IN : 0u) | mps0;
}

uint32_t u2ps_dwc2_hctsiz_ctl(u2ps_dwc2_stage_t stage, bool in, uint16_t len, uint8_t mps0)
{
  uint32_t whole = ((uint32_t)len + mps0 - 1u) / mps0; /* packets of len */
  uint32_t packets = whole == 0u ? 1u : whole;
  /* An IN stage reserves whole packets, at least one (73.14.35) */
  uint32_t size = in ? packets * mps0 : len;
  uint32_t pid = stage == U2PS_DWC2_STAGE_SETUP ? HCTSIZ_DPID_SETUP : HCTSIZ_DPID_DATA1;
  return pid | packets << HCTSIZ_PKTCNT_POS | size;
}

bool u2ps_dwc2_status_in(const uint8_t setup[8])
{
  bool data_in = (setup[0] & 0x80u) != 0u;
  uint16_t length = (uint16_t)(setup[6] | setup[7] << 8);
  return !(data_in && length > 0u);
}

/* ---- Port events ---------------------------------------------------------------- */

#define HPRT_CHANGES (U2PS_DWC2_HPRT_PCDET | U2PS_DWC2_HPRT_PENCHNG | U2PS_DWC2_HPRT_POCCHNG)

void u2ps_dwc2_hprt_event(uint32_t hprt, uint8_t state, u2ps_dwc2_hprt_event_t *e)
{
  *e = (u2ps_dwc2_hprt_event_t){.state = state, .speed = U2PS_SPEED_NONE};
  /* The change bits read, cleared; PSUSP (rs) written 0, which leaves it */
  e->ack = u2ps_dwc2_hprt_write(hprt, hprt & HPRT_CHANGES, U2PS_DWC2_HPRT_PSUSP);
  bool attached = (hprt & U2PS_DWC2_HPRT_PCSTS) != 0u;
  e->connect = (hprt & U2PS_DWC2_HPRT_PCDET) != 0u && attached;
  if ((hprt & U2PS_DWC2_HPRT_PENCHNG) == 0u)
    return;
  bool enabled = state == U2PS_DWC2_PORT_ENABLED || state == U2PS_DWC2_PORT_SUSPENDED ||
                 state == U2PS_DWC2_PORT_RESUMING || state == U2PS_DWC2_PORT_RESET_ENABLED;
  if ((hprt & U2PS_DWC2_HPRT_PENA) != 0u) {
    /* Enabled by the end of the reset the port ended; or while PRST is
     * still set, kept for the reset's end */
    if (state == U2PS_DWC2_PORT_RESET_END) {
      e->enabled = true;
      e->speed = u2ps_dwc2_hprt_speed(hprt);
      e->state = U2PS_DWC2_PORT_ENABLED;
    } else if (state == U2PS_DWC2_PORT_RESET) {
      e->speed = u2ps_dwc2_hprt_speed(hprt);
      e->state = U2PS_DWC2_PORT_RESET_ENABLED;
    }
  } else if (enabled) {
    /* Disabled by the hardware: with the device attached, port babble or
     * an overcurrent; without, a disconnect, which DISCINT reports */
    e->state = U2PS_DWC2_PORT_IDLE;
    e->disabled = attached;
    e->babble = attached && (hprt & U2PS_DWC2_HPRT_POCA) == 0u;
  }
}

uint8_t u2ps_dwc2_hprt_reset_end(uint8_t state, bool *now)
{
  *now = state == U2PS_DWC2_PORT_RESET_ENABLED;
  if (state == U2PS_DWC2_PORT_RESET)
    return U2PS_DWC2_PORT_RESET_END;
  return *now ? U2PS_DWC2_PORT_ENABLED : state;
}

/* ---- The stream's channels -------------------------------------------------------- */

#define HCCHAR_EPTYP_ISO (1u << 18)
#define HCCHAR_EPNUM_POS 11u
#define HCTSIZ_XFRSIZ_MSK 0x7FFFFu

uint32_t u2ps_dwc2_hcchar_iso(uint8_t addr, uint8_t ep, uint16_t mps, bool in)
{
  return (uint32_t)(addr & 0x7Fu) << HCCHAR_DAD_POS | HCCHAR_MCNT_1 | HCCHAR_EPTYP_ISO |
         (in ? HCCHAR_EPDIR_IN : 0u) | (uint32_t)(ep & 0xFu) << HCCHAR_EPNUM_POS |
         (mps & 0x7FFu);
}

U2PS_FAST_TEXT uint32_t u2ps_dwc2_hctsiz_iso(uint16_t len)
{
  return 1u << HCTSIZ_PKTCNT_POS | len; /* DPID 00: DATA0 */
}

U2PS_FAST_TEXT uint16_t u2ps_dwc2_hctsiz_in_len(uint32_t reserved, uint32_t hctsiz,
                                                uint16_t cap)
{
  uint32_t left = hctsiz & HCTSIZ_XFRSIZ_MSK;
  uint32_t n = left < reserved ? reserved - left : 0u;
  return (uint16_t)(n < cap ? n : cap);
}

/* ---- The control transfer ------------------------------------------------------------ */

#define HCTSIZ_DPID_MSK  (3u << 29)
#define HCTSIZ_PKTCNT_MSK (0x3FFu << HCTSIZ_PKTCNT_POS)

/* The halts that are transaction errors (RM0456 pp. 3409-3410, 3423);
 * DTERR restarts the count instead (p. 3411) */
#define HCINT_XACT_ERR (U2PS_DWC2_HCINT_TXERR | U2PS_DWC2_HCINT_BBERR | U2PS_DWC2_HCINT_FRMOR)

static void arm_stage(u2ps_dwc2_ctl_t *c, uint8_t stage, u2ps_dwc2_ctl_step_t *s)
{
  c->busy = true;
  c->stage = stage;
  s->what = U2PS_DWC2_CTL_ARM;
  s->stage = stage;
  s->offset = 0;
  if (stage == U2PS_DWC2_STAGE_SETUP) {
    c->armed = 8;
    s->in = false;
    s->hctsiz = u2ps_dwc2_hctsiz_ctl(U2PS_DWC2_STAGE_SETUP, false, 8, c->mps0);
  } else if (stage == U2PS_DWC2_STAGE_STATUS) {
    /* The opposite of the data stage; IN without one (USB 2.0 8.5.3) */
    c->armed = 0;
    s->in = !(c->data_in && c->length > 0u);
    s->hctsiz = u2ps_dwc2_hctsiz_ctl(U2PS_DWC2_STAGE_STATUS, s->in, 0, c->mps0);
  } else {
    /* The next part of the data stage, from c->offset: at most
     * U2PS_DWC2_PKTCNT_MAX packets, DATA1 after an even number of packets */
    uint32_t left = (uint32_t)c->length - c->offset;
    uint32_t most = U2PS_DWC2_PKTCNT_MAX * (uint32_t)c->mps0;
    c->armed = (uint16_t)(left < most ? left : most);
    s->in = c->data_in;
    s->offset = c->offset;
    s->hctsiz = u2ps_dwc2_hctsiz_ctl(stage, s->in, c->armed, c->mps0);
    if ((c->offset / c->mps0) % 2u != 0u)
      s->hctsiz &= ~HCTSIZ_DPID_MSK; /* DATA0 */
  }
}

/* The stage after the SETUP stage: the data stage, or the status stage
 * without one */
static uint8_t after_setup(const u2ps_dwc2_ctl_t *c)
{
  if (c->length == 0u)
    return U2PS_DWC2_STAGE_STATUS;
  return c->data_in ? U2PS_DWC2_STAGE_DATA_IN : U2PS_DWC2_STAGE_DATA_OUT;
}

static void ctl_done(u2ps_dwc2_ctl_t *c, u2ps_dwc2_ctl_result_t result, u2ps_dwc2_ctl_step_t *s)
{
  c->busy = false;
  c->aborting = false;
  s->what = U2PS_DWC2_CTL_DONE;
  s->result = (uint8_t)result;
  s->len = c->offset;
}

void u2ps_dwc2_ctl_start(u2ps_dwc2_ctl_t *c, const uint8_t setup[8], uint8_t mps0, bool setup_only,
                         u2ps_dwc2_ctl_step_t *s)
{
  *s = (u2ps_dwc2_ctl_step_t){.what = U2PS_DWC2_CTL_NONE};
  if (c->busy) {
    c->faults++;
    return;
  }
  c->setup_only = setup_only;
  c->held = false;
  c->aborting = false;
  c->data_in = (setup[0] & 0x80u) != 0u;
  c->length = (uint16_t)(setup[6] | setup[7] << 8);
  c->mps0 = mps0;
  c->errors = 0;
  c->offset = 0;
  arm_stage(c, U2PS_DWC2_STAGE_SETUP, s);
}

void u2ps_dwc2_ctl_data(u2ps_dwc2_ctl_t *c, u2ps_dwc2_ctl_step_t *s)
{
  *s = (u2ps_dwc2_ctl_step_t){.what = U2PS_DWC2_CTL_NONE};
  if (c->busy) {
    c->faults++;
    return;
  }
  if (!c->held) {
    c->faults++;
    c->offset = 0;
    s->what = U2PS_DWC2_CTL_DONE;
    s->result = U2PS_DWC2_CTL_ERROR;
    return;
  }
  c->held = false;
  c->setup_only = false;
  c->errors = 0;
  arm_stage(c, after_setup(c), s);
}

bool u2ps_dwc2_ctl_abort(u2ps_dwc2_ctl_t *c)
{
  if (!c->busy)
    return false;
  c->aborting = true;
  return true;
}

void u2ps_dwc2_ctl_drop(u2ps_dwc2_ctl_t *c)
{
  c->busy = c->held = c->aborting = false;
}

/* The bytes the part on the channel moved before its halt: an OUT part
 * whole packets by PKTCNT, an IN part by XFRSIZ; 'raw' the IN bytes
 * uncapped, which tell a short packet */
static uint16_t part_moved(const u2ps_dwc2_ctl_t *c, uint32_t hctsiz, uint32_t *raw)
{
  uint32_t packets = c->armed == 0u ? 1u : ((uint32_t)c->armed + c->mps0 - 1u) / c->mps0;
  uint32_t n;
  if (c->stage == U2PS_DWC2_STAGE_DATA_IN) {
    n = u2ps_dwc2_hctsiz_in_len(packets * c->mps0, hctsiz, 0xFFFFu);
  } else {
    uint32_t left = (hctsiz & HCTSIZ_PKTCNT_MSK) >> HCTSIZ_PKTCNT_POS;
    n = left < packets ? (packets - left) * c->mps0 : 0u;
  }
  *raw = n;
  return (uint16_t)(n < c->armed ? n : c->armed);
}

void u2ps_dwc2_ctl_chh(u2ps_dwc2_ctl_t *c, uint32_t hcint, uint32_t hctsiz,
                       u2ps_dwc2_ctl_step_t *s)
{
  *s = (u2ps_dwc2_ctl_step_t){.what = U2PS_DWC2_CTL_NONE};
  if (!c->busy) {
    c->faults++;
    return;
  }
  bool data = c->stage == U2PS_DWC2_STAGE_DATA_IN || c->stage == U2PS_DWC2_STAGE_DATA_OUT;
  uint32_t raw = 0;
  uint16_t moved = data ? part_moved(c, hctsiz, &raw) : 0u;
  if ((hcint & U2PS_DWC2_HCINT_XFRC) != 0u) {
    c->errors = 0;
    if (c->stage == U2PS_DWC2_STAGE_STATUS ||
        (c->stage == U2PS_DWC2_STAGE_SETUP && c->setup_only)) {
      /* The transfer has ended, an abort or not */
      c->held = c->stage == U2PS_DWC2_STAGE_SETUP;
      ctl_done(c, U2PS_DWC2_CTL_OK, s);
      return;
    }
    if (c->aborting) {
      ctl_done(c, U2PS_DWC2_CTL_ABORTED, s);
      return;
    }
    if (c->stage == U2PS_DWC2_STAGE_SETUP) {
      arm_stage(c, after_setup(c), s);
      return;
    }
    /* A data part: the next one, unless the stage is complete or a short
     * packet ended it */
    uint32_t packets = ((uint32_t)c->armed + c->mps0 - 1u) / c->mps0;
    bool short_packet = c->stage == U2PS_DWC2_STAGE_DATA_IN && raw < packets * c->mps0;
    c->offset = (uint16_t)(c->offset + moved);
    arm_stage(c, short_packet || c->offset >= c->length ? U2PS_DWC2_STAGE_STATUS : c->stage, s);
    return;
  }
  if ((hcint & U2PS_DWC2_HCINT_STALL) != 0u) {
    c->offset = (uint16_t)(c->offset + moved);
    ctl_done(c, U2PS_DWC2_CTL_STALL, s);
    return;
  }
  c->offset = (uint16_t)(c->offset + moved);
  if (c->aborting) {
    ctl_done(c, U2PS_DWC2_CTL_ABORTED, s);
    return;
  }
  if ((hcint & U2PS_DWC2_HCINT_AHBERR) != 0u) {
    c->faults++;
    ctl_done(c, U2PS_DWC2_CTL_ERROR, s);
    return;
  }
  if ((hcint & HCINT_XACT_ERR) == 0u && (hcint & U2PS_DWC2_HCINT_NAK) != 0u) {
    /* NAK alone, no transmission error (USB 2.0 10.2.6): the count
     * restarts, what is left goes again */
    c->errors = 0;
    c->naks++;
    arm_stage(c, c->stage, s);
    return;
  }
  /* Data moved restarts the count */
  if (moved > 0u)
    c->errors = 0;
  if ((hcint & HCINT_XACT_ERR) == 0u && (hcint & U2PS_DWC2_HCINT_DTERR) != 0u) {
    /* A data toggle error restarts it too (RM0456 p. 3411) */
    c->errors = 0;
  } else if (++c->errors >= U2PS_DWC2_CTL_STRIKES) {
    /* A transaction error, or a halt without a cause: the third in a row
     * retires the transfer (USB 2.0 10.2.6) */
    ctl_done(c, U2PS_DWC2_CTL_ERROR, s);
    return;
  }
  c->retries++;
  arm_stage(c, c->stage, s);
}
