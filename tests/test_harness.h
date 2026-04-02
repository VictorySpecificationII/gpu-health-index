#ifndef GPU_HEALTH_TEST_HARNESS_H
#define GPU_HEALTH_TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Minimal test harness — no external dependencies.
 *
 * Usage:
 *   int main(void) {
 *       test_foo();
 *       test_bar();
 *       return TEST_RESULT();
 *   }
 * ------------------------------------------------------------------------- */

static int g_failures = 0;
static int g_checks   = 0;

#define ASSERT(cond)                                                         \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define ASSERT_EQ_INT(a, b)                                                  \
    do {                                                                     \
        g_checks++;                                                          \
        int _a = (int)(a), _b = (int)(b);                                   \
        if (_a != _b) {                                                      \
            fprintf(stderr, "  FAIL %s:%d: %s == %s  (%d != %d)\n",        \
                    __FILE__, __LINE__, #a, #b, _a, _b);                    \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define ASSERT_EQ_DBL(a, b)                                                  \
    do {                                                                     \
        g_checks++;                                                          \
        double _a = (double)(a), _b = (double)(b);                          \
        if (_a != _b) {                                                      \
            fprintf(stderr, "  FAIL %s:%d: %s == %s  (%g != %g)\n",        \
                    __FILE__, __LINE__, #a, #b, _a, _b);                    \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define ASSERT_STR(a, b)                                                     \
    do {                                                                     \
        g_checks++;                                                          \
        if (strcmp((a), (b)) != 0) {                                        \
            fprintf(stderr, "  FAIL %s:%d: %s == %s  ('%s' != '%s')\n",   \
                    __FILE__, __LINE__, #a, #b, (a), (b));                  \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* Run a named test function and print its result */
#define RUN_TEST(fn)                                                         \
    do {                                                                     \
        int _before = g_failures;                                            \
        fn();                                                                \
        if (g_failures == _before)                                           \
            fprintf(stderr, "  ok  " #fn "\n");                             \
    } while (0)

/* Print summary and return appropriate exit code */
static inline int test_result(void)
{
    fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures > 0 ? 1 : 0;
}
#define TEST_RESULT() test_result()

#endif /* GPU_HEALTH_TEST_HARNESS_H */
