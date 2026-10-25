/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "cpe_ct.h"

bool cpe_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool cpe_ct_equal_str(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la != lb || la == 0) {
        return false;
    }
    return cpe_ct_equal((const uint8_t *)a, (const uint8_t *)b, la);
}
