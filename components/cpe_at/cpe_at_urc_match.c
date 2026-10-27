/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "cpe_at_urc_match.h"

static bool span_eq(const char *a, const char *b, size_t len)
{
    return memcmp(a, b, len) == 0;
}

cpe_at_line_kind_t cpe_at_urc_line_kind(const char *line, size_t len)
{
    if (line == NULL) {
        return CPE_AT_LINE_OTHER;
    }
    if (len == 2 && span_eq(line, "OK", 2)) {
        return CPE_AT_LINE_FINAL_OK;
    }
    if (len == 5 && span_eq(line, "ERROR", 5)) {
        return CPE_AT_LINE_FINAL_ERROR;
    }
    if (len >= 11 && (span_eq(line, "+CME ERROR:", 11) ||
                      span_eq(line, "+CMS ERROR:", 11))) {
        return CPE_AT_LINE_FINAL_ERROR;
    }
    return CPE_AT_LINE_OTHER;
}

bool cpe_at_urc_line_matches(const char *line, size_t len,
                             const char *const *prefixes, size_t count)
{
    if (line == NULL || prefixes == NULL ||
        cpe_at_urc_line_kind(line, len) != CPE_AT_LINE_OTHER) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        const char *p = prefixes[i];
        if (p == NULL || p[0] == '\0') {
            continue;
        }
        size_t plen = strlen(p);
        if (len >= plen && span_eq(line, p, plen)) {
            return true;
        }
    }
    return false;
}

bool cpe_at_urc_partial_matches(const char *partial, size_t len,
                                const char *const *prefixes, size_t count)
{
    if (partial == NULL || prefixes == NULL || len == 0) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        const char *p = prefixes[i];
        if (p == NULL || p[0] == '\0') {
            continue;
        }
        size_t plen = strlen(p);
        if (len >= plen) {
            if (span_eq(partial, p, plen)) {
                return true;
            }
        } else if (span_eq(p, partial, len)) {
            return true;
        }
    }
    return false;
}
