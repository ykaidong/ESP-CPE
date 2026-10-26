/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Must cover the modem's ME storage (23 slots on the EG25), otherwise the
 * newest records would be parsed away and the UI could neither show nor
 * delete them. */
#define CPE_SMS_MAX_MESSAGES 24
#define CPE_SMS_MAX_PARTS    8
#define CPE_SMS_MAX_ADDR     24
#define CPE_SMS_MAX_TIME     24
/* A single UCS2 segment carries 67 code units (up to 201 UTF-8 bytes for CJK).
 * 768 bytes hold three full segments plus a little slack; longer messages keep
 * the leading part and set `truncated`. */
#define CPE_SMS_MAX_BODY     768
#define CPE_SMS_MAX_STAT     24
/* The CMGL response stores text fields as UCS2 hex (4 chars per code unit)
 * before decoding: a 21-char number -> 84 hex chars, a 160-char ASCII body ->
 * 640 hex chars. These caps bound the raw hex buffers, not the decoded text. */
#define CPE_SMS_MAX_ADDR_HEX 88
#define CPE_SMS_MAX_BODY_HEX 648

typedef struct {
    int index;                        /* storage index shown in the UI */
    int indexes[CPE_SMS_MAX_PARTS];   /* every storage index of this message */
    uint8_t index_count;              /* valid entries in `indexes` */
    int uid;                          /* concatenation reference, 0 if single */
    uint8_t part_seq;                 /* 1-based segment number */
    uint8_t part_total;               /* 1 when the message is not concatenated */
    bool truncated;                   /* merged text cut, or a part was missing */
    char stat[CPE_SMS_MAX_STAT];
    char from[CPE_SMS_MAX_ADDR];
    char time[CPE_SMS_MAX_TIME];
    char body[CPE_SMS_MAX_BODY];
} cpe_sms_msg_t;

typedef struct {
    cpe_sms_msg_t items[CPE_SMS_MAX_MESSAGES];
    size_t count;
    bool truncated;                   /* list hit CPE_SMS_MAX_MESSAGES */
    int storage_used;                 /* CPMS <used>; negative when unknown */
    int storage_capacity;             /* CPMS <total>; negative when unknown */
} cpe_sms_list_t;

/* Parse a raw AT+CMGL="ALL" response into `out` (empty list is well-formed).
 * `from` and `body` are UCS2-decoded; `stat`/`time` stay ASCII. Returns true
 * when the response parsed without structural errors. */
bool cpe_sms_parse_cmgl(const char *raw, cpe_sms_list_t *out);

/* Parse a `+CPMS: "ME",<used>,<total>,"ME",...` response (or the equivalent
 * `+CPMS: <used>,<total>,...` form). Returns true when the first triple was
 * found; `used`/`total` are left untouched otherwise. */
bool cpe_sms_parse_cpms(const char *resp, int *used, int *total);

/* Parse the trailing `<uid>,<seg>,<total>` of a `+QCMGR:` header line.
 * Returns false when the line is not a QCMGR header or the fields are not
 * positive integers. */
bool cpe_sms_parse_qcmgr_seg(const char *line, int *uid, int *seg, int *total);

/* Merge concatenated parts in `list` in place: entries sharing from+uid+total
 * become one message ordered by part_seq, with all their storage indexes
 * collected in `indexes`. Entries whose metadata is missing (part_total <= 1)
 * are left alone. */
void cpe_sms_merge_parts(cpe_sms_list_t *list);

/* True when `stat` marks the message unread (substring "UNREAD", any case). */
bool cpe_sms_stat_unread(const char *stat);

/* True for an optional leading '+' followed by 3..20 digits. */
bool cpe_sms_number_valid(const char *num);

/* True when text is non-empty and fits a single SMS; sets *is_ascii and *units
 * (budget 160 for ASCII, 70 for UCS2 code units). */
bool cpe_sms_text_measure(const char *text, bool *is_ascii, int *units);
