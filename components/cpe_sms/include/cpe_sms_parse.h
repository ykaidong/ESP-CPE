/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define CPE_SMS_MAX_MESSAGES 20
#define CPE_SMS_MAX_ADDR     24
#define CPE_SMS_MAX_TIME     24
#define CPE_SMS_MAX_BODY     240
#define CPE_SMS_MAX_STAT     24
/* The CMGL response stores text fields as UCS2 hex (4 chars per code unit)
 * before decoding: a 21-char number -> 84 hex chars, a 160-char ASCII body ->
 * 640 hex chars. These caps bound the raw hex buffers, not the decoded text. */
#define CPE_SMS_MAX_ADDR_HEX 88
#define CPE_SMS_MAX_BODY_HEX 648

typedef struct {
    int index;
    char stat[CPE_SMS_MAX_STAT];
    char from[CPE_SMS_MAX_ADDR];
    char time[CPE_SMS_MAX_TIME];
    char body[CPE_SMS_MAX_BODY];
} cpe_sms_msg_t;

typedef struct {
    cpe_sms_msg_t items[CPE_SMS_MAX_MESSAGES];
    size_t count;
    bool truncated;
} cpe_sms_list_t;

/* Parse a raw AT+CMGL="ALL" response into `out` (empty list is well-formed).
 * `from` and `body` are UCS2-decoded; `stat`/`time` stay ASCII. Returns true
 * when the response parsed without structural errors. */
bool cpe_sms_parse_cmgl(const char *raw, cpe_sms_list_t *out);

/* True when `stat` marks the message unread (substring "UNREAD", any case). */
bool cpe_sms_stat_unread(const char *stat);

/* True for an optional leading '+' followed by 3..20 digits. */
bool cpe_sms_number_valid(const char *num);

/* True when text is non-empty and fits a single SMS; sets *is_ascii and *units
 * (budget 160 for ASCII, 70 for UCS2 code units). */
bool cpe_sms_text_measure(const char *text, bool *is_ascii, int *units);
