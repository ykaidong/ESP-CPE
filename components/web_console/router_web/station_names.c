/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Station MAC helpers and nickname persistence (audit P1-16/P1-17).
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cpe_kv.h"
#include "cpe_web.h"
#include "http_internal.h"

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void cpe_web_mac_to_hex(const uint8_t mac[6], char out[13])
{
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x",
             (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
             (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
}

void cpe_web_mac_to_colon(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
             (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
}

/* Strict MAC parser: accepts "aa:bb:cc:dd:ee:ff" or "aabbccddeeff". */
bool cpe_web_mac_parse(const char *str, uint8_t mac[6])
{
    if (str == NULL) {
        return false;
    }
    size_t len = strlen(str);
    if (len == 17) {
        for (int i = 0; i < 6; i++) {
            int hi = hex_nibble(str[i * 3]);
            int lo = hex_nibble(str[i * 3 + 1]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            if (i < 5 && str[i * 3 + 2] != ':') {
                return false;
            }
            mac[i] = (uint8_t)((hi << 4) | lo);
        }
        return true;
    }
    if (len == 12) {
        for (int i = 0; i < 6; i++) {
            int hi = hex_nibble(str[i * 2]);
            int lo = hex_nibble(str[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            mac[i] = (uint8_t)((hi << 4) | lo);
        }
        return true;
    }
    return false;
}

/* Whitelist for the user visible station name (audit P1-16). */
bool cpe_web_station_name_valid(const char *name)
{
    if (name == NULL) {
        return false;
    }
    size_t len = strlen(name);
    if (len == 0 || len > CPE_WEB_STA_NAME_MAX_LEN) {
        return false;
    }
    bool visible = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '.' ||
                c == '@' || c == '(' || c == ')') {
            if (c != ' ') {
                visible = true;
            }
            continue;
        }
        return false;
    }
    return visible;
}

esp_err_t cpe_web_station_name_load(const uint8_t mac[6], char *out, size_t out_len)
{
    char key[13];
    cpe_web_mac_to_hex(mac, key);

    cpe_kv_t kv;
    esp_err_t err = cpe_kv_open(&kv, CPE_KV_NS_STA_NAMES, false);
    if (err != ESP_OK) {
        return err;
    }
    err = cpe_kv_get_str(&kv, key, out, out_len);
    cpe_kv_close(&kv);
    if (err == ESP_OK && !cpe_web_station_name_valid(out)) {
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

esp_err_t cpe_web_station_name_save(const uint8_t mac[6], const char *name)
{
    char key[13];
    cpe_web_mac_to_hex(mac, key);

    cpe_kv_t kv;
    esp_err_t err = cpe_kv_open(&kv, CPE_KV_NS_STA_NAMES, true);
    if (err != ESP_OK) {
        return err;
    }
    err = cpe_kv_set_str(&kv, key, name);
    if (err == ESP_OK) {
        err = cpe_kv_commit(&kv);
    }
    cpe_kv_close(&kv);
    return err;
}

void cpe_web_station_names_erase_all(void)
{
    cpe_kv_t kv;
    if (cpe_kv_open(&kv, CPE_KV_NS_STA_NAMES, true) == ESP_OK) {
        if (cpe_kv_erase_all(&kv) == ESP_OK) {
            (void)cpe_kv_commit(&kv);
        }
        cpe_kv_close(&kv);
    }
}
