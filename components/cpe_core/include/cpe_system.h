/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device-level operations that span several components.
 *
 * The factory reset is the only operation in this file: it has to erase the
 * configuration (cpe_core) together with the data owned by other components
 * (station nicknames, the maintenance schedule). Those components register a
 * hook instead of the reset layer depending on them, so both entry points - the
 * web console and the physical reset button - leave exactly the same state
 * behind.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of hooks cpe_system_factory_reset() can run. */
#define CPE_SYSTEM_FACTORY_RESET_HOOKS      4

/**
 * @brief Hook run by cpe_system_factory_reset() once the configuration is gone.
 *
 * Must not fail: the configuration has already been erased, so the hook only
 * erases its own persisted data (log the error if it cannot).
 *
 * @note Register during start-up, before the console or the reset button can
 *       trigger a reset.
 */
typedef void (*cpe_system_factory_reset_hook_t)(void);

/**
 * @brief Register a hook for cpe_system_factory_reset().
 *
 * @param hook   Hook to run, must not be NULL.
 * @param owner  Component name, only used for logging (may be NULL).
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM when the hook table is
 *         full.
 */
esp_err_t cpe_system_register_factory_reset_hook(cpe_system_factory_reset_hook_t hook,
                                                 const char *owner);

/**
 * @brief Restore the factory settings.
 *
 * Erases the configuration (the device-unique AP SSID/password are kept, the
 * admin password goes back to the AP password), runs every registered hook and
 * clears the persistent boot/recovery counter so the device starts with a fresh
 * recovery budget.
 *
 * The caller is responsible for restarting (both current callers do); the
 * in-RAM authentication state is left to the caller as well, because only the
 * web console has a session to drop.
 */
esp_err_t cpe_system_factory_reset(void);

#ifdef __cplusplus
}
#endif
