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

static const char PASSWORD_ALPHABET[] =
    "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";

/* The whole persisted state in one snapshot: a transaction builds the next
 * state locally and only publishes it after every NVS write succeeded. */
typedef struct {
    cpe_config_t cfg;
    uint8_t adm_salt[CPE_ADMIN_SALT_LEN];
    uint8_t adm_hash[CPE_ADMIN_HASH_LEN];
    bool adm_must_change;
} cpe_config_state_t;

typedef struct {
    SemaphoreHandle_t lock;
    nvs_handle_t nvs;
    bool inited;
    bool first_boot;
    bool reset_pending_reboot;
    cpe_config_state_t state;
} cpe_config_ctx_t;

static cpe_config_ctx_t s_cfg;

/* Keys written by a transaction. The schema version is only written while
 * creating the namespace (first boot / factory reset), so it is not listed. */
typedef enum {
    CFG_KEY_AP_SSID = 0,
    CFG_KEY_AP_PASS,
    CFG_KEY_AP_CHANNEL,
    CFG_KEY_AP_BW,
    CFG_KEY_AP_HIDDEN,
    CFG_KEY_AP_MAX_STA,
    CFG_KEY_DEV_NAME,
    CFG_KEY_ADM_USER,
    CFG_KEY_ADM_SALT,
    CFG_KEY_ADM_HASH,
    CFG_KEY_ADM_MC,
    CFG_KEY_COUNT,
} cfg_key_t;

static const char *const s_key_names[CFG_KEY_COUNT] = {
    NVS_KEY_AP_SSID,
    NVS_KEY_AP_PASS,
    NVS_KEY_AP_CHANNEL,
    NVS_KEY_AP_BW,
    NVS_KEY_AP_HIDDEN,
    NVS_KEY_AP_MAX_STA,
    NVS_KEY_DEV_NAME,
    NVS_KEY_ADM_USER,
    NVS_KEY_ADM_SALT,
    NVS_KEY_ADM_HASH,
    NVS_KEY_ADM_MC,
};

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

static esp_err_t apply_defaults(cpe_config_state_t *st, bool regenerate_credentials)
{
    cpe_config_t *cfg = &st->cfg;
    memset(cfg, 0, sizeof(*cfg));

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        memset(mac, 0, sizeof(mac));
    }
    snprintf(cfg->ap_ssid, sizeof(cfg->ap_ssid), "%s-%02X%02X",
             CONFIG_CPE_AP_SSID_PREFIX, mac[4], mac[5]);
    random_password(cfg->ap_password, CPE_PASSWORD_LEN);
    cfg->ap_channel = CONFIG_CPE_AP_CHANNEL_DEFAULT;
    /* HT20 by default: 2.4 GHz has no really free 40 MHz block, and the wider
     * channel measurably increases jitter/latency for interactive traffic. */
    cfg->ap_bandwidth = CPE_BANDWIDTH_20;
    cfg->ap_hidden = false;
    cfg->max_sta = CONFIG_CPE_MAX_STA_DEFAULT;
    snprintf(cfg->admin_user, sizeof(cfg->admin_user), "%s", CONFIG_CPE_ADMIN_USERNAME);
    snprintf(cfg->device_name, sizeof(cfg->device_name), "%s", CONFIG_CPE_DEVICE_NAME);

    if (regenerate_credentials) {
        esp_err_t err = pbkdf2_sha256(cfg->ap_password, st->adm_salt, CPE_ADMIN_SALT_LEN,
                                      st->adm_hash);
        if (err != ESP_OK) {
            /* Never persist an all-zero hash: the admin account would be
             * permanently unusable. Fail the boot instead. */
            memset(st->adm_hash, 0, sizeof(st->adm_hash));
            ESP_LOGE(TAG, "Cannot derive the initial admin password hash");
            return err;
        }
        /* The initial admin password equals the AP password: whoever can join
         * the AP can log in, and is then forced to change it. */
        st->adm_must_change = true;
    }
    return ESP_OK;
}

static bool key_changed(cfg_key_t key, const cpe_config_state_t *a,
                        const cpe_config_state_t *b)
{
    switch (key) {
    case CFG_KEY_AP_SSID:
        return strcmp(a->cfg.ap_ssid, b->cfg.ap_ssid) != 0;
    case CFG_KEY_AP_PASS:
        return strcmp(a->cfg.ap_password, b->cfg.ap_password) != 0;
    case CFG_KEY_AP_CHANNEL:
        return a->cfg.ap_channel != b->cfg.ap_channel;
    case CFG_KEY_AP_BW:
        return a->cfg.ap_bandwidth != b->cfg.ap_bandwidth;
    case CFG_KEY_AP_HIDDEN:
        return a->cfg.ap_hidden != b->cfg.ap_hidden;
    case CFG_KEY_AP_MAX_STA:
        return a->cfg.max_sta != b->cfg.max_sta;
    case CFG_KEY_DEV_NAME:
        return strcmp(a->cfg.device_name, b->cfg.device_name) != 0;
    case CFG_KEY_ADM_USER:
        return strcmp(a->cfg.admin_user, b->cfg.admin_user) != 0;
    case CFG_KEY_ADM_SALT:
        return memcmp(a->adm_salt, b->adm_salt, sizeof(a->adm_salt)) != 0;
    case CFG_KEY_ADM_HASH:
        return memcmp(a->adm_hash, b->adm_hash, sizeof(a->adm_hash)) != 0;
    case CFG_KEY_ADM_MC:
        return a->adm_must_change != b->adm_must_change;
    default:
        return false;
    }
}

