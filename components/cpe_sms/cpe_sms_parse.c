/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure parsing/validation for SMS text mode, host-tested.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpe_sms_parse.h"
#include "cpe_sms_ucs2.h"

bool cpe_sms_number_valid(const char *num)
{
    if (num == NULL) {
        return false;
    }
    if (*num == '+') {
        num++;
    }
    size_t n = 0;
    for (; *num != '\0'; num++) {
        if (*num < '0' || *num > '9') {
            return false;
        }
        n++;
    }
    return n >= 3 && n <= 20;
}

static size_t utf8_codepoints(const char *s)
{
    size_t n = 0;
    while (*s != '\0') {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) {
            n++;
            s += 1;
        } else if ((c & 0xE0) == 0xC0) {
            n++;
            s += 2;
        } else if ((c & 0xF0) == 0xE0) {
            n++;
            s += 3;
        } else if ((c & 0xF8) == 0xF0) {
            n += 2; /* supplementary plane -> 2 UCS2 units */
            s += 4;
        } else {
            return 0;
        }
    }
    return n;
}

bool cpe_sms_text_measure(const char *text, bool *is_ascii, int *units)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    bool ascii = true;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (is_ascii != NULL) {
        *is_ascii = ascii;
    }
    if (ascii) {
        size_t len = strlen(text);
        if (units != NULL) {
            *units = (int)len;
        }
        return len <= 160;
    }
    size_t cp = utf8_codepoints(text);
    if (units != NULL) {
        *units = (int)cp;
    }
    return cp > 0 && cp <= 70;
}

bool cpe_sms_stat_unread(const char *stat)
{
    if (stat == NULL) {
        return false;
    }
    const char *p = stat;
    while (*p != '\0') {
        if (tolower((unsigned char)p[0]) == 'u' &&
            tolower((unsigned char)p[1]) == 'n' &&
            tolower((unsigned char)p[2]) == 'r' &&
            tolower((unsigned char)p[3]) == 'e' &&
            tolower((unsigned char)p[4]) == 'a' &&
            tolower((unsigned char)p[5]) == 'd') {
            return true;
        }
        p++;
    }
    return false;
}

/* Copy the next comma-separated field of *pp (advancing *pp past the field and
 * its trailing comma) into dst, quotes removed. Returns the field length. */
static size_t next_field(const char **pp, char *dst, size_t cap)
{
    const char *p = *pp;
    while (*p == ' ') {
        p++;
    }
    bool quoted = (*p == '"');
    if (quoted) {
        p++;
    }
    size_t n = 0;
    while (*p != '\0' && (quoted ? *p != '"' : *p != ',')) {
        if (n + 1 < cap) {
            dst[n++] = *p;
        }
        p++;
    }
    if (quoted && *p == '"') {
        p++;
    }
    if (*p == ',') {
        p++;
    }
    *pp = p;
    dst[n] = '\0';
    return n;
}

static void parse_cmgl_line(const char *line, const char *body, cpe_sms_msg_t *m)
{
    const char *p = line + 6; /* skip "+CMGL:" */
    char small[CPE_SMS_MAX_ADDR];
    char num_hex[CPE_SMS_MAX_ADDR_HEX];

    next_field(&p, small, sizeof(small));
    m->index = (int)strtol(small, NULL, 10);

    next_field(&p, small, sizeof(small));
    snprintf(m->stat, sizeof(m->stat), "%s", small);

    next_field(&p, num_hex, sizeof(num_hex));
    cpe_sms_ucs2_decode(num_hex, m->from, sizeof(m->from));

    next_field(&p, small, sizeof(small)); /* alpha, unused */

    next_field(&p, small, sizeof(small));
    snprintf(m->time, sizeof(m->time), "%s", small);

    if (body != NULL) {
        cpe_sms_ucs2_decode(body, m->body, sizeof(m->body));
    }
}

bool cpe_sms_parse_cmgl(const char *raw, cpe_sms_list_t *out)
{
    if (raw == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *p = raw;
    while (p != NULL && *p != '\0') {
        const char *eol = strstr(p, "\r\n");
        size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);

        char line[512];
        size_t cl = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, cl);
        line[cl] = '\0';

        if (strncmp(line, "+CMGL:", 6) == 0) {
            if (out->count >= CPE_SMS_MAX_MESSAGES) {
                out->truncated = true;
                return true;
            }
            cpe_sms_msg_t *m = &out->items[out->count];
            memset(m, 0, sizeof(*m));

            char body[CPE_SMS_MAX_BODY_HEX];
            body[0] = '\0';
            if (eol != NULL) {
                const char *bstart = eol + 2;
                const char *bend = strstr(bstart, "\r\n");
                if (bend != NULL) {
                    size_t blen = (size_t)(bend - bstart);
                    size_t cap = blen < sizeof(body) - 1 ? blen : sizeof(body) - 1;
                    memcpy(body, bstart, cap);
                    body[cap] = '\0';
                }
            }
            parse_cmgl_line(line, body, m);
            out->count++;
        }
        p = eol != NULL ? eol + 2 : NULL;
    }
    return true;
}
