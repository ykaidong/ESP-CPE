/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public interface of the ESP-CPE management console.
 *
 * The console runs its own netif-dependent services (SPIFFS assets, HTTP
 * server, station registry) and is started by main after the SoftAP is up.
 * Everything else (protocol helpers, JSON layer, API endpoints) is private to
 * the web_console component.
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

/**
 * @brief Erase every station nickname persisted by the management console.
 *
 * Both factory-reset paths (web console and the physical reset button) must
 * call this after cpe_config_factory_reset() succeeded, so they leave the same
 * state behind. Only the NVS entries are removed; the in-RAM station list is
 * left untouched (both callers reboot right afterwards).
 */
void cpe_web_station_names_erase_all(void);

#ifdef __cplusplus
}
#endif
