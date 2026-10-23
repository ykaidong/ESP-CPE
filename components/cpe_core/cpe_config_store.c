/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "cpe_config.h"
#include "cpe_config_priv.h"
#include "cpe_random.h"

/* The lock-free storage helpers keep the tag of the configuration module so
 * the UART log prefix is unchanged. */
static const char *TAG = "cpe_config";

#define CPE_CFG_VERSION     1
#define CPE_KDF_ITERATIONS  10000
#define CPE_PASSWORD_LEN    12

#define NVS_KEY_VERSION     "ver"

static const char PASSWORD_ALPHABET[] =
    "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";

/* Describe each persisted key once: the same table drives change detection,
 * reading and writing. The schema version is only written while creating the
 * namespace (first boot / factory reset), so it is handled separately. */
typedef enum {
    CFG_FMT_STR = 0,
    CFG_FMT_U8,
    CFG_FMT_BOOL,
    CFG_FMT_BLOB,
} cfg_fmt_t;

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

typedef struct {
    const char *name;
    cfg_fmt_t fmt;
    size_t offset;        /* within cpe_config_state_t */
    size_t size;          /* STR: buffer capacity, BLOB: blob length */
    bool optional;        /* a missing value falls back to `fallback` */
    const char *fallback; /* STR only, used when optional */
} cfg_key_desc_t;

#define CFG_OFF(field)  (offsetof(cpe_config_state_t, cfg) + offsetof(cpe_config_t, field))
#define CFG_SIZE(field) sizeof(((cpe_config_t *)0)->field)

static const cfg_key_desc_t s_keys[CFG_KEY_COUNT] = {
    [CFG_KEY_AP_SSID] = { "ap_ssid", CFG_FMT_STR, CFG_OFF(ap_ssid), CFG_SIZE(ap_ssid),
                          false, NULL },
    [CFG_KEY_AP_PASS] = { "ap_pass", CFG_FMT_STR, CFG_OFF(ap_password), CFG_SIZE(ap_password),
                          false, NULL },
    [CFG_KEY_AP_CHANNEL] = { "ap_ch", CFG_FMT_U8, CFG_OFF(ap_channel), sizeof(uint8_t),
                             false, NULL },
    [CFG_KEY_AP_BW] = { "ap_bw", CFG_FMT_U8, CFG_OFF(ap_bandwidth), sizeof(uint8_t),
                        false, NULL },
    [CFG_KEY_AP_HIDDEN] = { "ap_hid", CFG_FMT_BOOL, CFG_OFF(ap_hidden), sizeof(bool),
                            false, NULL },
    [CFG_KEY_AP_MAX_STA] = { "ap_max", CFG_FMT_U8, CFG_OFF(max_sta), sizeof(uint8_t),
                             false, NULL },
    [CFG_KEY_DEV_NAME] = { "dev_name", CFG_FMT_STR, CFG_OFF(device_name),
                           CFG_SIZE(device_name), true, CONFIG_CPE_DEVICE_NAME },
    [CFG_KEY_ADM_USER] = { "adm_user", CFG_FMT_STR, CFG_OFF(admin_user), CFG_SIZE(admin_user),
                           false, NULL },
    [CFG_KEY_ADM_SALT] = { "adm_salt", CFG_FMT_BLOB, offsetof(cpe_config_state_t, adm_salt),
                           CPE_ADMIN_SALT_LEN, false, NULL },
    [CFG_KEY_ADM_HASH] = { "adm_hash", CFG_FMT_BLOB, offsetof(cpe_config_state_t, adm_hash),
                           CPE_ADMIN_HASH_LEN, false, NULL },
    [CFG_KEY_ADM_MC] = { "adm_mc", CFG_FMT_BOOL, offsetof(cpe_config_state_t, adm_must_change),
                         sizeof(bool), false, NULL },
};

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

