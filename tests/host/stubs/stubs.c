/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implementations for the host stubs. Every function here stands in for
 * something the host cannot provide (NVS, PSA crypto, FreeRTOS locks, the HTTP
 * transport). They are deliberately trivial: the host suite covers parsing and
 * validation, none of which may depend on persistence, crypto or scheduling.
 *
 * If one of these ever grows real behaviour, the test is testing the stub.
 */

#include <string.h>

#include "cpe_config_priv.h"
#include "cpe_kv.h"
#include "esp_err.h"
#include "esp_random.h"
#include "freertos/semphr.h"
#include "http_internal.h"
#include "psa/crypto.h"

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "host-stub";
}

/* Deterministic, so a test that depends on randomness fails the same way twice. */
uint32_t esp_random(void)
{
    return 0xA5A5A5A5U;
}

void esp_fill_random(void *buf, size_t len)
{
    if (buf != NULL && len > 0) {
        memset(buf, 0xA5, len);
    }
}

psa_status_t psa_crypto_init(void)
{
    return PSA_SUCCESS;
}

static int s_lock_object;

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    return &s_lock_object;
}

void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    (void)sem;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t sem, TickType_t ticks)
{
    (void)sem;
    (void)ticks;
    return pdTRUE;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t sem)
{
    (void)sem;
    return pdTRUE;
}

static nvs_handle_t s_next_handle = 1;

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out)
{
    (void)ns;
    (void)mode;
    if (out != NULL) {
        *out = s_next_handle++;
    }
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    (void)key;
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *len)
{
    (void)handle;
    (void)key;
    (void)out;
    (void)len;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    (void)handle;
    (void)key;
    (void)out;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out)
{
    (void)handle;
    (void)key;
    (void)out;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t len)
{
    (void)handle;
    (void)key;
    (void)value;
    (void)len;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len)
{
    (void)handle;
    (void)key;
    (void)out;
    (void)len;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t cpe_kv_open(cpe_kv_t *kv, const char *ns, bool write)
{
    (void)write;
    if (kv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    kv->handle = 1;
    kv->ns = ns;
    return ESP_OK;
}

void cpe_kv_close(cpe_kv_t *kv)
{
    (void)kv;
}

esp_err_t cpe_kv_commit(cpe_kv_t *kv)
{
    (void)kv;
    return ESP_OK;
}

esp_err_t cpe_kv_erase_all(cpe_kv_t *kv)
{
    (void)kv;
    return ESP_OK;
}

esp_err_t cpe_kv_get_str(const cpe_kv_t *kv, const char *key, char *out, size_t out_len)
{
    (void)kv;
    (void)key;
    (void)out;
    (void)out_len;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t cpe_kv_set_str(cpe_kv_t *kv, const char *key, const char *value)
{
    (void)kv;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t cpe_kv_get_u32(const cpe_kv_t *kv, const char *key, uint32_t *out)
{
    (void)kv;
    (void)key;
    (void)out;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t cpe_kv_set_u32(cpe_kv_t *kv, const char *key, uint32_t value)
{
    (void)kv;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t cpe_kv_get_u8(const cpe_kv_t *kv, const char *key, uint8_t *out)
{
    (void)kv;
    (void)key;
    (void)out;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t cpe_kv_set_u8(cpe_kv_t *kv, const char *key, uint8_t value)
{
    (void)kv;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t cpe_config_store_pbkdf2(const char *password, const uint8_t *salt,
                                  size_t salt_len, uint8_t out[CPE_ADMIN_HASH_LEN])
{
    (void)password;
    (void)salt;
    (void)salt_len;
    if (out != NULL) {
        memset(out, 0x5A, CPE_ADMIN_HASH_LEN);
    }
    return ESP_OK;
}

bool cpe_config_store_load(nvs_handle_t nvs, cpe_config_state_t *st, bool *found)
{
    (void)nvs;
    (void)st;
    if (found != NULL) {
        *found = false;
    }
    return false;
}

esp_err_t cpe_config_store_apply_defaults(cpe_config_state_t *st, bool regenerate_credentials)
{
    (void)regenerate_credentials;
    if (st != NULL) {
        memset(st, 0, sizeof(*st));
    }
    return ESP_OK;
}

esp_err_t cpe_config_store_persist_full(nvs_handle_t nvs, const cpe_config_state_t *st)
{
    (void)nvs;
    (void)st;
    return ESP_OK;
}

esp_err_t cpe_config_store_persist_diff(nvs_handle_t nvs, const cpe_config_state_t *old,
                                        const cpe_config_state_t *next)
{
    (void)nvs;
    (void)old;
    (void)next;
    return ESP_OK;
}

esp_err_t cpe_web_send_json_body(httpd_req_t *req, const char *status_line,
                                 const char *body, bool close_socket)
{
    (void)req;
    (void)status_line;
    (void)body;
    (void)close_socket;
    return ESP_OK;
}

esp_err_t cpe_web_send_error(httpd_req_t *req, const char *code)
{
    (void)req;
    (void)code;
    return ESP_OK;
}
