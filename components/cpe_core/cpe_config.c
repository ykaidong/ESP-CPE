/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

#include "cpe_config.h"
#include "cpe_recovery.h"
#include "cpe_ct.h"

static const char *TAG = "cpe_config";

#define CPE_NVS_NAMESPACE   "cpe_cfg"
#define CPE_CFG_VERSION     1
#define CPE_ADMIN_SALT_LEN  16
#define CPE_ADMIN_HASH_LEN  32
#define CPE_KDF_ITERATIONS  10000
#define CPE_PASSWORD_LEN    12

#define NVS_KEY_VERSION     "ver"
#define NVS_KEY_AP_SSID     "ap_ssid"
#define NVS_KEY_AP_PASS     "ap_pass"
#define NVS_KEY_AP_CHANNEL  "ap_ch"
#define NVS_KEY_AP_BW       "ap_bw"
#define NVS_KEY_AP_HIDDEN   "ap_hid"
#define NVS_KEY_AP_MAX_STA  "ap_max"
#define NVS_KEY_DEV_NAME    "dev_name"
#define NVS_KEY_ADM_USER    "adm_user"
#define NVS_KEY_ADM_SALT    "adm_salt"
#define NVS_KEY_ADM_HASH    "adm_hash"
#define NVS_KEY_ADM_MC      "adm_mc"
#define NVS_KEY_CRED_GEN    "cred_gen"

static const char PASSWORD_ALPHABET[] =
    "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";

typedef struct {
    SemaphoreHandle_t lock;
    nvs_handle_t nvs;
    bool inited;
    bool first_boot;
    cpe_config_t cfg;
    uint8_t adm_salt[CPE_ADMIN_SALT_LEN];
    uint8_t adm_hash[CPE_ADMIN_HASH_LEN];
    bool adm_must_change;
} cpe_config_ctx_t;

static cpe_config_ctx_t s_cfg;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
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

static void random_password(char *buf, size_t len)
{
    const uint32_t alphabet_len = (uint32_t)(sizeof(PASSWORD_ALPHABET) - 1);
    /* 2^32 is not a multiple of alphabet_len: reject the low values so that
     * the modulo below stays unbiased. */
    const uint32_t threshold = (0u - alphabet_len) % alphabet_len;
    for (size_t i = 0; i < len; i++) {
        uint32_t r;
        do {
            r = esp_random();
        } while (r < threshold);
        buf[i] = PASSWORD_ALPHABET[r % alphabet_len];
    }
    buf[len] = '\0';
}

static esp_err_t pbkdf2_sha256(const char *password, const uint8_t *salt,
                               size_t salt_len, uint8_t out[CPE_ADMIN_HASH_LEN])
{
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, CPE_KDF_ITERATIONS);
    }
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len);
    }
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                            (const uint8_t *)password, strlen(password));
    }
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_output_bytes(&op, out, CPE_ADMIN_HASH_LEN);
    }
    psa_key_derivation_abort(&op);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PBKDF2 failed: %d", (int)st);
        memset(out, 0, CPE_ADMIN_HASH_LEN);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t nvs_read_str(nvs_handle_t h, const char *key, char *buf, size_t buf_len)
{
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, key, NULL, &len);
    if (err != ESP_OK) {
        return err;
    }
    if (len == 0 || len > buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return nvs_get_str(h, key, buf, &len);
}

static esp_err_t nvs_write_str(nvs_handle_t h, const char *key, const char *value)
{
    return nvs_set_str(h, key, value);
}

/* ------------------------------------------------------------------------- */
/* Validation                                                                */
/* ------------------------------------------------------------------------- */

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

/* ------------------------------------------------------------------------- */
/* Defaults / persistence                                                    */
/* ------------------------------------------------------------------------- */

