/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure parsing/validation for SMS, host-tested.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpe_sms_parse.h"
#include "cpe_sms_pdu.h"

_Static_assert(CPE_SMS_MAX_BODY >= CPE_SMS_MAX_PARTS * 201 + 1,
               "body buffer must hold one full UCS2 segment per part");
_Static_assert(CPE_SMS_PDU_MAX_HEX >= 2 * (1 + 12 + 163),
               "PDU hex buffer must hold an SMSC field plus a full TPDU");

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

/* "+CMGL: <index>,<stat>,[<alpha>],<length>" in PDU mode. The fields after
 * <stat> are informational, so only the first two are parsed. */
static bool parse_cmgl_pdu_header(const char *line, int *index, cpe_sms_stat_t *stat)
{
    const char *p = line + 6;
    char *end = NULL;

    while (*p == ' ') {
        p++;
    }
    long idx = strtol(p, &end, 10);
    if (end == p || idx < 0 || idx > 1000000) {
        return false;
    }
    p = end;
    while (*p == ' ') {
        p++;
    }
    if (*p != ',') {
        return false;
    }
    p++;
    while (*p == ' ') {
        p++;
    }
    long st = strtol(p, &end, 10);
    if (end == p || st < 0 || st > 3) {
        return false;
    }

    *index = (int)idx;
    switch (st) {
    case 0:  *stat = CPE_SMS_STAT_UNREAD; break;
    case 1:  *stat = CPE_SMS_STAT_READ; break;
    case 2:  *stat = CPE_SMS_STAT_UNSENT; break;
    default: *stat = CPE_SMS_STAT_SENT; break;
    }
    return true;
}

/* Decode the accumulated hex into `m`; on failure keep the hex so the record
 * can still be shown and deleted. `truncated` stays clear: it means readable
 * text was cut off, and appending an ellipsis to undecoded hex would say that.
 * `body` stays NULL when even that copy cannot be allocated. */
static void finish_cmgl_pdu(cpe_sms_msg_t *m, const char *hex)
{
    cpe_sms_pdu_status_t status = cpe_sms_pdu_decode(hex, m);
    if (status == CPE_SMS_PDU_OK || status == CPE_SMS_PDU_NO_MEM) {
        return;
    }
    m->from[0] = '\0';
    m->time[0] = '\0';
    m->uid = 0;
    m->part_seq = 1;
    m->part_total = 1;
    size_t n = strlen(hex);
    if (n >= CPE_SMS_MAX_BODY) {
        n = CPE_SMS_MAX_BODY - 1;
    }
    m->body = malloc(n + 1);
    if (m->body != NULL) {
        memcpy(m->body, hex, n);
        m->body[n] = '\0';
    }
}

bool cpe_sms_parse_cmgl_pdu(const char *raw, cpe_sms_list_t *out)
{
    if (raw == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->storage_used = -1;
    out->storage_capacity = -1;

    char line[CPE_SMS_PDU_MAX_HEX + 1];
    char hex[CPE_SMS_PDU_MAX_HEX + 1];
    size_t hex_len = 0;
    cpe_sms_msg_t *cur = NULL;
    const char *p = raw;

    while (p != NULL && *p != '\0') {
        const char *eol = strstr(p, "\r\n");
        size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
        size_t cl = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, cl);
        line[cl] = '\0';

        if (strncmp(line, "+CMGL:", 6) == 0) {
            if (cur != NULL) {
                finish_cmgl_pdu(cur, hex);
                cur = NULL;
            }
            if (out->count >= CPE_SMS_MAX_MESSAGES) {
                out->truncated = true;
                return true;
            }
            int index;
            cpe_sms_stat_t stat;
            if (!parse_cmgl_pdu_header(line, &index, &stat)) {
                p = eol != NULL ? eol + 2 : NULL;
                continue;
            }
            cur = &out->items[out->count];
            memset(cur, 0, sizeof(*cur));
            cur->index = index;
            cur->stat = stat;
            out->count++;
            hex[0] = '\0';
            hex_len = 0;
        } else if (cur != NULL && is_terminal_line(line)) {
            finish_cmgl_pdu(cur, hex);
            cur = NULL;
        } else if (cur != NULL && line[0] != '\0') {
            size_t alen = strlen(line);
            if (hex_len + alen + 1 < sizeof(hex)) {
                memcpy(hex + hex_len, line, alen + 1);
                hex_len += alen;
            }
        }

        p = eol != NULL ? eol + 2 : NULL;
    }

    if (cur != NULL) {
        finish_cmgl_pdu(cur, hex);
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
        memmove(dst + *len, src, clen);
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
        char *merged = malloc(CPE_SMS_MAX_BODY);
        if (merged == NULL) {
            /* Keep the parts as separate entries rather than lose their text. */
            done[i] = true;
            continue;
        }
        merged[0] = '\0';
        size_t len = 0;
        bool cut = false;
        bool unread = false;
        bool part_truncated = false;
        for (size_t k = 0; k < n; k++) {
            cpe_sms_msg_t *p = &list->items[order[k]];
            if (p->stat == CPE_SMS_STAT_UNREAD) {
                unread = true;
            }
            if (p->truncated) {
                part_truncated = true;
            }
            if (p->body == NULL ||
                !append_utf8(merged, CPE_SMS_MAX_BODY, &len, p->body)) {
                cut = true;
            }
            base->indexes[k] = p->index;
            done[order[k]] = true;
            drop[order[k]] = true;
        }
        free(base->body);
        base->body = merged;
        for (size_t k = 1; k < n; k++) {
            free(list->items[order[k]].body);
            list->items[order[k]].body = NULL;
        }
        drop[base_i] = false;
        base->index_count = (uint8_t)n;
        base->index = base->indexes[0];
        base->truncated = cut || part_truncated || n < (size_t)base->part_total;
        if (unread) {
            base->stat = CPE_SMS_STAT_UNREAD;
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
    /* Moved entries leave a stale copy of their body pointer behind; clear the
     * vacated tail so cpe_sms_list_clear cannot free the same body twice. */
    if (w < list->count) {
        memset(&list->items[w], 0, (list->count - w) * sizeof(list->items[0]));
    }
    list->count = w;
}

void cpe_sms_list_clear(cpe_sms_list_t *list)
{
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < CPE_SMS_MAX_MESSAGES; i++) {
        free(list->items[i].body);
        list->items[i].body = NULL;
    }
    list->count = 0;
    list->truncated = false;
    list->storage_used = -1;
    list->storage_capacity = -1;
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
