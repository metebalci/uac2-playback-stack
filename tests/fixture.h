/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * fixture.h: the descriptor fixtures of tests/fixtures, which the device
 * and the host tests share.
 */
#ifndef FIXTURE_H
#define FIXTURE_H

#include <stdint.h>

/* Read a fixture of tests/fixtures (hex bytes, '#' comments) into buf.
 * Returns the number of bytes, or -1. */
int fixture_load(const char *name, uint8_t *buf, unsigned max);

#endif /* FIXTURE_H */