static esp_err_t apply_defaults_locked(bool regenerate_credentials)
{
    cpe_config_t *cfg = &s_cfg.cfg;
    memset(cfg, 0, sizeof(*cfg));

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        memset(mac, 0, sizeof(mac));
    }
    snprintf(cfg->ap_ssid, sizeof(cfg->ap_ssid), "%s-%02X%02X",
             CONFIG_CPE_AP_SSID_PREFIX, mac[4], mac[5]);
    random_password(cfg->ap_password, CPE_PASSWORD_LEN);
    cfg->ap_channel = CONFIG_CPE_AP_CHANNEL_DEFAULT;
    cfg->ap_bandwidth = 40;
    cfg->ap_hidden = false;
    cfg->max_sta = CONFIG_CPE_MAX_STA_DEFAULT;
    snprintf(cfg->admin_user, sizeof(cfg->admin_user), "%s", CONFIG_CPE_ADMIN_USERNAME);
    snprintf(cfg->device_name, sizeof(cfg->device_name), "%s", CONFIG_CPE_DEVICE_NAME);

    if (regenerate_credentials) {
        esp_err_t err = pbkdf2_sha256(cfg->ap_password, s_cfg.adm_salt, CPE_ADMIN_SALT_LEN,
                                      s_cfg.adm_hash);
        if (err != ESP_OK) {
            /* Never persist an all-zero hash: the admin account would be
             * permanently unusable. Fail the boot instead. */
            memset(s_cfg.adm_hash, 0, sizeof(s_cfg.adm_hash));
            ESP_LOGE(TAG, "Cannot derive the initial admin password hash");
            return err;
        }
        /* The initial admin password equals the AP password: whoever can join
         * the AP can log in, and is then forced to change it. */
        s_cfg.adm_must_change = true;
    }
    return ESP_OK;
}

