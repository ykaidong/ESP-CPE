/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UTF-8 -> UCS2 hex conversion for text-mode sending (AT+CSCS="UCS2"); the
 * receive path decodes UCS2 inside cpe_sms_pdu.c.
 */

#include <stdint.h>
#include <stdio.h>

#include "cpe_sms_ucs2.h"

size_t cpe_sms_ucs2_encode(const char *src, char *dst, size_t dst_cap)
{
    size_t out = 0;
    while (*src != '\0') {
        uint32_t cp = 0;
        unsigned char c = (unsigned char)*src;
        int len;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            return 0;
        }
        for (int i = 1; i < len; i++) {
            unsigned char cc = (unsigned char)src[i];
            if ((cc & 0xC0) != 0x80) {
                return 0;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        src += len;

        int n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unsigned hi = 0xD800 + (cp >> 10);
            unsigned lo = 0xDC00 + (cp & 0x3FF);
            if (out + 8 + 1 > dst_cap) return 0;
            n = snprintf(dst + out, dst_cap - out, "%04X%04X", hi, lo);
        } else {
            if (out + 4 + 1 > dst_cap) return 0;
            n = snprintf(dst + out, dst_cap - out, "%04X", (unsigned)cp);
        }
        if (n < 0 || (size_t)n >= dst_cap - out) return 0;
        out += (size_t)n;
    }
    dst[out] = '\0';
    return out;
}
