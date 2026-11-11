/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * fixture.c: see fixture.h.
 */
#include "fixture.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

int fixture_load(const char *name, uint8_t *buf, unsigned max)
{
  char path[512];
  char line[256];
  unsigned n = 0;

  snprintf(path, sizeof path, "%s/%s", U2PS_FIXTURES, name);
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    printf("fixture %s: cannot open\n", path);
    return -1;
  }
  while (fgets(line, sizeof line, f) != NULL) {
    if (line[0] == '#')
      continue;
    for (char *p = line; *p != '\0';) {
      if (isspace((unsigned char)*p)) {
        p++;
        continue;
      }
      char *end;
      unsigned long v = strtoul(p, &end, 16);
      if (end == p || v > 0xFFu || n >= max) {
        printf("fixture %s: bad byte or too long\n", path);
        fclose(f);
        return -1;
      }
      buf[n++] = (uint8_t)v;
      p = end;
    }
  }
  fclose(f);
  return (int)n;
}