static esp_err_t write_key_locked(cfg_key_t key, const cpe_config_state_t *st)
{
    switch (key) {
    case CFG_KEY_AP_SSID:
        return nvs_write_str(s_cfg.nvs, NVS_KEY_AP_SSID, st->cfg.ap_ssid);
    case CFG_KEY_AP_PASS:
        return nvs_write_str(s_cfg.nvs, NVS_KEY_AP_PASS, st->cfg.ap_password);
    case CFG_KEY_AP_CHANNEL:
        return nvs_set_u8(s_cfg.nvs, NVS_KEY_AP_CHANNEL, st->cfg.ap_channel);
    case CFG_KEY_AP_BW:
        return nvs_set_u8(s_cfg.nvs, NVS_KEY_AP_BW, st->cfg.ap_bandwidth);
    case CFG_KEY_AP_HIDDEN:
        return nvs_set_u8(s_cfg.nvs, NVS_KEY_AP_HIDDEN, st->cfg.ap_hidden ? 1 : 0);
    case CFG_KEY_AP_MAX_STA:
        return nvs_set_u8(s_cfg.nvs, NVS_KEY_AP_MAX_STA, st->cfg.max_sta);
    case CFG_KEY_DEV_NAME:
        return nvs_write_str(s_cfg.nvs, NVS_KEY_DEV_NAME, st->cfg.device_name);
    case CFG_KEY_ADM_USER:
        return nvs_write_str(s_cfg.nvs, NVS_KEY_ADM_USER, st->cfg.admin_user);
    case CFG_KEY_ADM_SALT:
        return nvs_set_blob(s_cfg.nvs, NVS_KEY_ADM_SALT, st->adm_salt, sizeof(st->adm_salt));
    case CFG_KEY_ADM_HASH:
        return nvs_set_blob(s_cfg.nvs, NVS_KEY_ADM_HASH, st->adm_hash, sizeof(st->adm_hash));
    case CFG_KEY_ADM_MC:
        return nvs_set_u8(s_cfg.nvs, NVS_KEY_ADM_MC, st->adm_must_change ? 1 : 0);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

/* Create (or recreate) the whole namespace: first boot and factory reset. */
static esp_err_t persist_full_locked(const cpe_config_state_t *st)
{
    esp_err_t err = nvs_set_u32(s_cfg.nvs, NVS_KEY_VERSION, CPE_CFG_VERSION);
    for (int i = 0; err == ESP_OK && i < CFG_KEY_COUNT; i++) {
        err = write_key_locked((cfg_key_t)i, st);
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_cfg.nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist configuration: %s", esp_err_to_name(err));
    }
    return err;
}

/* Write only the keys that differ from `old`. IDF v6.1 NVS is write-through
 * (nvs_commit() is a no-op), so a failure halfway through has already changed
 * flash: put every attempted key back to its old value before returning. */
static esp_err_t persist_diff_locked(const cpe_config_state_t *old,
                                     const cpe_config_state_t *next)
{
    cfg_key_t attempted[CFG_KEY_COUNT];
    size_t attempted_count = 0;
    esp_err_t err = ESP_OK;

    for (int i = 0; i < CFG_KEY_COUNT; i++) {
        cfg_key_t key = (cfg_key_t)i;
        if (!key_changed(key, old, next)) {
            continue;
        }
        attempted[attempted_count++] = key;
        err = write_key_locked(key, next);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist %s: %s", s_key_names[key], esp_err_to_name(err));
            break;
        }
    }

    if (err == ESP_OK && attempted_count > 0) {
        err = nvs_commit(s_cfg.nvs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to commit configuration: %s", esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) {
        return ESP_OK;
    }

    while (attempted_count > 0) {
        cfg_key_t key = attempted[--attempted_count];
        esp_err_t rollback_err = write_key_locked(key, old);
        if (rollback_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to roll back %s: %s",
                     s_key_names[key], esp_err_to_name(rollback_err));
        }
    }
    esp_err_t rollback_err = nvs_commit(s_cfg.nvs);
    if (rollback_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit configuration rollback: %s",
                 esp_err_to_name(rollback_err));
    }
    return err;
}

