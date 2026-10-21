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
 * @brief Release the configuration lock and NVS handle.
 *
 * Only for a failed start-up retry (the caller erases NVS and calls
 * cpe_config_init() again). Must not be called while other tasks use the
 * configuration.
 */
void cpe_config_deinit(void);

/**
 * @brief Get a copy of the current configuration (thread safe).
 */
esp_err_t cpe_config_get(cpe_config_t *out);

/**
 * @brief True when this boot generated fresh credentials.
 *
 * Set only while cpe_config_init() is creating a missing/invalid namespace;
 * it stays true until the next reboot (it is not cleared after printing).
 */
bool cpe_config_first_boot(void);

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
 * @brief Callback used by cpe_config_update() to edit a working copy.
 *
 * Runs with the internal configuration lock held: only modify fields of
 * @p cfg. Do not block, touch NVS or the radio, and do not call another
 * configuration update from inside the callback.
 *
 * @return ESP_OK to commit the copy, or an error to abort the update.
 */
typedef esp_err_t (*cpe_config_mutator_t)(cpe_config_t *cfg, void *ctx);

/**
 * @brief Apply a configuration change as one transaction.
 *
 * The mutator edits a copy of the current configuration under the lock. The
 * result is validated as a whole and only the changed NVS keys are written.
 * Previously written keys are restored if a write fails, and the in-memory
 * configuration is left untouched on any error.
 *
 * @param mutate  Callback that applies the change to the working copy.
 * @param ctx     Opaque pointer passed to the callback.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before init or while a
 *         factory reset is pending reboot, or another IDF error code.
 */
esp_err_t cpe_config_update(cpe_config_mutator_t mutate, void *ctx);

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
 * @brief Erase stored configuration while keeping the device reachable.
 *
 * The device-unique AP SSID and password are preserved (so the user does not
 * lose the SoftAP), every other setting returns to its default, and the admin
 * password is reset to the AP password with the forced-change flag set.
 */
esp_err_t cpe_config_factory_reset(void);

#ifdef __cplusplus
}
#endif
