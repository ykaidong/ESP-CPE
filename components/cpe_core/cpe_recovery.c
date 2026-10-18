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

static uint32_t read_count(void)
{
    nvs_handle_t h;
    uint32_t value = 0;
    if (nvs_open(CPE_SYS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u32(h, NVS_KEY_BOOT_COUNT, &value) != ESP_OK) {
            value = 0;
        }
        nvs_close(h);
    }
    return value;
}

uint32_t cpe_boot_count_get(void)
{
    return read_count();
}

uint32_t cpe_boot_count_increment(void)
{
    nvs_handle_t h;
    uint32_t value = read_count() + 1;
    if (nvs_open(CPE_SYS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u32(h, NVS_KEY_BOOT_COUNT, value) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
    ESP_LOGW(TAG, "Boot/recovery count: %u", value);
    return value;
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
