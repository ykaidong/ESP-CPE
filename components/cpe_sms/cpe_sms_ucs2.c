/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UCS2 <-> UTF-8 conversion for SMS text mode (AT+CSCS="UCS2").
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpe_sms_ucs2.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

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

size_t cpe_sms_ucs2_decode(const char *src, char *dst, size_t dst_cap)
{
    if (src == NULL || dst == NULL || dst_cap == 0) {
        return 0;
    }
    size_t len = strlen(src);
    if (len < 4 || len % 4 != 0) {
        size_t n = len < dst_cap - 1 ? len : dst_cap - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
        return n;
    }
    for (size_t i = 0; i < len; i++) {
        if (hexval(src[i]) < 0) {
            size_t n = len < dst_cap - 1 ? len : dst_cap - 1;
            memcpy(dst, src, n);
            dst[n] = '\0';
            return n;
        }
    }

    size_t used = 0;
    for (size_t i = 0; i < len; i += 4) {
        uint32_t cp = (uint32_t)((hexval(src[i]) << 12) |
                                 (hexval(src[i + 1]) << 8) |
                                 (hexval(src[i + 2]) << 4) |
                                  hexval(src[i + 3]));
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 7 < len) {
            uint32_t low = (uint32_t)((hexval(src[i + 4]) << 12) |
                                      (hexval(src[i + 5]) << 8) |
                                      (hexval(src[i + 6]) << 4) |
                                       hexval(src[i + 7]));
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 4;
            }
        }
        if ((cp < 0x20 && cp != '\t' && cp != '\r' && cp != '\n') ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            size_t n = len < dst_cap - 1 ? len : dst_cap - 1;
            memcpy(dst, src, n);
            dst[n] = '\0';
            return n;
        }

        uint8_t utf8[4];
        size_t n;
        if (cp <= 0x7F) {
            utf8[0] = (uint8_t)cp;
            n = 1;
        } else if (cp <= 0x7FF) {
            utf8[0] = (uint8_t)(0xC0 | (cp >> 6));
            utf8[1] = (uint8_t)(0x80 | (cp & 0x3F));
            n = 2;
        } else if (cp <= 0xFFFF) {
            utf8[0] = (uint8_t)(0xE0 | (cp >> 12));
            utf8[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            utf8[2] = (uint8_t)(0x80 | (cp & 0x3F));
            n = 3;
        } else {
            utf8[0] = (uint8_t)(0xF0 | (cp >> 18));
            utf8[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
            utf8[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            utf8[3] = (uint8_t)(0x80 | (cp & 0x3F));
            n = 4;
        }
        if (used + n >= dst_cap) return 0;
        memcpy(dst + used, utf8, n);
        used += n;
    }
    dst[used] = '\0';
    return used;
}
