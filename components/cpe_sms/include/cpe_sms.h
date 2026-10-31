/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "cpe_sms_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CPE_SMS_UNKNOWN = 0,      /* modem offline, or capability not probed yet */
    CPE_SMS_READY,            /* capability probed and usable */
    CPE_SMS_UNSUPPORTED,      /* modem answered but rejected the SMS commands */
} cpe_sms_state_t;

/** Start the SMS probe task. Safe to call once; a no-op afterwards. */
esp_err_t cpe_sms_init(void);

cpe_sms_state_t cpe_sms_state(void);

/** Monotonic counter bumped whenever the stored messages change: a +CMTI
 *  indication, a send, a delete, or the storage-full prune. Lets the console
 *  decide whether a full list is worth an AT session. */
uint32_t cpe_sms_inbox_generation(void);

/** Monotonic counter bumped only by a +CMTI indication. A forwarding task that
 *  only cares about newly received messages polls this instead of the inbox
 *  generation, so its own sends and deletes do not trigger a listing. An
 *  indication can be lost (URC queue overflow, modem reset), so callers still
 *  need a slow full-list fallback. */
uint32_t cpe_sms_rx_generation(void);

/** List stored messages into `out` (caller allocates; a few KB for the struct,
 *  plus a heap copy per body: release both with cpe_sms_list_clear and free).
 *  Received over AT+CMGL=4 in PDU mode; concatenated parts are merged into one
 *  entry using the UDH. Records that cannot be decoded keep their raw hex.
 *  `out->storage_used/capacity` report AT+CPMS (-1 when unknown).
 *  Blocks up to `timeout_ms`. */
esp_err_t cpe_sms_list(cpe_sms_list_t *out, uint32_t timeout_ms);

/** Send one SMS. On failure `*err_str` (when non-NULL) receives a static
 *  error code string ("sms_bad_number", "sms_bad_text", "sms_text_too_long",
 *  "sms_rate_limited", "sms_busy", "sms_no_prompt", "sms_send_timeout",
 *  "sms_send_failed"). */
esp_err_t cpe_sms_send(const char *number, const char *text, const char **err_str,
                       uint32_t timeout_ms);

/** Delete several storage records in one AT session (a concatenated message
 *  spans one record per segment). */
esp_err_t cpe_sms_delete_many(const int *indexes, size_t count, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
