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
#define CPE_SMS_MAX_PARTS    10
/* The longest alphanumeric sender is 11 septets; every character can expand to
 * three UTF-8 bytes, so 40 bytes hold a fully decoded one. */
#define CPE_SMS_MAX_ADDR     40
#define CPE_SMS_MAX_TIME     24
/* A concatenated UCS2 segment carries 67 code units (up to 201 UTF-8 bytes for
 * CJK). 2048 bytes hold ten full segments; longer messages keep the leading
 * part, retain every storage index, and set `truncated`. */
#define CPE_SMS_MAX_BODY     2048
/* UCS2 hex used to encode a phone number for AT+CMGS (21 chars -> 84 digits). */
#define CPE_SMS_MAX_ADDR_HEX 88
/* A PDU is at most 140 octets plus a 12-octet SMSC address; hex doubles that. */
#define CPE_SMS_PDU_MAX_HEX  400

typedef enum {
    CPE_SMS_STAT_UNREAD = 0,
    CPE_SMS_STAT_READ,
    CPE_SMS_STAT_UNSENT,
    CPE_SMS_STAT_SENT,
    CPE_SMS_STAT_UNKNOWN,
} cpe_sms_stat_t;

typedef struct {
    int index;                        /* storage index shown in the UI */
    int indexes[CPE_SMS_MAX_PARTS];   /* every storage index of this message */
    uint8_t index_count;              /* valid entries in `indexes` */
    int uid;                          /* concatenation reference, 0 if single */
    uint8_t part_seq;                 /* 1-based segment number */
    uint8_t part_total;               /* 1 when the message is not concatenated */
    bool truncated;                   /* text cut, a part missing, or not decodable */
    cpe_sms_stat_t stat;
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

/* Parse a raw AT+CMGL=4 (PDU mode) response into `out`. Each record is decoded
 * with cpe_sms_pdu_decode_deliver(); a record that cannot be decoded is kept
 * with its raw hex as `body` and `truncated` set. Returns false only for a
 * NULL argument. */
bool cpe_sms_parse_cmgl_pdu(const char *raw, cpe_sms_list_t *out);

/* Parse a `+CPMS: "ME",<used>,<total>,"ME",...` response (or the equivalent
 * `+CPMS: <used>,<total>,...` form). Returns true when the first triple was
 * found; `used`/`total` are left untouched otherwise. */
bool cpe_sms_parse_cpms(const char *resp, int *used, int *total);

/* Parse the storage index from a `+CMTI: "ME",<index>` indication. Returns
 * false when the line is not a CMTI indication or the index is not a
 * non-negative integer. */
bool cpe_sms_parse_cmti(const char *line, int *index);

/* Merge concatenated parts in `list` in place: entries sharing from+uid+total
 * become one message ordered by part_seq, with all their storage indexes
 * collected in `indexes`. Entries whose metadata is missing (part_total <= 1)
 * are left alone. */
void cpe_sms_merge_parts(cpe_sms_list_t *list);

/* True for an optional leading '+' followed by 3..20 digits. */
bool cpe_sms_number_valid(const char *num);

/* True when text is non-empty and fits a single SMS; sets *is_ascii and *units
 * (budget 160 for ASCII, 70 for UCS2 code units). */
bool cpe_sms_text_measure(const char *text, bool *is_ascii, int *units);
