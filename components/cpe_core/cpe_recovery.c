/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"

#include "cpe_kv.h"
#include "cpe_recovery.h"

static const char *TAG = "cpe_recovery";

#define NVS_KEY_BOOT_COUNT  "boot_cnt"
#define NVS_KEY_MAINT_COUNT "mnt_cnt"

/* A missing namespace/key is not a failure: it simply means the counter has
 * not been written yet. Real NVS errors are logged and returned. */
static esp_err_t read_u32(const char *key, uint32_t *out)
{
    cpe_kv_t kv;
    esp_err_t err = cpe_kv_open(&kv, CPE_KV_NS_SYSTEM, false);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        *out = 0;
        return err;
    }
    err = cpe_kv_get_u32(&kv, key, out);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = 0;
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read %s: %s", key, esp_err_to_name(err));
        *out = 0;
    }
    cpe_kv_close(&kv);
    return err;
}

static esp_err_t write_u32(const char *key, uint32_t value)
{
    cpe_kv_t kv;
    esp_err_t err = cpe_kv_open(&kv, CPE_KV_NS_SYSTEM, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open %s for writing: %s",
                 CPE_KV_NS_SYSTEM, esp_err_to_name(err));
        return err;
    }
    err = cpe_kv_set_u32(&kv, key, value);
    if (err == ESP_OK) {
        err = cpe_kv_commit(&kv);
    }
    cpe_kv_close(&kv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist %s: %s", key, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t read_count(uint32_t *out)
{
    return read_u32(NVS_KEY_BOOT_COUNT, out);
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
    /* Same helper as the maintenance counter: open, set, commit and close in
     * one place, so both counters fail the same way. */
    err = write_u32(NVS_KEY_BOOT_COUNT, value);
    ESP_LOGW(TAG, "Boot/recovery count: %u", value);
    *count = value;
    return err;
}

esp_err_t cpe_boot_count_clear(void)
{
    return write_u32(NVS_KEY_BOOT_COUNT, 0);
}

esp_err_t cpe_maint_reboot_count_get(uint32_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_u32(NVS_KEY_MAINT_COUNT, count);
}

esp_err_t cpe_maint_reboot_count_increment(uint32_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t value = 0;
    esp_err_t err = read_u32(NVS_KEY_MAINT_COUNT, &value);
    if (err != ESP_OK) {
        *count = 0;
        return err;
    }
    value++;
    err = write_u32(NVS_KEY_MAINT_COUNT, value);
    ESP_LOGW(TAG, "Scheduled reboot count: %u", value);
    *count = value;
    return err;
}

esp_err_t cpe_maint_reboot_count_clear(void)
{
    return write_u32(NVS_KEY_MAINT_COUNT, 0);
}
