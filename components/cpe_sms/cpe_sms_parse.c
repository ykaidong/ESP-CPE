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

/* Count UCS2 code units in UTF-8 text. Invalid input (including a truncated
 * sequence at the end of the buffer) returns 0 instead of walking past the
 * terminator. */
static size_t utf8_codepoints(const char *s)
{
    size_t n = 0;
    while (*s != '\0') {
        unsigned char c = (unsigned char)*s;
        size_t cont;
        if (c < 0x80) {
            n++;
            s += 1;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            cont = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            cont = 2;
        } else if (c >= 0xF0 && c <= 0xF4) {
            cont = 3;
        } else {
            return 0;
        }
        for (size_t k = 1; k <= cont; k++) {
            if (((unsigned char)s[k] & 0xC0) != 0x80) {
                return 0; /* truncated or not a continuation byte */
            }
        }
        n += cont == 3 ? 2 : 1; /* supplementary plane -> 2 UCS2 units */
        s += cont + 1;
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

static void parse_cmgl_header(const char *line, cpe_sms_msg_t *m)
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
}

/* A line that ends the list, not part of any message body. The response reader
 * may stop at the "\r" of the final line, so a trailing "\r" is allowed. */
static bool is_terminal_line(const char *line)
{
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\r') {
        len--;
    }
    return (len == 2 && strncmp(line, "OK", 2) == 0) ||
           (len == 5 && strncmp(line, "ERROR", 5) == 0) ||
           (len >= 11 && (strncmp(line, "+CME ERROR:", 11) == 0 ||
                          strncmp(line, "+CMS ERROR:", 11) == 0));
}

bool cpe_sms_parse_cmgl(const char *raw, cpe_sms_list_t *out)
{
    if (raw == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->storage_used = -1;
    out->storage_capacity = -1;

    const char *p = raw;
    cpe_sms_msg_t *cur = NULL;
    char hex[CPE_SMS_MAX_BODY_HEX];
    size_t hex_len = 0;

    while (p != NULL && *p != '\0') {
        const char *eol = strstr(p, "\r\n");
        size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);

        char line[CPE_SMS_MAX_BODY_HEX];
        size_t cl = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, cl);
        line[cl] = '\0';

        if (strncmp(line, "+CMGL:", 6) == 0) {
            if (cur != NULL) {
                cpe_sms_ucs2_decode(hex, cur->body, sizeof(cur->body));
            }
            if (out->count >= CPE_SMS_MAX_MESSAGES) {
                out->truncated = true;
                return true;
            }
            cur = &out->items[out->count];
            memset(cur, 0, sizeof(*cur));
            parse_cmgl_header(line, cur);
            cur->part_total = 1;
            cur->index_count = 1;
            cur->indexes[0] = cur->index;
            hex[0] = '\0';
            hex_len = 0;
            out->count++;
        } else if (cur != NULL && is_terminal_line(line)) {
            cpe_sms_ucs2_decode(hex, cur->body, sizeof(cur->body));
            cur = NULL;
        } else if (cur != NULL && line[0] != '\0') {
            /* Body line: accumulate the raw hex. A body with an embedded newline
             * is emitted across several lines, so keep appending until the next
             * header or terminal line. */
            size_t alen = strlen(line);
            if (hex_len + alen + 1 < sizeof(hex)) {
                memcpy(hex + hex_len, line, alen + 1);
                hex_len += alen;
            }
        }

        p = eol != NULL ? eol + 2 : NULL;
    }

    if (cur != NULL) {
        cpe_sms_ucs2_decode(hex, cur->body, sizeof(cur->body));
    }
    return true;
}

bool cpe_sms_parse_cpms(const char *resp, int *used, int *total)
{
    if (resp == NULL) {
        return false;
    }
    const char *p = strstr(resp, "+CPMS:");
    if (p == NULL) {
        return false;
    }
    p += 6;
    while (*p == ' ') {
        p++;
    }
    if (*p == '"') { /* "+CPMS: \"ME\",<used>,<total>,... */
        p++;
        while (*p != '\0' && *p != '"') {
            p++;
        }
        if (*p != '"') {
            return false;
        }
        p++;
        if (*p != ',') {
            return false;
        }
        p++;
    }
    char *end = NULL;
    long u = strtol(p, &end, 10);
    if (end == p || *end != ',') {
        return false;
    }
    p = end + 1;
    long t = strtol(p, &end, 10);
    if (end == p || u < 0 || t <= 0) {
        return false;
    }
    if (used != NULL) {
        *used = (int)u;
    }
    if (total != NULL) {
        *total = (int)t;
    }
    return true;
}

