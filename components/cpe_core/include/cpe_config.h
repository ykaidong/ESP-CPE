/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CPE_SSID_MAX_LEN            32
#define CPE_WIFI_PASS_MIN_LEN       8
#define CPE_WIFI_PASS_MAX_LEN       63
#define CPE_ADMIN_USER_MAX_LEN      32
#define CPE_DEVICE_NAME_MAX_LEN     32
#define CPE_CHANNEL_MIN             1
#define CPE_CHANNEL_MAX             13
#define CPE_BANDWIDTH_20            20
#define CPE_BANDWIDTH_40            40
#define CPE_MAX_STA_MIN             1
#define CPE_MAX_STA_MAX             10

/**
 * @brief Runtime CPE configuration (all values are validated before use).
 */
typedef struct {
    char ap_ssid[CPE_SSID_MAX_LEN + 1];        /*!< SoftAP SSID (1..32 chars) */
    char ap_password[CPE_WIFI_PASS_MAX_LEN + 1]; /*!< SoftAP WPA2 password (8..63 chars) */
    uint8_t ap_channel;                        /*!< 1..13 */
    uint8_t ap_bandwidth;                      /*!< 20 or 40 */
    bool ap_hidden;                            /*!< Hide SSID */
    uint8_t max_sta;                           /*!< 1..10 */
    char admin_user[CPE_ADMIN_USER_MAX_LEN + 1];
    char device_name[CPE_DEVICE_NAME_MAX_LEN + 1];
} cpe_config_t;

/**
 * @brief Initialize configuration storage.
 *
 * Loads the configuration from NVS, validates it and falls back to secure
 * defaults if it is missing or invalid. On first boot (and after a factory
 * reset) random per-device credentials are generated and printed to the
 * console exactly once.
 */
esp_err_t cpe_config_init(void);

/**
 * @brief Get a copy of the current configuration (thread safe).
 */
esp_err_t cpe_config_get(cpe_config_t *out);

/**
 * @brief True until the first configuration has been persisted/printed.
 */
bool cpe_config_first_boot(void);

/**
 * @brief Print AP SSID and initial admin password to the console.
 *
 * Call only when the credentials have just been generated (first boot or
 * factory reset); credentials are never printed on normal boots.
 */
/**
 * @brief Print the current credentials to the UART log (never to an HTTP response).
 *
 * @param reason  Short description of why they are printed, e.g.
 *                "first boot" or "reset button request". May be NULL.
 */
void cpe_config_print_credentials(const char *reason);

/* Validation helpers, all return ESP_OK or ESP_ERR_INVALID_ARG. */
esp_err_t cpe_config_validate_ap_ssid(const char *ssid);
esp_err_t cpe_config_validate_wifi_password(const char *password);
esp_err_t cpe_config_validate_channel(int channel);
esp_err_t cpe_config_validate_bandwidth(int bandwidth);
esp_err_t cpe_config_validate_max_sta(int max_sta);
esp_err_t cpe_config_validate_device_name(const char *name);

/**
 * @brief Update SoftAP settings (persisted only if all values are valid).
 *
 * @param password  New password, or NULL to keep the current one.
 */
esp_err_t cpe_config_set_ap(const char *ssid, const char *password,
                            int channel, int bandwidth, bool hidden, int max_sta);

esp_err_t cpe_config_set_device_name(const char *name);

/**
 * @brief Verify the admin password against the stored PBKDF2 hash (constant time).
 */
bool cpe_config_check_admin_password(const char *password);

/**
 * @brief Set a new admin password (hashed with per-device random salt).
 *
 * @param must_change  Mark password as "must change on next login".
 */
esp_err_t cpe_config_set_admin_password(const char *new_password, bool must_change);

bool cpe_config_admin_must_change(void);

/**
 * @brief True when the stored admin hash still matches the current AP password.
 *
 * Used by the boot-time reset-button helper: the plaintext admin password can
 * only be printed while it is still the AP password (the device never stores
 * the plaintext admin password).
 */
bool cpe_config_admin_password_is_ap_password(void);

/**
 * @brief Erase stored configuration and regenerate random per-device credentials.
 */
esp_err_t cpe_config_factory_reset(void);

#ifdef __cplusplus
}
#endif