static esp_err_t persist_locked(void)
{
    cpe_config_t *cfg = &s_cfg.cfg;
    esp_err_t err = nvs_set_u32(s_cfg.nvs, NVS_KEY_VERSION, CPE_CFG_VERSION);
    if (err == ESP_OK) {
        err = nvs_write_str(s_cfg.nvs, NVS_KEY_AP_SSID, cfg->ap_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_write_str(s_cfg.nvs, NVS_KEY_AP_PASS, cfg->ap_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(s_cfg.nvs, NVS_KEY_AP_CHANNEL, cfg->ap_channel);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(s_cfg.nvs, NVS_KEY_AP_BW, cfg->ap_bandwidth);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(s_cfg.nvs, NVS_KEY_AP_HIDDEN, cfg->ap_hidden ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(s_cfg.nvs, NVS_KEY_AP_MAX_STA, cfg->max_sta);
    }
    if (err == ESP_OK) {
        err = nvs_write_str(s_cfg.nvs, NVS_KEY_DEV_NAME, cfg->device_name);
    }
    if (err == ESP_OK) {
        err = nvs_write_str(s_cfg.nvs, NVS_KEY_ADM_USER, cfg->admin_user);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(s_cfg.nvs, NVS_KEY_ADM_SALT, s_cfg.adm_salt, CPE_ADMIN_SALT_LEN);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(s_cfg.nvs, NVS_KEY_ADM_HASH, s_cfg.adm_hash, CPE_ADMIN_HASH_LEN);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(s_cfg.nvs, NVS_KEY_ADM_MC, s_cfg.adm_must_change ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(s_cfg.nvs, NVS_KEY_CRED_GEN, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_cfg.nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist configuration: %s", esp_err_to_name(err));
    }
    return err;
}

static bool load_locked(bool *found)
{
    cpe_config_t *cfg = &s_cfg.cfg;
    memset(cfg, 0, sizeof(*cfg));

    uint32_t ver = 0;
    if (nvs_get_u32(s_cfg.nvs, NVS_KEY_VERSION, &ver) != ESP_OK || ver != CPE_CFG_VERSION) {
        *found = false;
        return false;
    }

    bool ok = true;
    ok &= (nvs_read_str(s_cfg.nvs, NVS_KEY_AP_SSID, cfg->ap_ssid, sizeof(cfg->ap_ssid)) == ESP_OK);
    ok &= (nvs_read_str(s_cfg.nvs, NVS_KEY_AP_PASS, cfg->ap_password, sizeof(cfg->ap_password)) == ESP_OK);
    uint8_t u8 = 0;
    ok &= (nvs_get_u8(s_cfg.nvs, NVS_KEY_AP_CHANNEL, &u8) == ESP_OK);
    cfg->ap_channel = u8;
    ok &= (nvs_get_u8(s_cfg.nvs, NVS_KEY_AP_BW, &u8) == ESP_OK);
    cfg->ap_bandwidth = u8;
    ok &= (nvs_get_u8(s_cfg.nvs, NVS_KEY_AP_HIDDEN, &u8) == ESP_OK);
    cfg->ap_hidden = u8 != 0;
    ok &= (nvs_get_u8(s_cfg.nvs, NVS_KEY_AP_MAX_STA, &u8) == ESP_OK);
    cfg->max_sta = u8;
    /* Device name is optional; fall back to the default. */
    if (nvs_read_str(s_cfg.nvs, NVS_KEY_DEV_NAME, cfg->device_name, sizeof(cfg->device_name)) != ESP_OK) {
        snprintf(cfg->device_name, sizeof(cfg->device_name), "%s", CONFIG_CPE_DEVICE_NAME);
    }
    ok &= (nvs_read_str(s_cfg.nvs, NVS_KEY_ADM_USER, cfg->admin_user, sizeof(cfg->admin_user)) == ESP_OK);

    size_t blob_len = CPE_ADMIN_SALT_LEN;
    ok &= (nvs_get_blob(s_cfg.nvs, NVS_KEY_ADM_SALT, s_cfg.adm_salt, &blob_len) == ESP_OK &&
           blob_len == CPE_ADMIN_SALT_LEN);
    blob_len = CPE_ADMIN_HASH_LEN;
    ok &= (nvs_get_blob(s_cfg.nvs, NVS_KEY_ADM_HASH, s_cfg.adm_hash, &blob_len) == ESP_OK &&
           blob_len == CPE_ADMIN_HASH_LEN);
    ok &= (nvs_get_u8(s_cfg.nvs, NVS_KEY_ADM_MC, &u8) == ESP_OK);
    s_cfg.adm_must_change = u8 != 0;

    *found = true;

    if (!ok) {
        ESP_LOGE(TAG, "Stored configuration is incomplete, restoring secure defaults");
        return false;
    }

    if (cpe_config_validate_ap_ssid(cfg->ap_ssid) != ESP_OK ||
            cpe_config_validate_wifi_password(cfg->ap_password) != ESP_OK ||
            cpe_config_validate_channel(cfg->ap_channel) != ESP_OK ||
            cpe_config_validate_bandwidth(cfg->ap_bandwidth) != ESP_OK ||
            cpe_config_validate_max_sta(cfg->max_sta) != ESP_OK ||
            cpe_config_validate_device_name(cfg->device_name) != ESP_OK ||
            cfg->admin_user[0] == '\0') {
        ESP_LOGE(TAG, "Stored configuration is invalid (channel=%u, bw=%u, max_sta=%u), restoring secure defaults",
                 cfg->ap_channel, cfg->ap_bandwidth, cfg->max_sta);
        return false;
    }
    return true;
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

    /* Random salt for the admin password must exist before the first hash. */
    esp_fill_random(s_cfg.adm_salt, CPE_ADMIN_SALT_LEN);

    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed");
        return ESP_FAIL;
    }

    esp_err_t err = nvs_open(CPE_NVS_NAMESPACE, NVS_READWRITE, &s_cfg.nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    bool found = false;
    bool valid = load_locked(&found);
    bool generated = false;

    if (!found || !valid) {
        /* If the namespace was found but invalid, module-level defaults keep
         * the read values; regenerate everything to a known-good state. */
        esp_fill_random(s_cfg.adm_salt, CPE_ADMIN_SALT_LEN);
        err = apply_defaults_locked(true);
        if (err != ESP_OK) {
            return err;
        }
        generated = true;
        err = persist_locked();
        if (err != ESP_OK) {
            return err;
        }
    }

    s_cfg.first_boot = generated;
    s_cfg.inited = true;

    ESP_LOGI(TAG, "Configuration loaded: AP SSID \"%s\", channel %u, '%uMHz', admin \"%s\"",
             s_cfg.cfg.ap_ssid, s_cfg.cfg.ap_channel, s_cfg.cfg.ap_bandwidth, s_cfg.cfg.admin_user);
    if (generated) {
        cpe_config_print_credentials("first boot");
    }
    return ESP_OK;
}

esp_err_t cpe_config_get(cpe_config_t *out)
{
    if (!s_cfg.inited || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    *out = s_cfg.cfg;
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
    ESP_LOGW(TAG, "  Web console     : http://192.168.4.1");
    ESP_LOGW(TAG, "==================================================");
}

esp_err_t cpe_config_set_ap(const char *ssid, const char *password,
                            int channel, int bandwidth, bool hidden, int max_sta)
{
    if (!s_cfg.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = cpe_config_validate_ap_ssid(ssid);
    if (err == ESP_OK) {
        err = cpe_config_validate_channel(channel);
    }
    if (err == ESP_OK) {
        err = cpe_config_validate_bandwidth(bandwidth);
    }
    if (err == ESP_OK) {
        err = cpe_config_validate_max_sta(max_sta);
    }
    if (err == ESP_OK && password != NULL) {
        err = cpe_config_validate_wifi_password(password);
    }
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    cpe_config_t backup = s_cfg.cfg;
    snprintf(s_cfg.cfg.ap_ssid, sizeof(s_cfg.cfg.ap_ssid), "%s", ssid);
    if (password != NULL) {
        snprintf(s_cfg.cfg.ap_password, sizeof(s_cfg.cfg.ap_password), "%s", password);
    }
    s_cfg.cfg.ap_channel = (uint8_t)channel;
    s_cfg.cfg.ap_bandwidth = (uint8_t)bandwidth;
    s_cfg.cfg.ap_hidden = hidden;
    s_cfg.cfg.max_sta = (uint8_t)max_sta;
    err = persist_locked();
    if (err != ESP_OK) {
        s_cfg.cfg = backup;
    }
    xSemaphoreGiveRecursive(s_cfg.lock);
    return err;
}

esp_err_t cpe_config_set_device_name(const char *name)
{
    if (!s_cfg.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = cpe_config_validate_device_name(name);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    char backup[sizeof(s_cfg.cfg.device_name)];
    memcpy(backup, s_cfg.cfg.device_name, sizeof(backup));
    snprintf(s_cfg.cfg.device_name, sizeof(s_cfg.cfg.device_name), "%s", name);
    err = persist_locked();
    if (err != ESP_OK) {
        memcpy(s_cfg.cfg.device_name, backup, sizeof(backup));
    }
    xSemaphoreGiveRecursive(s_cfg.lock);
    return err;
}

bool cpe_config_check_admin_password(const char *password)
{
    if (!s_cfg.inited || password == NULL) {
        return false;
    }
    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    uint8_t hash[CPE_ADMIN_HASH_LEN];
    bool ok = false;
    if (pbkdf2_sha256(password, s_cfg.adm_salt, CPE_ADMIN_SALT_LEN, hash) == ESP_OK) {
        ok = cpe_ct_equal(hash, s_cfg.adm_hash, CPE_ADMIN_HASH_LEN);
    }
    memset(hash, 0, sizeof(hash));
    xSemaphoreGiveRecursive(s_cfg.lock);
    return ok;
}

esp_err_t cpe_config_set_admin_password(const char *new_password, bool must_change)
{
    if (!s_cfg.inited || new_password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(new_password) < CPE_WIFI_PASS_MIN_LEN || strlen(new_password) > CPE_WIFI_PASS_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_printable_ascii(new_password, strlen(new_password))) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t salt[CPE_ADMIN_SALT_LEN];
    uint8_t hash[CPE_ADMIN_HASH_LEN];
    esp_fill_random(salt, sizeof(salt));
    esp_err_t err = pbkdf2_sha256(new_password, salt, sizeof(salt), hash);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    uint8_t old_salt[CPE_ADMIN_SALT_LEN];
    uint8_t old_hash[CPE_ADMIN_HASH_LEN];
    bool old_mc = s_cfg.adm_must_change;
    memcpy(old_salt, s_cfg.adm_salt, sizeof(old_salt));
    memcpy(old_hash, s_cfg.adm_hash, sizeof(old_hash));

    memcpy(s_cfg.adm_salt, salt, sizeof(salt));
    memcpy(s_cfg.adm_hash, hash, sizeof(hash));
    s_cfg.adm_must_change = must_change;
    err = persist_locked();
    if (err != ESP_OK) {
        memcpy(s_cfg.adm_salt, old_salt, sizeof(old_salt));
        memcpy(s_cfg.adm_hash, old_hash, sizeof(old_hash));
        s_cfg.adm_must_change = old_mc;
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
    bool mc = s_cfg.adm_must_change;
    xSemaphoreGiveRecursive(s_cfg.lock);
    return mc;
}

bool cpe_config_admin_password_is_ap_password(void)
{
    if (!s_cfg.inited) {
        return false;
    }
    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    uint8_t hash[CPE_ADMIN_HASH_LEN];
    bool match = false;
    if (pbkdf2_sha256(s_cfg.cfg.ap_password, s_cfg.adm_salt, CPE_ADMIN_SALT_LEN,
                      hash) == ESP_OK) {
        match = cpe_ct_equal(hash, s_cfg.adm_hash, sizeof(hash));
    }
    memset(hash, 0, sizeof(hash));
    xSemaphoreGiveRecursive(s_cfg.lock);
    return match;
}

esp_err_t cpe_config_factory_reset(void)
{
    if (!s_cfg.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);

    /* A reset performed from the web UI must not lock the user out of the
     * SoftAP (the new random password would only be visible on the UART).
     * Keep the device-unique AP SSID/password, reset everything else and set
     * the admin password back to the AP password (forced change on login). */
    char keep_ssid[sizeof(s_cfg.cfg.ap_ssid)];
    char keep_password[sizeof(s_cfg.cfg.ap_password)];
    memcpy(keep_ssid, s_cfg.cfg.ap_ssid, sizeof(keep_ssid));
    memcpy(keep_password, s_cfg.cfg.ap_password, sizeof(keep_password));

    esp_err_t err = nvs_erase_all(s_cfg.nvs);
    if (err == ESP_OK) {
        err = nvs_commit(s_cfg.nvs);
    }
    if (err == ESP_OK) {
        esp_fill_random(s_cfg.adm_salt, CPE_ADMIN_SALT_LEN);
        err = apply_defaults_locked(false);
    }
    if (err == ESP_OK) {
        memcpy(s_cfg.cfg.ap_ssid, keep_ssid, sizeof(keep_ssid));
        memcpy(s_cfg.cfg.ap_password, keep_password, sizeof(keep_password));
        /* The admin credential must match the (kept) AP password again. */
        err = pbkdf2_sha256(s_cfg.cfg.ap_password, s_cfg.adm_salt, CPE_ADMIN_SALT_LEN,
                            s_cfg.adm_hash);
        if (err != ESP_OK) {
            /* Fail closed: do not persist an all-zero hash. */
            memset(s_cfg.adm_hash, 0, sizeof(s_cfg.adm_hash));
        }
        s_cfg.adm_must_change = true;
    }
    if (err == ESP_OK) {
        err = persist_locked();
    }
    if (err == ESP_OK) {
        s_cfg.first_boot = true;
    }
    xSemaphoreGiveRecursive(s_cfg.lock);
    if (err == ESP_OK) {
        /* A factory reset also clears the persistent recovery budget, so the
         * device gets a fresh set of bounded recovery attempts. */
        (void)cpe_boot_count_clear();
        /* The credentials are printed on the next boot when the reset button
         * is held down while the application starts (only while the admin
         * password has not been changed). */
        ESP_LOGW(TAG, "Factory reset performed (AP credentials kept, admin password "
                 "reset to the AP password); hold the reset button during the next boot "
                 "to print the credentials");
    }
    return err;
}