bool cpe_sms_parse_qcmgr_seg(const char *line, int *uid, int *seg, int *total)
{
    if (line == NULL) {
        return false;
    }
    const char *p = strstr(line, "+QCMGR:");
    if (p == NULL) {
        return false;
    }
    /* The tail is always ",<uid>,<seg>,<total>"; the SCTS field may itself
     * contain a comma, so walk back from the end of the header line instead of
     * splitting fields. Only the header is considered: a body line may contain
     * anything. */
    const char *end = p;
    while (*end != '\0' && *end != '\r' && *end != '\n') {
        end++;
    }
    const char *comma[3] = { NULL, NULL, NULL };
    int found = 0;
    for (const char *q = end - 1; q >= p && found < 3; q--) {
        if (*q == ',') {
            comma[found++] = q;
        }
    }
    if (found < 3) {
        return false;
    }
    char *e = NULL;
    long v_uid = strtol(comma[2] + 1, &e, 10);
    if (e == comma[2] + 1 || v_uid < 0) {
        return false;
    }
    long v_seg = strtol(comma[1] + 1, &e, 10);
    if (e == comma[1] + 1 || v_seg < 1) {
        return false;
    }
    long v_total = strtol(comma[0] + 1, &e, 10);
    if (e == comma[0] + 1 || v_total < 1 || v_seg > v_total) {
        return false;
    }
    while (e < end && *e == ' ') {
        e++;
    }
    if (e != end) { /* a single message's SCTS tail, not a segment triple */
        return false;
    }
    if (uid != NULL) {
        *uid = (int)v_uid;
    }
    if (seg != NULL) {
        *seg = (int)v_seg;
    }
    if (total != NULL) {
        *total = (int)v_total;
    }
    return true;
}

/* Append `src` to dst+*len, stopping at a UTF-8 boundary when it no longer
 * fits. Returns false (and leaves any partial code point out) when cut. */
static bool append_utf8(char *dst, size_t cap, size_t *len, const char *src)
{
    while (*src != '\0') {
        unsigned char c = (unsigned char)*src;
        size_t clen;
        if (c < 0x80) {
            clen = 1;
        } else if ((c & 0xE0) == 0xC0) {
            clen = 2;
        } else if ((c & 0xF0) == 0xE0) {
            clen = 3;
        } else if ((c & 0xF8) == 0xF0) {
            clen = 4;
        } else {
            clen = 1;
        }
        if (*len + clen + 1 > cap) {
            dst[*len] = '\0';
            return false;
        }
        memcpy(dst + *len, src, clen);
        *len += clen;
        src += clen;
    }
    dst[*len] = '\0';
    return true;
}

void cpe_sms_merge_parts(cpe_sms_list_t *list)
{
    if (list == NULL) {
        return;
    }
    bool drop[CPE_SMS_MAX_MESSAGES] = { false };
    bool done[CPE_SMS_MAX_MESSAGES] = { false };

    for (size_t i = 0; i < list->count; i++) {
        if (done[i]) {
            continue;
        }
        cpe_sms_msg_t *m = &list->items[i];
        m->index_count = 1;
        m->indexes[0] = m->index;
        if (m->part_total <= 1 || m->part_seq < 1 || m->part_seq > m->part_total) {
            done[i] = true;
            continue;
        }

        size_t order[CPE_SMS_MAX_PARTS];
        size_t n = 0;
        for (size_t j = i; j < list->count; j++) {
            cpe_sms_msg_t *p = &list->items[j];
            if (done[j] || p->part_total != m->part_total ||
                p->uid != m->uid || strcmp(p->from, m->from) != 0 ||
                p->part_seq < 1 || p->part_seq > p->part_total) {
                continue;
            }
            bool dup = false;
            for (size_t k = 0; k < n; k++) {
                if (list->items[order[k]].part_seq == p->part_seq) {
                    dup = true;
                    break;
                }
            }
            if (!dup && n < CPE_SMS_MAX_PARTS) {
                order[n++] = j;
            }
        }
        if (n == 0) {
            done[i] = true;
            continue;
        }
        for (size_t a = 1; a < n; a++) { /* insertion sort by part_seq */
            size_t key = order[a];
            size_t b = a;
            while (b > 0 &&
                   list->items[order[b - 1]].part_seq > list->items[key].part_seq) {
                order[b] = order[b - 1];
                b--;
            }
            order[b] = key;
        }

        size_t base_i = order[0];
        cpe_sms_msg_t *base = &list->items[base_i];
        size_t len = strlen(base->body);
        bool cut = false;
        bool unread = false;
        for (size_t k = 0; k < n; k++) {
            cpe_sms_msg_t *p = &list->items[order[k]];
            if (cpe_sms_stat_unread(p->stat)) {
                unread = true;
            }
            if (k > 0 && !append_utf8(base->body, sizeof(base->body), &len, p->body)) {
                cut = true;
            }
            base->indexes[k] = p->index;
            done[order[k]] = true;
            drop[order[k]] = true;
        }
        drop[base_i] = false;
        base->index_count = (uint8_t)n;
        base->index = base->indexes[0];
        base->truncated = cut || n < (size_t)base->part_total || base->truncated;
        if (unread) {
            snprintf(base->stat, sizeof(base->stat), "REC UNREAD");
        }
    }

    size_t w = 0;
    for (size_t r = 0; r < list->count; r++) {
        if (drop[r]) {
            continue;
        }
        if (w != r) {
            list->items[w] = list->items[r];
        }
        w++;
    }
    list->count = w;
}

bool cpe_sms_parse_cmti(const char *line, int *index)
{
    if (line == NULL || index == NULL || strncmp(line, "+CMTI:", 6) != 0) {
        return false;
    }

    const char *comma = strrchr(line, ',');
    if (comma == NULL) {
        return false;
    }

    const char *p = comma + 1;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }

    int value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        if (value > 1000000) {
            return false;
        }
        p++;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p != '\0') {
        return false;
    }

    *index = value;
    return true;
}
