/* Minimal, dependency-free test helpers shared by the test_*.c files. */
#ifndef PEXPR_TEST_H
#define PEXPR_TEST_H

#include <stdio.h>
#include <string.h>

extern int g_test_count;
extern int g_test_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_test_count++;                                                    \
        if (!(cond)) {                                                     \
            g_test_failures++;                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

#define CHECK_EQ_LL(a, b)                                                            \
    do {                                                                             \
        long long _ca = (long long)(a), _cb = (long long)(b);                       \
        g_test_count++;                                                              \
        if (_ca != _cb) {                                                            \
            g_test_failures++;                                                       \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__,       \
                    __LINE__, #a, #b, _ca, _cb);                                     \
        }                                                                            \
    } while (0)

#define CHECK_EQ_DBL(a, b)                                                     \
    do {                                                                       \
        double _da = (a), _db = (b);                                          \
        g_test_count++;                                                        \
        if (_da != _db) {                                                      \
            g_test_failures++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%.17g != %.17g)\n", __FILE__, \
                    __LINE__, #a, #b, _da, _db);                               \
        }                                                                      \
    } while (0)

#define CHECK_STREQ(a, b)                                                       \
    do {                                                                        \
        const char *_sa = (a), *_sb = (b);                                     \
        g_test_count++;                                                         \
        if (strcmp(_sa, _sb) != 0) {                                            \
            g_test_failures++;                                                  \
            fprintf(stderr, "FAIL %s:%d: %s == %s (\"%s\" != \"%s\")\n", __FILE__, \
                    __LINE__, #a, #b, _sa, _sb);                                \
        }                                                                       \
    } while (0)

#endif
