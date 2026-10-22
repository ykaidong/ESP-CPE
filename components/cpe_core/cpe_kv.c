/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "esp_log.h"

#include "cpe_kv.h"

static const char *TAG = "cpe_kv";

esp_err_t cpe_kv_open(cpe_kv_t *kv, const char *ns, bool write)
{
    if (kv == NULL || ns == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    kv->handle = 0;
    kv->ns = ns;

    esp_err_t err = nvs_open(ns, write ? NVS_READWRITE : NVS_READONLY, &kv->handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Nothing persisted yet: the caller applies its defaults. */
        kv->handle = 0;
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open %s: %s", ns, esp_err_to_name(err));
        kv->handle = 0;
        return err;
    }
    return ESP_OK;
}

void cpe_kv_close(cpe_kv_t *kv)
{
    if (kv == NULL || kv->handle == 0) {
        return;
    }
    nvs_close(kv->handle);
    kv->handle = 0;
}

esp_err_t cpe_kv_commit(cpe_kv_t *kv)
{
    if (kv == NULL || kv->handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_commit(kv->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit %s: %s", kv->ns ? kv->ns : "?",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t cpe_kv_erase_all(cpe_kv_t *kv)
{
    if (kv == NULL || kv->handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_erase_all(kv->handle);
}

esp_err_t cpe_kv_get_u32(const cpe_kv_t *kv, const char *key, uint32_t *out)
{
    if (kv == NULL || kv->handle == 0 || key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_get_u32(kv->handle, key, out);
}

esp_err_t cpe_kv_set_u32(cpe_kv_t *kv, const char *key, uint32_t value)
{
    if (kv == NULL || kv->handle == 0 || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_set_u32(kv->handle, key, value);
}

esp_err_t cpe_kv_get_u8(const cpe_kv_t *kv, const char *key, uint8_t *out)
{
    if (kv == NULL || kv->handle == 0 || key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_get_u8(kv->handle, key, out);
}

esp_err_t cpe_kv_set_u8(cpe_kv_t *kv, const char *key, uint8_t value)
{
    if (kv == NULL || kv->handle == 0 || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_set_u8(kv->handle, key, value);
}

esp_err_t cpe_kv_get_str(const cpe_kv_t *kv, const char *key, char *out, size_t out_len)
{
    if (kv == NULL || kv->handle == 0 || key == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = out_len;
    esp_err_t err = nvs_get_str(kv->handle, key, out, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read %s/%s: %s", kv->ns ? kv->ns : "?", key,
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t cpe_kv_set_str(cpe_kv_t *kv, const char *key, const char *value)
{
    if (kv == NULL || kv->handle == 0 || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_set_str(kv->handle, key, value);
}
