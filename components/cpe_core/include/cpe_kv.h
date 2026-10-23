/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Thin wrapper over NVS.
 *
 * Four different namespaces and the same open/get/commit/close boilerplate used
 * to be repeated in every component that persists something. The
 * namespace strings live here so "what does the device store" can be answered
 * from one file, and the wrapper keeps the error handling in one place.
 *
 * The wrapper deliberately stays close to NVS: a caller that writes several keys
 * opens one session and commits once, exactly as before.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Persisted namespaces. Do not rename an existing string: NVS data would be
 * orphaned on devices already in the field. */
#define CPE_KV_NS_CONFIG        "cpe_cfg"       /* device configuration */
#define CPE_KV_NS_SYSTEM        "cpe_sys"       /* boot/recovery counters */
#define CPE_KV_NS_MAINT         "cpe_maint"     /* maintenance policy */
#define CPE_KV_NS_STA_NAMES     "sta_names"     /* station nicknames */

/** An open NVS namespace. Zero-initialise or use cpe_kv_open(). */
typedef struct {
    nvs_handle_t handle;
    const char *ns;         /* for logging only */
} cpe_kv_t;

/**
 * @brief Open a namespace.
 *
 * @param kv     Session to fill in.
 * @param ns     Namespace name (one of the CPE_KV_NS_* strings).
 * @param write  Open read-write instead of read-only.
 *
 * @return ESP_OK, ESP_ERR_NVS_NOT_FOUND when a read-only namespace does not
 *         exist yet, or another NVS error.
 */
esp_err_t cpe_kv_open(cpe_kv_t *kv, const char *ns, bool write);

/** Close the session; safe on an already closed session. */
void cpe_kv_close(cpe_kv_t *kv);

/** Commit pending writes of the session. */
esp_err_t cpe_kv_commit(cpe_kv_t *kv);

/** Erase every key of the namespace (no-op when it does not exist). */
esp_err_t cpe_kv_erase_all(cpe_kv_t *kv);

esp_err_t cpe_kv_get_u32(const cpe_kv_t *kv, const char *key, uint32_t *out);
esp_err_t cpe_kv_set_u32(cpe_kv_t *kv, const char *key, uint32_t value);
esp_err_t cpe_kv_get_u8(const cpe_kv_t *kv, const char *key, uint8_t *out);
esp_err_t cpe_kv_set_u8(cpe_kv_t *kv, const char *key, uint8_t value);

/**
 * @brief Read a string value.
 *
 * @param out      Destination buffer.
 * @param out_len  Size of @p out, including the terminator.
 *
 * @return ESP_OK, ESP_ERR_NVS_INVALID_LENGTH when the stored string does not
 *         fit, or another NVS error.
 */
esp_err_t cpe_kv_get_str(const cpe_kv_t *kv, const char *key, char *out, size_t out_len);

/** Write a string value (committed by the caller). */
esp_err_t cpe_kv_set_str(cpe_kv_t *kv, const char *key, const char *value);

#ifdef __cplusplus
}
#endif
