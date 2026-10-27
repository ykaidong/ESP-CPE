/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CPE_AT_LINE_OTHER = 0,
    CPE_AT_LINE_FINAL_OK,
    CPE_AT_LINE_FINAL_ERROR,
} cpe_at_line_kind_t;

/* Classify one complete line with its CR/LF terminator already removed. */
cpe_at_line_kind_t cpe_at_urc_line_kind(const char *line, size_t len);

/* True when a complete line starts with one of `prefixes` (for example
 * "+CMTI:"). Result lines never match, so a prefix such as "+CME" cannot
 * swallow the "+CME ERROR:" final line. */
bool cpe_at_urc_line_matches(const char *line, size_t len,
                             const char *const *prefixes, size_t count);

/* True when a line that has not been terminated yet could still become a URC:
 * it already starts with a prefix, or is itself a prefix of one. */
bool cpe_at_urc_partial_matches(const char *partial, size_t len,
                                const char *const *prefixes, size_t count);

#ifdef __cplusplus
}
#endif
