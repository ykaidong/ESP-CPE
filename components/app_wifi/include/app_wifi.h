/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SoftAP management interface for the ESP-CPE product.
 *
 * All parameters (SSID, WPA2 password, channel, bandwidth, hidden flag and
 * maximum station count) come from components/cpe_core (cpe_config.h), never
 * from compile-time credentials. Every API returns an error instead of
 * aborting through ESP_ERROR_CHECK so that a bad configuration can never turn
 * into a boot loop (audit P1-13).
 */

#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include "cpe_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the SoftAP with the configuration stored by cpe_core.
 *
 * The AP is always secured with WPA2-PSK; the password is validated to be
 * 8..63 bytes by cpe_config. The AP netif is kept alive for the whole runtime
 * (the 4G/ECM link is independent, see components/cpe_wan).
 *
 * @return ESP_OK on success, an IDF error code otherwise.
 */
esp_err_t app_wifi_start(void);

/**
 * @brief Apply new AP parameters at runtime.
 *
 * The radio configuration is only changed after every field passed validation
 * (SSID 1..32, password 8..63, channel 1..13, bandwidth 20/40, max_sta 1..10).
 * Channel changes use esp_wifi_set_channel() and bandwidth changes use
 * esp_wifi_set_bandwidth(); on failure the previous configuration is restored,
 * so the caller must only persist the configuration after this call returned
 * ESP_OK.
 *
 * @param cfg  New configuration (must not be NULL).
 * @return ESP_OK on success, an IDF error code otherwise.
 */
esp_err_t app_wifi_apply(const cpe_config_t *cfg);

/**
 * @brief Get the SoftAP netif handle (NULL before app_wifi_start()).
 *
 * Used by components/cpe_wan to enable NAPT and to apply upstream DNS to the
 * AP DHCP server. This component never touches DNS or NAPT itself.
 */
esp_netif_t *app_wifi_get_ap_netif(void);

/**
 * @brief Deauthenticate every station associated with the SoftAP.
 *
 * Call this shortly before esp_restart(). Without an explicit deauth, a quick
 * reboot lets a station (notably Windows) keep its pre-reboot association when
 * the same BSSID reappears within a couple of seconds: it keeps using the old
 * keys, the AP silently drops its frames, and the management console stays
 * unreachable until the user reconnects Wi-Fi manually.
 *
 * Blocks for ~100 ms so the deauth frame(s) can leave the radio. Calling it
 * while Wi-Fi is not started only logs a warning.
 */
void app_wifi_deauth_all_stations(void);

/**
 * @brief Deauthenticate a single station.
 *
 * Implemented here rather than in the console: resolving the AID and calling
 * esp_wifi_deauth_sta() is SoftAP knowledge, and the console must not need the
 * Wi-Fi driver's station table to expose a "disconnect" action.
 *
 * @param mac  Station MAC address (must not be NULL).
 *
 * @return ESP_OK when the station was disconnected, ESP_ERR_NOT_FOUND when it is
 *         not associated any more, an IDF error code otherwise.
 */
esp_err_t app_wifi_sta_disconnect(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
