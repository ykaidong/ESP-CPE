/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Configuration snapshot and persistence primitives shared inside cpe_core.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h"

#include "cpe_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CPE_ADMIN_SALT_LEN 16
#define CPE_ADMIN_HASH_LEN 32

/* The whole persisted state in one snapshot: a transaction builds the next
 * state locally and only publishes it after every NVS write succeeded. */
typedef struct {
    cpe_config_t cfg;
    uint8_t adm_salt[CPE_ADMIN_SALT_LEN];
    uint8_t adm_hash[CPE_ADMIN_HASH_LEN];
    bool adm_must_change;
} cpe_config_state_t;

/**
 * @brief Derive the admin password hash with PBKDF2-HMAC-SHA256.
 *
 * Long running (10000 iterations): callers must not hold the configuration
 * lock while this runs.
 */
esp_err_t cpe_config_store_pbkdf2(const char *password, const uint8_t *salt,
                                  size_t salt_len, uint8_t out[CPE_ADMIN_HASH_LEN]);

/**
 * @brief Fill @p st with the secure defaults.
 *
 * @param regenerate_credentials  Also derive the admin hash from the generated
 *                                AP password and force a password change.
 */
esp_err_t cpe_config_store_apply_defaults(cpe_config_state_t *st,
                                          bool regenerate_credentials);

/**
 * @brief Load and validate the stored configuration.
 *
 * @param found  Set to true when a namespace of the expected version exists,
 *               even if its content turns out to be incomplete or invalid.
 * @return true when the snapshot is complete and valid.
 */
bool cpe_config_store_load(nvs_handle_t nvs, cpe_config_state_t *st, bool *found);

/** Write every key plus the schema version (first boot / factory reset). */
esp_err_t cpe_config_store_persist_full(nvs_handle_t nvs, const cpe_config_state_t *st);

/**
 * @brief Write only the keys that differ from @p old.
 *
 * On failure every already written key is restored to its @p old value. The
 * original error is returned; @p old and @p next are never modified.
 */
esp_err_t cpe_config_store_persist_diff(nvs_handle_t nvs,
                                        const cpe_config_state_t *old,
                                        const cpe_config_state_t *next);

#ifdef __cplusplus
}
#endif
