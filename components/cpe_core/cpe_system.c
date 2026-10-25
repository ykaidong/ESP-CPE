/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>

#include "esp_log.h"

#include "cpe_config.h"
#include "cpe_recovery.h"
#include "cpe_system.h"

static const char *TAG = "cpe_system";

typedef struct {
    cpe_system_factory_reset_hook_t hook;
    const char *owner;
} hook_entry_t;

/* Registered from start-up code only (web console and maintenance task), never
 * while a reset is running, so no lock is needed. */
static hook_entry_t s_hooks[CPE_SYSTEM_FACTORY_RESET_HOOKS];
static size_t s_hook_count;

esp_err_t cpe_system_register_factory_reset_hook(cpe_system_factory_reset_hook_t hook,
                                                 const char *owner)
{
    if (hook == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_hook_count >= CPE_SYSTEM_FACTORY_RESET_HOOKS) {
        ESP_LOGE(TAG, "No room for the factory-reset hook of %s", owner ? owner : "?");
        return ESP_ERR_NO_MEM;
    }
    s_hooks[s_hook_count].hook = hook;
    s_hooks[s_hook_count].owner = owner;
    s_hook_count++;
    return ESP_OK;
}

esp_err_t cpe_system_factory_reset(void)
{
    /* Configuration first: it is the point of no return, and the hooks below
     * only erase data that belongs to an already reset device. */
    esp_err_t err = cpe_config_factory_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < s_hook_count; i++) {
        s_hooks[i].hook();
    }

    /* Recovery state, not configuration: clearing it here instead of inside
     * cpe_config_factory_reset() keeps the configuration layer free of the
     * recovery section. */
    (void)cpe_boot_count_clear();
    ESP_LOGI(TAG, "Factory settings restored (AP credentials kept)");
    return ESP_OK;
}
