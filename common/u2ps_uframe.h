/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * u2ps_uframe.h: microframe numbers and their arithmetic.
 *
 * A high-speed host keeps a microframe number of at least 14 bits that
 * increments every 125 us, and sends its bits 3 to 13 as the 11-bit frame
 * number of each SOF, so one frame number appears in eight consecutive
 * microframes (USB 2.0 10.2.3, p. 280). The frame number rolls over at 7FFh
 * (USB 2.0 8.3.3); a device may count the eight microframes of a frame
 * 0 to 7 (USB 2.0 8.4.3.1). u2ps_uframe_t holds such a number,
 * (frame << 3 | microframe) mod 2^14, and wraps every 2.048 s.
 *
 * All arithmetic is modulo 2^14. Arguments of type u2ps_uframe_t must be
 * at most U2PS_UFRAME_MASK. The functions are static inline: the packet
 * paths of both stacks use them (USB 2.0 10.2.3).
 */
#ifndef U2PS_UFRAME_H
#define U2PS_UFRAME_H

#include <stdint.h>

typedef uint16_t u2ps_uframe_t;

#define U2PS_UFRAME_BITS  14u
#define U2PS_UFRAME_COUNT (1u << U2PS_UFRAME_BITS) /* 16384 microframes */
#define U2PS_UFRAME_MASK  (U2PS_UFRAME_COUNT - 1u) /* 0x3FFF */
#define U2PS_FRAME_MASK   0x7FFu                   /* 11-bit frame number */
#define U2PS_MICRO_MASK   0x7u                     /* microframe 0..7 */

/* u + n microframes */
static inline u2ps_uframe_t u2ps_uframe_add(u2ps_uframe_t u, uint16_t n)
{
  return (u2ps_uframe_t)(((unsigned)u + n) & U2PS_UFRAME_MASK);
}

/* u - n microframes. With n a microframe number, this is the forward
 * distance from n to u, 0..0x3FFF. */
static inline u2ps_uframe_t u2ps_uframe_sub(u2ps_uframe_t u, uint16_t n)
{
  return (u2ps_uframe_t)(((unsigned)u - n) & U2PS_UFRAME_MASK);
}

/* Signed distance from 'from' to 'to', -8192..8191: positive when 'to' lies
 * after 'from' by less than half the wrap. */
static inline int16_t u2ps_uframe_dist(u2ps_uframe_t from, u2ps_uframe_t to)
{
  unsigned d = ((unsigned)to - from) & U2PS_UFRAME_MASK;

  /* The upper half of the forward distance is a step backwards */
  if (d >= U2PS_UFRAME_COUNT / 2u)
    return (int16_t)((int)d - (int)U2PS_UFRAME_COUNT);
  return (int16_t)d;
}

/* The 11-bit frame number of u, as an SOF carries it */
static inline uint16_t u2ps_uframe_frame(u2ps_uframe_t u)
{
  return (uint16_t)((u >> 3) & U2PS_FRAME_MASK);
}

/* The microframe 0..7 of u within its frame */
static inline uint8_t u2ps_uframe_micro(u2ps_uframe_t u)
{
  return (uint8_t)(u & U2PS_MICRO_MASK);
}

/* The microframe number of microframe 'micro' in frame 'frame'; both are
 * taken modulo their range. */
static inline u2ps_uframe_t u2ps_uframe_make(uint16_t frame, uint8_t micro)
{
  return (u2ps_uframe_t)(((frame & U2PS_FRAME_MASK) << 3) | (micro & U2PS_MICRO_MASK));
}

#endif /* U2PS_UFRAME_H */
