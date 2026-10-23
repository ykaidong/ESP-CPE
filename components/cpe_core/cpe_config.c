/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "cpe_config.h"
#include "cpe_config_priv.h"
#include "cpe_ct.h"
#include "cpe_kv.h"

static const char *TAG = "cpe_config";

typedef struct {
    SemaphoreHandle_t lock;
    nvs_handle_t nvs;
    bool nvs_opened;
    bool inited;
    bool first_boot;
    bool reset_pending_reboot;
    cpe_config_state_t state;
} cpe_config_ctx_t;

static cpe_config_ctx_t s_cfg;

/* ------------------------------------------------------------------------- */
/* Validation                                                                */
/* ------------------------------------------------------------------------- */

static bool is_printable_ascii(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) {
            return false;
        }
    }
    return true;
}

esp_err_t cpe_config_validate_ap_ssid(const char *ssid)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(ssid);
    if (len == 0 || len > CPE_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    return is_printable_ascii(ssid, len) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t cpe_config_validate_wifi_password(const char *password)
{
    if (password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(password);
    if (len < CPE_WIFI_PASS_MIN_LEN || len > CPE_WIFI_PASS_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    return is_printable_ascii(password, len) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t cpe_config_validate_channel(int channel)
{
    return (channel >= CPE_CHANNEL_MIN && channel <= CPE_CHANNEL_MAX) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t cpe_config_validate_bandwidth(int bandwidth)
{
    return (bandwidth == CPE_BANDWIDTH_20 || bandwidth == CPE_BANDWIDTH_40) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t cpe_config_validate_max_sta(int max_sta)
{
    return (max_sta >= CPE_MAX_STA_MIN && max_sta <= CPE_MAX_STA_MAX) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t cpe_config_validate_device_name(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(name);
    if (len == 0 || len > CPE_DEVICE_NAME_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    return is_printable_ascii(name, len) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t validate_cfg(const cpe_config_t *cfg)
{
    esp_err_t err = cpe_config_validate_ap_ssid(cfg->ap_ssid);
    if (err == ESP_OK) {
        err = cpe_config_validate_wifi_password(cfg->ap_password);
    }
    if (err == ESP_OK) {
        err = cpe_config_validate_channel(cfg->ap_channel);
    }
    if (err == ESP_OK) {
        err = cpe_config_validate_bandwidth(cfg->ap_bandwidth);
    }
    if (err == ESP_OK) {
        err = cpe_config_validate_max_sta(cfg->max_sta);
    }
    if (err == ESP_OK) {
        err = cpe_config_validate_device_name(cfg->device_name);
    }
    if (err == ESP_OK && cfg->admin_user[0] == '\0') {
        err = ESP_ERR_INVALID_ARG;
    }
    return err;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t cpe_config_init(void)
{
    if (s_cfg.inited) {
        return ESP_OK;
    }

    s_cfg.lock = xSemaphoreCreateRecursiveMutex();
    if (s_cfg.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed");
        cpe_config_deinit();
        return ESP_FAIL;
    }

    esp_err_t err = nvs_open(CPE_KV_NS_CONFIG, NVS_READWRITE, &s_cfg.nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        cpe_config_deinit();
        return err;
    }
    s_cfg.nvs_opened = true;

    bool found = false;
    cpe_config_state_t state;
    bool valid = cpe_config_store_load(s_cfg.nvs, &state, &found);
    bool generated = false;

    if (!found || !valid) {
        /* If the namespace was found but invalid, regenerate everything to a
         * known-good state instead of keeping the partially read values. */
        memset(&state, 0, sizeof(state));
        esp_fill_random(state.adm_salt, CPE_ADMIN_SALT_LEN);
        err = cpe_config_store_apply_defaults(&state, true);
        if (err == ESP_OK) {
            err = cpe_config_store_persist_full(s_cfg.nvs, &state);
        }
        if (err != ESP_OK) {
            memset(&state, 0, sizeof(state));
            cpe_config_deinit();
            return err;
        }
        generated = true;
    }
    s_cfg.state = state;
    memset(&state, 0, sizeof(state));

    s_cfg.first_boot = generated;
    s_cfg.inited = true;

    ESP_LOGI(TAG, "Configuration loaded: AP SSID \"%s\", channel %u, '%uMHz', admin \"%s\"",
             s_cfg.state.cfg.ap_ssid, s_cfg.state.cfg.ap_channel, s_cfg.state.cfg.ap_bandwidth,
             s_cfg.state.cfg.admin_user);
    if (generated) {
        cpe_config_print_credentials("first boot");
    }
    return ESP_OK;
}

void cpe_config_deinit(void)
{
    if (s_cfg.nvs_opened) {
        nvs_close(s_cfg.nvs);
        s_cfg.nvs_opened = false;
    }
    if (s_cfg.lock != NULL) {
        vSemaphoreDelete(s_cfg.lock);
        s_cfg.lock = NULL;
    }
    memset(&s_cfg.state, 0, sizeof(s_cfg.state));
    s_cfg.inited = false;
    s_cfg.first_boot = false;
    s_cfg.reset_pending_reboot = false;
}

esp_err_t cpe_config_get(cpe_config_t *out)
{
    if (!s_cfg.inited || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    *out = s_cfg.state.cfg;
    xSemaphoreGiveRecursive(s_cfg.lock);
    return ESP_OK;
}

bool cpe_config_first_boot(void)
{
    return s_cfg.first_boot;
}

void cpe_config_print_credentials(const char *reason)
{
    if (!s_cfg.inited) {
        return;
    }
    cpe_config_t cfg;
    cpe_config_get(&cfg);

    /* The AP password is always printable (it is stored in NVS). The admin
     * password is only known while it still equals the AP password; after a
     * change only its PBKDF2 hash exists, so say so instead of printing a
     * misleading value. */
    bool admin_is_ap = cpe_config_admin_password_is_ap_password();

    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, "  Credentials (%s)", reason != NULL ? reason : "requested");
    ESP_LOGW(TAG, "  SoftAP SSID     : %s", cfg.ap_ssid);
    ESP_LOGW(TAG, "  SoftAP password : %s", cfg.ap_password);
    ESP_LOGW(TAG, "  Admin user      : %s", cfg.admin_user);
    if (admin_is_ap) {
        ESP_LOGW(TAG, "  Admin password  : %s", cfg.ap_password);
    } else {
        ESP_LOGW(TAG, "  Admin password  : (changed, plaintext not stored; "
                 "hold the button 10 s to factory reset)");
    }
    ESP_LOGW(TAG, "  Web console     : http://%s", CONFIG_SERVER_IP);
    ESP_LOGW(TAG, "==================================================");
}

esp_err_t cpe_config_update(cpe_config_mutator_t mutate, void *ctx)
{
    if (!s_cfg.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mutate == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_cfg.reset_pending_reboot) {
        cpe_config_state_t next = s_cfg.state;
        err = mutate(&next.cfg, ctx);
        if (err == ESP_OK) {
            err = validate_cfg(&next.cfg);
        }
        if (err == ESP_OK) {
            err = cpe_config_store_persist_diff(s_cfg.nvs, &s_cfg.state, &next);
            if (err == ESP_OK) {
                s_cfg.state = next;
            }
        }
        memset(&next, 0, sizeof(next));
    }
    xSemaphoreGiveRecursive(s_cfg.lock);
    return err;
}

/* Derive the PBKDF2 hash of `password` with `salt` and compare it against
 * `expected` in constant time. The caller copies salt/expected out of the
 * configuration under the lock first (PBKDF2 is long running). */
static bool pbkdf2_matches(const char *password,
                           const uint8_t salt[CPE_ADMIN_SALT_LEN],
                           const uint8_t expected[CPE_ADMIN_HASH_LEN])
{
    uint8_t hash[CPE_ADMIN_HASH_LEN];
    bool match = false;
    if (cpe_config_store_pbkdf2(password, salt, CPE_ADMIN_SALT_LEN, hash) == ESP_OK) {
        match = cpe_ct_equal(hash, expected, sizeof(hash));
    }
    memset(hash, 0, sizeof(hash));
    return match;
}

bool cpe_config_check_admin_password(const char *password)
{
    if (!s_cfg.inited || password == NULL) {
        return false;
    }

    /* PBKDF2 is long running: copy the material under the lock and derive the
     * hash outside it, so a login attempt cannot stall configuration users. */
    uint8_t salt[CPE_ADMIN_SALT_LEN];
    uint8_t expected[CPE_ADMIN_HASH_LEN];
    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    memcpy(salt, s_cfg.state.adm_salt, sizeof(salt));
    memcpy(expected, s_cfg.state.adm_hash, sizeof(expected));
    xSemaphoreGiveRecursive(s_cfg.lock);

    bool ok = pbkdf2_matches(password, salt, expected);
    memset(salt, 0, sizeof(salt));
    memset(expected, 0, sizeof(expected));
    return ok;
}

esp_err_t cpe_config_set_admin_password(const char *new_password, bool must_change)
{
    if (!s_cfg.inited || new_password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cpe_config_validate_wifi_password(new_password) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t salt[CPE_ADMIN_SALT_LEN];
    uint8_t hash[CPE_ADMIN_HASH_LEN];
    esp_fill_random(salt, sizeof(salt));
    esp_err_t err = cpe_config_store_pbkdf2(new_password, salt, sizeof(salt), hash);
    if (err != ESP_OK) {
        memset(salt, 0, sizeof(salt));
        return err;
    }

    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    err = ESP_ERR_INVALID_STATE;
    if (!s_cfg.reset_pending_reboot) {
        cpe_config_state_t next = s_cfg.state;
        memcpy(next.adm_salt, salt, sizeof(salt));
        memcpy(next.adm_hash, hash, sizeof(hash));
        next.adm_must_change = must_change;
        err = cpe_config_store_persist_diff(s_cfg.nvs, &s_cfg.state, &next);
        if (err == ESP_OK) {
            s_cfg.state = next;
        }
        memset(&next, 0, sizeof(next));
    }
    xSemaphoreGiveRecursive(s_cfg.lock);
    memset(salt, 0, sizeof(salt));
    memset(hash, 0, sizeof(hash));
    return err;
}

bool cpe_config_admin_must_change(void)
{
    if (!s_cfg.inited) {
        return true;
    }
    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    bool mc = s_cfg.state.adm_must_change;
    xSemaphoreGiveRecursive(s_cfg.lock);
    return mc;
}

bool cpe_config_admin_password_is_ap_password(void)
{
    if (!s_cfg.inited) {
        return false;
    }

    char ap_password[sizeof(s_cfg.state.cfg.ap_password)];
    uint8_t salt[CPE_ADMIN_SALT_LEN];
    uint8_t expected[CPE_ADMIN_HASH_LEN];
    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    memcpy(ap_password, s_cfg.state.cfg.ap_password, sizeof(ap_password));
    memcpy(salt, s_cfg.state.adm_salt, sizeof(salt));
    memcpy(expected, s_cfg.state.adm_hash, sizeof(expected));
    xSemaphoreGiveRecursive(s_cfg.lock);

    bool match = pbkdf2_matches(ap_password, salt, expected);
    memset(ap_password, 0, sizeof(ap_password));
    memset(salt, 0, sizeof(salt));
    memset(expected, 0, sizeof(expected));
    return match;
}

esp_err_t cpe_config_factory_reset(void)
{
    if (!s_cfg.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A reset performed from the web UI must not lock the user out of the
     * SoftAP (the new random password would only be visible on the UART).
     * Keep the device-unique AP SSID/password, reset everything else and set
     * the admin password back to the AP password (forced change on login). */
    char keep_ssid[sizeof(s_cfg.state.cfg.ap_ssid)];
    char keep_password[sizeof(s_cfg.state.cfg.ap_password)];
    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    memcpy(keep_ssid, s_cfg.state.cfg.ap_ssid, sizeof(keep_ssid));
    memcpy(keep_password, s_cfg.state.cfg.ap_password, sizeof(keep_password));
    xSemaphoreGiveRecursive(s_cfg.lock);

    /* PBKDF2 is long running: derive the new admin hash before taking the
     * lock, so a factory reset cannot stall configuration readers for the
     * whole derivation (audit D6). The salt is random and does not depend on
     * the state, and the hash is derived from the copied AP password which is
     * also the one written back below, so the two always agree. */
    uint8_t salt[CPE_ADMIN_SALT_LEN];
    uint8_t hash[CPE_ADMIN_HASH_LEN];
    esp_fill_random(salt, sizeof(salt));
    esp_err_t derive_err = cpe_config_store_pbkdf2(keep_password, salt,
                                                   sizeof(salt), hash);
    if (derive_err != ESP_OK) {
        memset(hash, 0, sizeof(hash));
    }

    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);

    cpe_config_state_t next;
    memset(&next, 0, sizeof(next));
    esp_err_t err = nvs_erase_all(s_cfg.nvs);
    if (err == ESP_OK) {
        err = nvs_commit(s_cfg.nvs);
    }
    bool erased = (err == ESP_OK);
    if (erased) {
        err = cpe_config_store_apply_defaults(&next, false);
    }
    if (erased && err == ESP_OK) {
        memcpy(next.cfg.ap_ssid, keep_ssid, sizeof(keep_ssid));
        memcpy(next.cfg.ap_password, keep_password, sizeof(keep_password));
        /* The admin credential must match the (kept) AP password again. */
        memcpy(next.adm_salt, salt, sizeof(salt));
        if (derive_err == ESP_OK) {
            memcpy(next.adm_hash, hash, sizeof(hash));
        } else {
            /* Fail closed: do not persist an all-zero hash. */
            memset(next.adm_hash, 0, sizeof(next.adm_hash));
            err = derive_err;
        }
        next.adm_must_change = true;
    }
    if (erased && err == ESP_OK) {
        err = cpe_config_store_persist_full(s_cfg.nvs, &next);
    }
    if (erased) {
        /* The namespace is gone: keep the regenerated state even if rewriting
         * it failed, so RAM matches what the next boot will fall back to.
         * Erasing is also the point of no return: the device must reboot into
         * the regenerated defaults, so refuse later configuration writes even
         * when rewriting the defaults failed. */
        s_cfg.state = next;
        s_cfg.reset_pending_reboot = true;
        s_cfg.first_boot = true;
    }
    memset(&next, 0, sizeof(next));
    xSemaphoreGiveRecursive(s_cfg.lock);
    memset(keep_password, 0, sizeof(keep_password));
    memset(salt, 0, sizeof(salt));
    memset(hash, 0, sizeof(hash));
    if (err == ESP_OK) {
        /* The persistent recovery budget is cleared by
         * cpe_system_factory_reset(): keeping it out of the configuration layer
         * avoids cpe_core depending on the recovery section (audit A7). */
        /* The credentials are printed on the next boot when the reset button
         * is held down while the application starts (only while the admin
         * password has not been changed). */
        ESP_LOGW(TAG, "Factory reset performed (AP credentials kept, admin password "
                 "reset to the AP password); hold the reset button during the next boot "
                 "to print the credentials");
    }
    return err;
}
