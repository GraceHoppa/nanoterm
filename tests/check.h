// Minimal test harness: CHECK records a failure and carries on, so one run
// reports every broken case; TEST_MAIN_END prints a summary and sets the
// exit code for `make test`.
#ifndef NANOTERM_CHECK_H
#define NANOTERM_CHECK_H

#include <stdio.h>
#include <string.h>

static int checks_run, checks_failed;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks_run++;                                                      \
        if (!(cond)) {                                                     \
            checks_failed++;                                               \
            fprintf(stderr, "  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

// True if haystack contains needle (for checking error messages).
static inline int contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

#define TEST_MAIN_END(name)                                                     \
    do {                                                                        \
        printf("%-16s %d checks, %d failed\n", name, checks_run, checks_failed); \
        return checks_failed ? 1 : 0;                                           \
    } while (0)

#endif
