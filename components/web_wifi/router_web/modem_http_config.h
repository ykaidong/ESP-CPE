/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Embedded web management server for the ESP-CPE product.
 *
 * The management interface is protected by a cpe_core session cookie
 * (CPE_SID, HttpOnly + SameSite=Strict) and a per-session CSRF token
 * (X-CSRF-Token) instead of the upstream HTTP Basic credentials. All
 * state-changing handlers additionally require a JSON content type and a
 * same-origin (or absent) Origin/Referer header.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the SPIFFS asset mount and the HTTP management server.
 *
 * Safe to call once during start-up. The SoftAP must be started before this
 * call (see app_wifi_start()); the server itself only depends on lwIP and the
 * SPIFFS partition, not on the 4G/ECM link.
 *
 * @return ESP_OK on success, an IDF error code otherwise.
 */
esp_err_t modem_http_start(void);

/**
 * @brief Stop the HTTP server, unregister the Wi-Fi event handlers and free
 *        the station list. The SPIFFS mount is left registered.
 *
 * Reserved for lifecycle operations that are not wired up yet (e.g. stopping
 * the console before an OTA update or a management-plane restart). Kept and
 * documented so the teardown path is maintained, not dead code.
 *
 * @return ESP_OK on success (also when the server was not running).
 */
esp_err_t modem_http_stop(void);

#ifdef __cplusplus
}
#endif