esp_err_t cpe_config_store_pbkdf2(const char *password, const uint8_t *salt,
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

/* ------------------------------------------------------------------------- */
/* Key access                                                                */
/* ------------------------------------------------------------------------- */

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

static bool key_changed(cfg_key_t key, const cpe_config_state_t *a, const cpe_config_state_t *b)
{
    const cfg_key_desc_t *d = &s_keys[key];
    const uint8_t *pa = (const uint8_t *)a + d->offset;
    const uint8_t *pb = (const uint8_t *)b + d->offset;
    switch (d->fmt) {
    case CFG_FMT_STR:
        return strcmp((const char *)pa, (const char *)pb) != 0;
    case CFG_FMT_U8:
        return *(const uint8_t *)pa != *(const uint8_t *)pb;
    case CFG_FMT_BOOL:
        return *(const bool *)pa != *(const bool *)pb;
    case CFG_FMT_BLOB:
        return memcmp(pa, pb, d->size) != 0;
    }
    return false;
}

static esp_err_t write_key(nvs_handle_t nvs, cfg_key_t key, const cpe_config_state_t *st)
{
    const cfg_key_desc_t *d = &s_keys[key];
    const void *value = (const uint8_t *)st + d->offset;
    switch (d->fmt) {
    case CFG_FMT_STR:
        return nvs_set_str(nvs, d->name, (const char *)value);
    case CFG_FMT_U8:
        return nvs_set_u8(nvs, d->name, *(const uint8_t *)value);
    case CFG_FMT_BOOL:
        return nvs_set_u8(nvs, d->name, *(const bool *)value ? 1 : 0);
    case CFG_FMT_BLOB:
        return nvs_set_blob(nvs, d->name, value, d->size);
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t read_key(nvs_handle_t nvs, cfg_key_t key, cpe_config_state_t *st)
{
    const cfg_key_desc_t *d = &s_keys[key];
    void *value = (uint8_t *)st + d->offset;
    esp_err_t err;
    switch (d->fmt) {
    case CFG_FMT_STR:
        err = nvs_read_str(nvs, d->name, (char *)value, d->size);
        if (err != ESP_OK && d->optional && d->fallback != NULL) {
            snprintf((char *)value, d->size, "%s", d->fallback);
        }
        return err;
    case CFG_FMT_U8: {
        uint8_t tmp = 0;
        err = nvs_get_u8(nvs, d->name, &tmp);
        if (err == ESP_OK) {
            *(uint8_t *)value = tmp;
        }
        return err;
    }
    case CFG_FMT_BOOL: {
        uint8_t tmp = 0;
        err = nvs_get_u8(nvs, d->name, &tmp);
        if (err == ESP_OK) {
            *(bool *)value = (tmp != 0);
        }
        return err;
    }
    case CFG_FMT_BLOB: {
        size_t len = d->size;
        err = nvs_get_blob(nvs, d->name, value, &len);
        if (err == ESP_OK && len != d->size) {
            return ESP_ERR_INVALID_SIZE;
        }
        return err;
    }
    }
    return ESP_ERR_INVALID_ARG;
}

/* ------------------------------------------------------------------------- */
/* Defaults / persistence                                                    */
/* ------------------------------------------------------------------------- */

esp_err_t cpe_config_store_apply_defaults(cpe_config_state_t *st,
                                          bool regenerate_credentials)
{
    cpe_config_t *cfg = &st->cfg;
    memset(cfg, 0, sizeof(*cfg));

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        memset(mac, 0, sizeof(mac));
    }
    snprintf(cfg->ap_ssid, sizeof(cfg->ap_ssid), "%s-%02X%02X",
             CONFIG_CPE_AP_SSID_PREFIX, mac[4], mac[5]);
    /* A SoftAP password has to stay typeable, so only CPE_PASSWORD_LEN + 1
     * bytes of this (much larger) field are filled: see cpe_random.h. Shared
     * with the session/CSRF tokens, one alphabet, one truncation rule. */
    cpe_random_string(cfg->ap_password, CPE_PASSWORD_LEN + 1, PASSWORD_ALPHABET);
    cfg->ap_channel = CONFIG_CPE_AP_CHANNEL_DEFAULT;
    /* HT20 by default: 2.4 GHz has no really free 40 MHz block, and the wider
     * channel measurably increases jitter/latency for interactive traffic. */
    cfg->ap_bandwidth = CPE_BANDWIDTH_20;
    cfg->ap_hidden = false;
    cfg->max_sta = CONFIG_CPE_MAX_STA_DEFAULT;
    snprintf(cfg->admin_user, sizeof(cfg->admin_user), "%s", CONFIG_CPE_ADMIN_USERNAME);
    snprintf(cfg->device_name, sizeof(cfg->device_name), "%s", CONFIG_CPE_DEVICE_NAME);

    if (regenerate_credentials) {
        esp_err_t err = cpe_config_store_pbkdf2(cfg->ap_password, st->adm_salt,
                                                CPE_ADMIN_SALT_LEN, st->adm_hash);
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

/* Create (or recreate) the whole namespace: first boot and factory reset. */
esp_err_t cpe_config_store_persist_full(nvs_handle_t nvs, const cpe_config_state_t *st)
{
    esp_err_t err = nvs_set_u32(nvs, NVS_KEY_VERSION, CPE_CFG_VERSION);
    for (int i = 0; err == ESP_OK && i < CFG_KEY_COUNT; i++) {
        err = write_key(nvs, (cfg_key_t)i, st);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist configuration: %s", esp_err_to_name(err));
    }
    return err;
}

/* Write only the keys that differ from `old`. IDF v6.1 NVS is write-through
 * (nvs_commit() is a no-op), so a failure halfway through has already changed
 * flash: put every attempted key back to its old value before returning. */
esp_err_t cpe_config_store_persist_diff(nvs_handle_t nvs,
                                        const cpe_config_state_t *old,
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
        err = write_key(nvs, key, next);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist %s: %s", s_keys[i].name, esp_err_to_name(err));
            break;
        }
    }

    if (err == ESP_OK && attempted_count > 0) {
        err = nvs_commit(nvs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to commit configuration: %s", esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) {
        return ESP_OK;
    }

    while (attempted_count > 0) {
        cfg_key_t key = attempted[--attempted_count];
        esp_err_t rollback_err = write_key(nvs, key, old);
        if (rollback_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to roll back %s: %s",
                     s_keys[key].name, esp_err_to_name(rollback_err));
        }
    }
    esp_err_t rollback_err = nvs_commit(nvs);
    if (rollback_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit configuration rollback: %s",
                 esp_err_to_name(rollback_err));
    }
    return err;
}

bool cpe_config_store_load(nvs_handle_t nvs, cpe_config_state_t *st, bool *found)
{
    cpe_config_t *cfg = &st->cfg;
    memset(st, 0, sizeof(*st));

    uint32_t ver = 0;
    if (nvs_get_u32(nvs, NVS_KEY_VERSION, &ver) != ESP_OK || ver != CPE_CFG_VERSION) {
        *found = false;
        return false;
    }

    bool ok = true;
    for (int i = 0; i < CFG_KEY_COUNT; i++) {
        if (read_key(nvs, (cfg_key_t)i, st) != ESP_OK && !s_keys[i].optional) {
            ok = false;
        }
    }

    *found = true;

    if (!ok) {
        ESP_LOGE(TAG, "Stored configuration is incomplete, restoring secure defaults");
        return false;
    }

    if (cpe_config_validate(cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Stored configuration is invalid (channel=%u, bw=%u, max_sta=%u), restoring secure defaults",
                 cfg->ap_channel, cfg->ap_bandwidth, cfg->max_sta);
        return false;
    }
    return true;
}
