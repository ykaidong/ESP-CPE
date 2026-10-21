/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "nvs.h"
#include "cpe_recovery.h"

static const char *TAG = "cpe_recovery";

#define CPE_SYS_NAMESPACE   "cpe_sys"
#define NVS_KEY_BOOT_COUNT  "boot_cnt"

/* A missing namespace/key is not a failure: it simply means the counter has
 * not been written yet. Real NVS errors are logged and returned. */
static esp_err_t read_count(uint32_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CPE_SYS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open %s: %s", CPE_SYS_NAMESPACE, esp_err_to_name(err));
        *out = 0;
        return err;
    }
    err = nvs_get_u32(h, NVS_KEY_BOOT_COUNT, out);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = 0;
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read the boot count: %s", esp_err_to_name(err));
        *out = 0;
    }
    nvs_close(h);
    return err;
}

esp_err_t cpe_boot_count_get(uint32_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_count(count);
}

esp_err_t cpe_boot_count_increment(uint32_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t value = 0;
    esp_err_t err = read_count(&value);
    if (err != ESP_OK) {
        /* Do not overwrite the stored counter based on a failed read. */
        *count = 0;
        return err;
    }
    value++;

    nvs_handle_t h;
    err = nvs_open(CPE_SYS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open %s for writing: %s",
                 CPE_SYS_NAMESPACE, esp_err_to_name(err));
        *count = value;
        return err;
    }
    err = nvs_set_u32(h, NVS_KEY_BOOT_COUNT, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist the boot count: %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "Boot/recovery count: %u", value);
    *count = value;
    return err;
}

esp_err_t cpe_boot_count_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CPE_SYS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(h, NVS_KEY_BOOT_COUNT, 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
