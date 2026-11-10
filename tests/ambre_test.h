/*
 * ambre_test.h — Minimal test harness (no external dependency, PC + QNX).
 *
 * Usage in tests/test_foo.c:
 *
 *   #include "ambre_test.h"
 *   static void test_a(void) { AMBRE_CHECK(1 == 1); }
 *   static void test_b(void) { AMBRE_CHECK_EQ_U64(2, 1 + 1); }
 *   static const ambre_test_case_t cases[] = {
 *       { "a", test_a }, { "b", test_b }
 *   };
 *   AMBRE_TEST_MAIN(cases)
 *
 * Exit code = number of failed checks (0 = green).
 */

#ifndef AMBRE_TEST_H
#define AMBRE_TEST_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

typedef struct {
    const char *name;
    void (*fn)(void);
} ambre_test_case_t;

extern int ambre_test_checks;
extern int ambre_test_failures;
extern const char *ambre_test_current;

void ambre_test_fail(const char *file, int line, const char *fmt, ...);

#define AMBRE_CHECK(cond)                                                    \
    do {                                                                     \
        ambre_test_checks++;                                                 \
        if (!(cond)) ambre_test_fail(__FILE__, __LINE__, "CHECK(%s)", #cond);\
    } while (0)

#define AMBRE_CHECK_MSG(cond, ...)                                           \
    do {                                                                     \
        ambre_test_checks++;                                                 \
        if (!(cond)) ambre_test_fail(__FILE__, __LINE__, __VA_ARGS__);       \
    } while (0)

#define AMBRE_CHECK_EQ_U64(actual, expected)                                 \
    do {                                                                     \
        uint64_t a_ = (uint64_t)(actual), e_ = (uint64_t)(expected);         \
        ambre_test_checks++;                                                 \
        if (a_ != e_) ambre_test_fail(__FILE__, __LINE__,                    \
            "%s = %" PRIu64 ", expected %" PRIu64, #actual, a_, e_);         \
    } while (0)

#define AMBRE_CHECK_EQ_I64(actual, expected)                                 \
    do {                                                                     \
        int64_t a_ = (int64_t)(actual), e_ = (int64_t)(expected);            \
        ambre_test_checks++;                                                 \
        if (a_ != e_) ambre_test_fail(__FILE__, __LINE__,                    \
            "%s = %" PRId64 ", expected %" PRId64, #actual, a_, e_);         \
    } while (0)

#define AMBRE_CHECK_OK(call)                                                 \
    do {                                                                     \
        int rc_ = (call);                                                    \
        ambre_test_checks++;                                                 \
        if (rc_ != 0) ambre_test_fail(__FILE__, __LINE__,                    \
            "%s returned %d, expected 0", #call, rc_);                       \
    } while (0)

#define AMBRE_CHECK_MEM_EQ(a, b, len)                                        \
    do {                                                                     \
        ambre_test_checks++;                                                 \
        if (memcmp((a), (b), (len)) != 0)                                    \
            ambre_test_fail(__FILE__, __LINE__,                              \
                "memory differs over %u bytes", (unsigned)(len));            \
    } while (0)

#define AMBRE_TEST_MAIN(cases)                                               \
    int ambre_test_checks = 0;                                               \
    int ambre_test_failures = 0;                                             \
    const char *ambre_test_current = "";                                     \
    int main(void) {                                                         \
        const ambre_test_case_t *c_ = (cases);                               \
        size_t n_ = sizeof(cases) / sizeof((cases)[0]);                      \
        for (size_t i_ = 0; i_ < n_; i_++) {                                 \
            int before_ = ambre_test_failures;                               \
            ambre_test_current = c_[i_].name;                                \
            printf("  %-42s", c_[i_].name);                                  \
            fflush(stdout);                                                  \
            c_[i_].fn();                                                     \
            printf("%s\n", ambre_test_failures > before_ ? "FAIL" : "ok");   \
        }                                                                    \
        printf("%s: %d checks, %d failures\n",                               \
               ambre_test_failures ? "FAILED" : "PASSED",                    \
               ambre_test_checks, ambre_test_failures);                      \
        return ambre_test_failures;                                          \
    }

#endif /* AMBRE_TEST_H */
