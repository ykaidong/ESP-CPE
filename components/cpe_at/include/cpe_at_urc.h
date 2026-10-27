/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CPE_AT_URC_MAX_SUBSCRIBERS 8
#define CPE_AT_URC_MAX_PREFIXES    8
/* Includes the terminating NUL: a longer prefix is rejected. */
#define CPE_AT_URC_PREFIX_MAX_LEN  32

/* Called on the URC task for every complete line that starts with one of the
 * subscription's prefixes. The line is NUL-terminated and has no CR/LF. The
 * handler may block and may use cpe_at_session() / cpe_at_modem_reset(); it
 * runs serially with all other handlers, so a slow one delays later events and
 * the delivery queue eventually drops them. */
typedef void (*cpe_at_urc_handler_t)(const char *line, void *ctx);

/* The descriptor and `ctx` must stay valid for the lifetime of the program
 * (normally static storage): a line already queued for delivery still uses
 * them after unsubscribe. Prefix strings are copied at subscribe time. */
typedef struct {
    const char *prefixes[CPE_AT_URC_MAX_PREFIXES];
    size_t prefix_count;
    cpe_at_urc_handler_t handler;
    void *ctx;
} cpe_at_urc_subscription_t;

/** Register a subscription. Safe to call before or after cpe_at_start().
 *  Subscribing the same descriptor twice is a no-op. Returns
 *  ESP_ERR_INVALID_ARG for an empty list, an empty/oversized prefix or a NULL
 *  handler, and ESP_ERR_NO_MEM when every slot is taken. */
esp_err_t cpe_at_urc_subscribe(const cpe_at_urc_subscription_t *sub);

/** Remove a subscription. Returns ESP_ERR_NOT_FOUND when it was not
 *  registered. Lines already queued are still delivered. */
esp_err_t cpe_at_urc_unsubscribe(const cpe_at_urc_subscription_t *sub);

#ifdef __cplusplus
}
#endif
