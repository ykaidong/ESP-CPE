/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the reset button GPIO and start the boot-print window.
 *
 * Must be called early in app_main(). The button is sampled by
 * reset_button_start(); a double-click within
 * CPE_CREDENTIAL_PRINT_WINDOW_MS after this call prints the credentials
 * (AP password always; admin password while it still equals the AP password).
 */
esp_err_t reset_button_init(void);

/**
 * @brief Start the button monitor task.
 *
 * - Double-click within CPE_CREDENTIAL_PRINT_WINDOW_MS after boot: print the
 *   SoftAP/admin credentials.
 * - Hold for CPE_FACTORY_RESET_HOLD_MS: restore factory settings and reboot.
 */
esp_err_t reset_button_start(void);

#ifdef __cplusplus
}
#endif
