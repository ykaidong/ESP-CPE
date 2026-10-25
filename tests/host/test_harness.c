/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_harness.h"

#include <stdio.h>
#include <string.h>

static int s_checks;
static int s_failed;

void ht_check(bool cond, const char *expr, const char *file, int line)
{
    s_checks++;
    if (!cond) {
        printf("    FAIL %s:%d: %s\n", file, line, expr);
        s_failed++;
    }
}

void ht_check_str(const char *got, const char *want, const char *file, int line)
{
    s_checks++;
    bool equal = (got == NULL || want == NULL) ? (got == want) : (strcmp(got, want) == 0);
    if (!equal) {
        printf("    FAIL %s:%d: got \"%s\", expected \"%s\"\n", file, line,
               got != NULL ? got : "(null)", want != NULL ? want : "(null)");
        s_failed++;
    }
}

static void print_bytes(const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        printf("%02x ", data[i]);
    }
}

void ht_check_mem(const void *got, const void *want, size_t len, const char *file, int line)
{
    s_checks++;
    if (got == NULL || want == NULL || memcmp(got, want, len) != 0) {
        printf("    FAIL %s:%d: %zu bytes differ\n           got     ", file, line, len);
        if (got != NULL) {
            print_bytes((const unsigned char *)got, len);
        }
        printf("\n           expected ");
        if (want != NULL) {
            print_bytes((const unsigned char *)want, len);
        }
        printf("\n");
        s_failed++;
    }
}

void ht_check_int(long long got, long long want, const char *expr, const char *file, int line)
{
    s_checks++;
    if (got != want) {
        printf("    FAIL %s:%d: %s is %lld, expected %lld\n", file, line, expr, got, want);
        s_failed++;
    }
}

void ht_run_suite(const char *name, void (*suite)(void))
{
    printf("  %s\n", name);
    suite();
}

int ht_report(void)
{
    printf("\n%d checks, %d failed\n", s_checks, s_failed);
    return s_failed == 0 ? 0 : 1;
}
