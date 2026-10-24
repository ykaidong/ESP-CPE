/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal host test harness. No Unity, no ESP-IDF, no build system integration:
 * just enough to run the pure logic of this project in an ordinary process and
 * report a failure with file and line. It is intentionally dependency free so
 * tests/host/run.ps1 can compile everything with a plain gcc.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void ht_check(bool cond, const char *expr, const char *file, int line);
void ht_check_str(const char *got, const char *want, const char *file, int line);
void ht_check_mem(const void *got, const void *want, size_t len, const char *file, int line);
void ht_check_int(long long got, long long want, const char *expr, const char *file, int line);

/* Runs one suite and prints its name; suites report their own assertions. */
void ht_run_suite(const char *name, void (*suite)(void));

/* Prints the summary and returns 0 when nothing failed. */
int ht_report(void);

#define HT_CHECK(cond)               ht_check((cond), #cond, __FILE__, __LINE__)
#define HT_CHECK_STR(got, want)      ht_check_str((got), (want), __FILE__, __LINE__)
#define HT_CHECK_MEM(got, want, len) ht_check_mem((got), (want), (len), __FILE__, __LINE__)
#define HT_CHECK_INT(got, want)      ht_check_int((long long)(got), (long long)(want), #got, __FILE__, __LINE__)
