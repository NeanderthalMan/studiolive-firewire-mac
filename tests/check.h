// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The whole test framework: CHECK records a failure and carries on, so one run
 * reports every broken expectation rather than the first; TEST names the block
 * a failure belongs to; the exit status is the number of failures, capped, so
 * `make test` gates on it.
 *
 * Tests #include the .c file they cover instead of linking it. That is what
 * lets them reach the static depacketiser, clock fit and I/O functions, which
 * is where the bugs have been, without a console on the bus.
 */
#ifndef CHECK_H
#define CHECK_H

#include <math.h>
#include <stdio.h>

static int check_failures, check_count;
static const char *check_test = "?";

#define TEST(name) (check_test = (name))

#define CHECK(cond, ...) do {                                             \
        check_count++;                                                    \
        if (!(cond)) {                                                    \
            check_failures++;                                             \
            fprintf(stderr, "FAIL %s (%s:%d): %s — ", check_test,         \
                    __FILE__, __LINE__, #cond);                           \
            fprintf(stderr, __VA_ARGS__);                                 \
            fputc('\n', stderr);                                          \
        }                                                                 \
    } while (0)

#define CHECK_NEAR(a, b, tol, what) \
    CHECK(fabs((double)(a) - (double)(b)) <= (tol), "%s: %.9g, want %.9g ± %g", what, \
          (double)(a), (double)(b), (double)(tol))

static int check_done(const char *suite)
{
    printf("%-10s %d checks, %d failed\n", suite, check_count, check_failures);
    return check_failures > 100 ? 100 : check_failures;
}

#endif
