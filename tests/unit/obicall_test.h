#ifndef OBICALL_TEST_H
#define OBICALL_TEST_H

/* Minimal header-only test harness: each test binary is its own
 * standalone CTest case (see tests/CMakeLists.txt), so this only needs
 * assertion macros and a pass/fail summary - not a registration/runner
 * framework. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_obicall_test_failures = 0;
static int g_obicall_test_count = 0;

#define OBICALL_CHECK(cond)                                                                      \
    do {                                                                                          \
        g_obicall_test_count++;                                                                   \
        if (!(cond)) {                                                                             \
            g_obicall_test_failures++;                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
        }                                                                                           \
    } while (0)

#define OBICALL_CHECK_EQ_INT(a, b)                                                                \
    do {                                                                                           \
        long long _a = (long long)(a), _b = (long long)(b);                                        \
        g_obicall_test_count++;                                                                    \
        if (_a != _b) {                                                                             \
            g_obicall_test_failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, #a, _a, #b, _b); \
        }                                                                                            \
    } while (0)

#define OBICALL_CHECK_NEAR(a, b, tol)                                                             \
    do {                                                                                           \
        double _a = (double)(a), _b = (double)(b), _t = (double)(tol);                              \
        g_obicall_test_count++;                                                                    \
        if (fabs(_a - _b) > _t) {                                                                    \
            g_obicall_test_failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s (%.9g) not near %s (%.9g) tol=%.9g\n", __FILE__, __LINE__, \
                    #a, _a, #b, _b, _t);                                                             \
        }                                                                                            \
    } while (0)

#define OBICALL_TEST_MAIN_BEGIN() int main(void) {
#define OBICALL_TEST_MAIN_END()                                                                   \
    fprintf(stderr, "%s: %d/%d checks passed\n", __FILE__, g_obicall_test_count - g_obicall_test_failures, \
            g_obicall_test_count);                                                                  \
    return g_obicall_test_failures == 0 ? 0 : 1;                                                    \
    }

#endif /* OBICALL_TEST_H */
