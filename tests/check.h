/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * check.h: the minimal check macros of the unit tests. Each test is one
 * executable with one source file; main() returns check_done(), which
 * fails the test if any check failed. A failed check prints its file,
 * line, expression and, for CHECK_EQ, both values, and the test goes on.
 */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>
#include <stdlib.h>

static unsigned long check_count;
static unsigned long check_failures;

static inline void check_true(int ok, const char *expr, const char *file, int line)
{
  check_count++;
  if (!ok) {
    check_failures++;
    printf("%s:%d: CHECK(%s) failed\n", file, line, expr);
  }
}

static inline void check_eq(long long actual, long long expected, const char *actual_expr,
                            const char *expected_expr, const char *file, int line)
{
  check_count++;
  if (actual != expected) {
    check_failures++;
    printf("%s:%d: CHECK_EQ(%s, %s) failed: %lld (0x%llx) != %lld (0x%llx)\n", file, line,
           actual_expr, expected_expr, actual, (unsigned long long)actual, expected,
           (unsigned long long)expected);
  }
}

#define CHECK(cond) check_true((cond) ? 1 : 0, #cond, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) \
  check_eq((long long)(actual), (long long)(expected), #actual, #expected, __FILE__, __LINE__)

static inline int check_done(void)
{
  printf("%lu checks, %lu failed\n", check_count, check_failures);
  return check_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif /* CHECK_H */
