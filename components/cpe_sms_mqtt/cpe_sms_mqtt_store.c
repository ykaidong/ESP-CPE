/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NVS persistence for the SMS forwarding component: the broker configuration
 * and the duplicate ring.
 */

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "cpe_kv.h"

#include "cpe_sms_mqtt_store.h"

static const char *TAG = "sms_mqtt";

#define K_ENABLED   "en"
#define K_ALLOW_SEND "rs"
#define K_HOST      "host"
#define K_PORT      "port"
#define K_USER      "user"
#define K_PASS      "pass"
#define K_TOPIC_UP  "up"
#define K_TOPIC_DN  "dn"
#define K_RING      "ring"
#define K_CMD       "cmd"
#define K_CA        "ca"

esp_err_t cpe_sms_mqtt_store_load(cpe_sms_mqtt_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cpe_kv_t kv;
    esp_err_t err = cpe_kv_open(&kv, CPE_KV_NS_SMS_MQTT, false);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t flag = 0;
    if (cpe_kv_get_u8(&kv, K_ENABLED, &flag) == ESP_OK) {
        cfg->enabled = flag != 0;
    }
    if (cpe_kv_get_u8(&kv, K_ALLOW_SEND, &flag) == ESP_OK) {
        cfg->allow_remote_send = flag != 0;
    }
    (void)cpe_kv_get_str(&kv, K_HOST, cfg->host, sizeof(cfg->host));
    (void)cpe_kv_get_str(&kv, K_USER, cfg->username, sizeof(cfg->username));
    (void)cpe_kv_get_str(&kv, K_PASS, cfg->password, sizeof(cfg->password));
    (void)cpe_kv_get_str(&kv, K_TOPIC_UP, cfg->topic_up, sizeof(cfg->topic_up));
    (void)cpe_kv_get_str(&kv, K_TOPIC_DN, cfg->topic_down, sizeof(cfg->topic_down));

    uint32_t port = 0;
    if (cpe_kv_get_u32(&kv, K_PORT, &port) == ESP_OK) {
        cfg->port = port <= 65535u ? (uint16_t)port : 0;
    }

    cpe_kv_close(&kv);

    /* A stored block that no longer validates (older firmware, a truncated
     * NVS entry) must not keep the device reconnecting at every boot. */
    const char *reason = NULL;
    if (!cpe_sms_mqtt_config_check(cfg, &reason)) {
        ESP_LOGW(TAG, "Stored configuration is not usable (%s), forwarding off",
                 reason != NULL ? reason : "invalid");
        cfg->enabled = false;
    }
    return ESP_OK;
}

esp_err_t cpe_sms_mqtt_store_save(const cpe_sms_mqtt_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cpe_kv_t kv;
    esp_err_t err = cpe_kv_open(&kv, CPE_KV_NS_SMS_MQTT, true);
    if (err != ESP_OK) {
        return err;
    }
    err = cpe_kv_set_u8(&kv, K_ENABLED, cfg->enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = cpe_kv_set_u8(&kv, K_ALLOW_SEND, cfg->allow_remote_send ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = cpe_kv_set_str(&kv, K_HOST, cfg->host);
    }
    if (err == ESP_OK) {
        err = cpe_kv_set_u32(&kv, K_PORT, cfg->port);
    }
    if (err == ESP_OK) {
        err = cpe_kv_set_str(&kv, K_USER, cfg->username);
    }
    if (err == ESP_OK) {
        err = cpe_kv_set_str(&kv, K_PASS, cfg->password);
    }
    if (err == ESP_OK) {
        err = cpe_kv_set_str(&kv, K_TOPIC_UP, cfg->topic_up);
    }
    if (err == ESP_OK) {
        err = cpe_kv_set_str(&kv, K_TOPIC_DN, cfg->topic_down);
    }
    if (err == ESP_OK) {
        err = cpe_kv_commit(&kv);
    }
    cpe_kv_close(&kv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save the configuration: %s", esp_err_to_name(err));
    }
    return err;
}

static void ring_load_key(const char *key, cpe_sms_mqtt_ring_t *ring)
{
    cpe_sms_mqtt_ring_init(ring);
    cpe_kv_t kv;
    if (cpe_kv_open(&kv, CPE_KV_NS_SMS_MQTT, false) != ESP_OK) {
        return;
    }
    cpe_sms_mqtt_ring_t stored;
    size_t len = sizeof(stored);
    esp_err_t err = cpe_kv_get_blob(&kv, key, &stored, &len);
    cpe_kv_close(&kv);
    if (err != ESP_OK || len != sizeof(stored) ||
        stored.count > CPE_SMS_MQTT_RING_SIZE ||
        stored.next >= CPE_SMS_MQTT_RING_SIZE) {
        return;
    }
    *ring = stored;
}

static esp_err_t ring_save_key(const char *key, const cpe_sms_mqtt_ring_t *ring)
{
    cpe_kv_t kv;
    esp_err_t err = cpe_kv_open(&kv, CPE_KV_NS_SMS_MQTT, true);
    if (err != ESP_OK) {
        return err;
    }
    err = cpe_kv_set_blob(&kv, key, ring, sizeof(*ring));
    if (err == ESP_OK) {
        err = cpe_kv_commit(&kv);
    }
    cpe_kv_close(&kv);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save %s: %s", key, esp_err_to_name(err));
    }
    return err;
}

