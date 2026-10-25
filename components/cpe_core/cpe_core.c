/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "cpe_core.h"

static const char *TAG = "cpe_core";

esp_err_t cpe_core_init(void)
{
    static bool s_core_inited = false;
    if (s_core_inited) {
        return ESP_OK;
    }

    esp_err_t err = cpe_config_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cpe_config_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = cpe_auth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cpe_auth_init failed: %s", esp_err_to_name(err));
        /* Release the NVS handle: main() erases NVS and retries once. */
        cpe_config_deinit();
        return err;
    }

    err = cpe_status_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cpe_status_init failed: %s", esp_err_to_name(err));
        cpe_config_deinit();
        return err;
    }

    uint32_t boot_count = 0;
    esp_err_t count_err = cpe_boot_count_increment(&boot_count);
    cpe_status_set_boot_count(boot_count);
    if (count_err != ESP_OK) {
        ESP_LOGW(TAG, "Boot counter is not persistent (%s)",
                 esp_err_to_name(count_err));
    }

    ESP_LOGI(TAG, "CPE core initialized (boot count %u, first boot %d)",
             boot_count, cpe_config_first_boot());
    s_core_inited = true;
    return ESP_OK;
}
