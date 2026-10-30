/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SMS-DELIVER and SMS-SUBMIT PDU decoding (3GPP TS 23.040), independent of the
 * AT layers.
 */
#pragma once

#include "cpe_sms_parse.h"

typedef enum {
    CPE_SMS_PDU_OK = 0,
    CPE_SMS_PDU_MALFORMED,   /* structurally invalid or truncated */
    CPE_SMS_PDU_UNSUPPORTED, /* valid but not text we can decode (8-bit, ...) */
    CPE_SMS_PDU_NO_MEM,      /* the text buffer could not be allocated */
} cpe_sms_pdu_status_t;

/* Decode one SMS-DELIVER or SMS-SUBMIT PDU (SMSC address included, as
 * `AT+CMGL=4` returns it) into `out`. The caller zero-initializes `out`; `stat`
 * is left untouched.
 *
 * Fills `from`, `time`, `body`, `truncated` and the concatenation fields
 * (`uid`, `part_seq`, `part_total`; 1/1 for a single message). For a submit,
 * `from` holds the destination address and `time` stays empty because the PDU
 * carries no timestamp. `body` is a heap copy owned by the caller (free it, or
 * hand `out` to cpe_sms_list_clear); it is NULL when nothing could be decoded,
 * so the caller can keep the raw hex instead.
 *
 * Returns CPE_SMS_PDU_OK when the text (possibly empty) was decoded. */
cpe_sms_pdu_status_t cpe_sms_pdu_decode(const char *hex, cpe_sms_msg_t *out);