static bool load_locked(cpe_config_state_t *st, bool *found)
{
    cpe_config_t *cfg = &st->cfg;
    memset(st, 0, sizeof(*st));

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
    ok &= (nvs_get_blob(s_cfg.nvs, NVS_KEY_ADM_SALT, st->adm_salt, &blob_len) == ESP_OK &&
           blob_len == CPE_ADMIN_SALT_LEN);
    blob_len = CPE_ADMIN_HASH_LEN;
    ok &= (nvs_get_blob(s_cfg.nvs, NVS_KEY_ADM_HASH, st->adm_hash, &blob_len) == ESP_OK &&
           blob_len == CPE_ADMIN_HASH_LEN);
    ok &= (nvs_get_u8(s_cfg.nvs, NVS_KEY_ADM_MC, &u8) == ESP_OK);
    st->adm_must_change = u8 != 0;

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
    cpe_config_state_t state;
    bool valid = load_locked(&state, &found);
    bool generated = false;

    if (!found || !valid) {
        /* If the namespace was found but invalid, regenerate everything to a
         * known-good state instead of keeping the partially read values. */
        memset(&state, 0, sizeof(state));
        esp_fill_random(state.adm_salt, CPE_ADMIN_SALT_LEN);
        err = apply_defaults(&state, true);
        if (err == ESP_OK) {
            err = persist_full_locked(&state);
        }
        if (err != ESP_OK) {
            memset(&state, 0, sizeof(state));
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
    ESP_LOGW(TAG, "  Web console     : http://192.168.4.1");
    ESP_LOGW(TAG, "==================================================");
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
            err = persist_diff_locked(&s_cfg.state, &next);
            if (err == ESP_OK) {
                s_cfg.state = next;
            }
        }
        memset(&next, 0, sizeof(next));
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
    if (pbkdf2_sha256(password, s_cfg.state.adm_salt, CPE_ADMIN_SALT_LEN, hash) == ESP_OK) {
        ok = cpe_ct_equal(hash, s_cfg.state.adm_hash, CPE_ADMIN_HASH_LEN);
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
    err = ESP_ERR_INVALID_STATE;
    if (!s_cfg.reset_pending_reboot) {
        cpe_config_state_t next = s_cfg.state;
        memcpy(next.adm_salt, salt, sizeof(salt));
        memcpy(next.adm_hash, hash, sizeof(hash));
        next.adm_must_change = must_change;
        err = persist_diff_locked(&s_cfg.state, &next);
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
    xSemaphoreTakeRecursive(s_cfg.lock, portMAX_DELAY);
    uint8_t hash[CPE_ADMIN_HASH_LEN];
    bool match = false;
    if (pbkdf2_sha256(s_cfg.state.cfg.ap_password, s_cfg.state.adm_salt, CPE_ADMIN_SALT_LEN,
                      hash) == ESP_OK) {
        match = cpe_ct_equal(hash, s_cfg.state.adm_hash, sizeof(hash));
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
    char keep_ssid[sizeof(s_cfg.state.cfg.ap_ssid)];
    char keep_password[sizeof(s_cfg.state.cfg.ap_password)];
    memcpy(keep_ssid, s_cfg.state.cfg.ap_ssid, sizeof(keep_ssid));
    memcpy(keep_password, s_cfg.state.cfg.ap_password, sizeof(keep_password));

    cpe_config_state_t next;
    memset(&next, 0, sizeof(next));
    esp_err_t err = nvs_erase_all(s_cfg.nvs);
    if (err == ESP_OK) {
        err = nvs_commit(s_cfg.nvs);
    }
    bool erased = (err == ESP_OK);
    if (erased) {
        esp_fill_random(next.adm_salt, CPE_ADMIN_SALT_LEN);
        err = apply_defaults(&next, false);
    }
    if (erased && err == ESP_OK) {
        memcpy(next.cfg.ap_ssid, keep_ssid, sizeof(keep_ssid));
        memcpy(next.cfg.ap_password, keep_password, sizeof(keep_password));
        /* The admin credential must match the (kept) AP password again. */
        err = pbkdf2_sha256(next.cfg.ap_password, next.adm_salt, CPE_ADMIN_SALT_LEN,
                            next.adm_hash);
        if (err != ESP_OK) {
            /* Fail closed: do not persist an all-zero hash. */
            memset(next.adm_hash, 0, sizeof(next.adm_hash));
        }
        next.adm_must_change = true;
    }
    if (erased && err == ESP_OK) {
        err = persist_full_locked(&next);
    }
    if (erased) {
        /* The namespace is gone: keep the regenerated state even if rewriting
         * it failed, so RAM matches what the next boot will fall back to. */
        s_cfg.state = next;
    }
    memset(&next, 0, sizeof(next));
    if (err == ESP_OK) {
        s_cfg.first_boot = true;
        s_cfg.reset_pending_reboot = true;
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
