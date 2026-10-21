/* Minimal test helpers. */
#ifndef RAG_TEST_H
#define RAG_TEST_H

#include <stdio.h>
#include <string.h>

static int test_checks, test_failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        test_checks++;                                                               \
        if (!(cond)) {                                                               \
            test_failures++;                                                         \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define CHECK_STR(actual, expected)                                                            \
    do {                                                                                       \
        const char *a_ = (actual), *e_ = (expected);                                           \
        test_checks++;                                                                         \
        if (!a_ || !e_ || strcmp(a_, e_) != 0) {                                               \
            test_failures++;                                                                   \
            fprintf(stderr, "%s:%d: %s\n  got:      %s\n  expected: %s\n", __FILE__, __LINE__, \
                    #actual, a_ ? a_ : "(null)", e_ ? e_ : "(null)");                          \
        }                                                                                      \
    } while (0)

#define CHECK_SIZE(actual, expected)                                                             \
    do {                                                                                         \
        size_t a_ = (size_t)(actual), e_ = (size_t)(expected);                                   \
        test_checks++;                                                                           \
        if (a_ != e_) {                                                                          \
            test_failures++;                                                                     \
            fprintf(stderr, "%s:%d: %s\n  got:      %zu\n  expected: %zu\n", __FILE__, __LINE__, \
                    #actual, a_, e_);                                                            \
        }                                                                                        \
    } while (0)

#define RUN(test)                \
    do {                         \
        printf("- %s\n", #test); \
        fflush(stdout);          \
        test();                  \
    } while (0)

static int test_report(void) {
    printf("%d checks, %d failures\n", test_checks, test_failures);
    return test_failures ? 1 : 0;
}

#endif
