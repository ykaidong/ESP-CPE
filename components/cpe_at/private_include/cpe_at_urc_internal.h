/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the registry lock, delivery queue and URC task. Idempotent. */
esp_err_t cpe_at_urc_init(void);

/* Offer one complete, unterminated line to the registry. Returns true when at
 * least one subscription matched; the caller then keeps the line out of the
 * command response. Matching is synchronous, delivery is queued. */
bool cpe_at_urc_dispatch(const char *line, size_t len);

/* True when an unterminated line could still grow into a subscribed URC. */
bool cpe_at_urc_partial_subscribed(const char *partial, size_t len);

/* True when at least one subscription is registered. */
bool cpe_at_urc_has_subscribers(void);

#ifdef __cplusplus
}
#endif