void cpe_sms_mqtt_store_ring_load(cpe_sms_mqtt_ring_t *ring)
{
    if (ring != NULL) {
        ring_load_key(K_RING, ring);
    }
}

esp_err_t cpe_sms_mqtt_store_ring_save(const cpe_sms_mqtt_ring_t *ring)
{
    if (ring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ring_save_key(K_RING, ring);
}

void cpe_sms_mqtt_store_cmd_load(cpe_sms_mqtt_ring_t *ring)
{
    if (ring != NULL) {
        ring_load_key(K_CMD, ring);
    }
}

esp_err_t cpe_sms_mqtt_store_cmd_save(const cpe_sms_mqtt_ring_t *ring)
{
    if (ring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ring_save_key(K_CMD, ring);
}

char *cpe_sms_mqtt_store_cert_load(void)
{
    cpe_kv_t kv;
    if (cpe_kv_open(&kv, CPE_KV_NS_SMS_MQTT, false) != ESP_OK) {
        return NULL;
    }
    char *pem = malloc(CPE_SMS_MQTT_PEM_MAX + 1);
    if (pem == NULL) {
        cpe_kv_close(&kv);
        return NULL;
    }
    esp_err_t err = cpe_kv_get_str(&kv, K_CA, pem, CPE_SMS_MQTT_PEM_MAX + 1);
    cpe_kv_close(&kv);
    /* A stored document that no longer validates (older firmware, a truncated
     * entry) is treated as "no custom CA", not as a reason to fail start-up. */
    if (err != ESP_OK || !cpe_sms_mqtt_pem_valid(pem)) {
        free(pem);
        return NULL;
    }
    return pem;
}

static esp_err_t cert_store(const char *pem)
{
    cpe_kv_t kv;
    esp_err_t err = cpe_kv_open(&kv, CPE_KV_NS_SMS_MQTT, true);
    if (err != ESP_OK) {
        return err;
    }
    err = cpe_kv_set_str(&kv, K_CA, pem);
    if (err == ESP_OK) {
        err = cpe_kv_commit(&kv);
    }
    cpe_kv_close(&kv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save the CA certificate: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t cpe_sms_mqtt_store_cert_save(const char *pem)
{
    if (pem == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return cert_store(pem);
}

esp_err_t cpe_sms_mqtt_store_cert_clear(void)
{
    /* An empty value reads back as "nothing installed" without a separate
     * erase path; the key disappears with the rest of the namespace. */
    return cert_store("");
}

void cpe_sms_mqtt_store_erase_all(void)
{
    cpe_kv_t kv;
    if (cpe_kv_open(&kv, CPE_KV_NS_SMS_MQTT, true) != ESP_OK) {
        return;
    }
    if (cpe_kv_erase_all(&kv) == ESP_OK) {
        (void)cpe_kv_commit(&kv);
    }
    cpe_kv_close(&kv);
}
