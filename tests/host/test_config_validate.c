/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the validation in components/cpe_core/cpe_config.c. This is the
 * "do not brick the device" line: a value that passes here is written to NVS and
 * applied to the SoftAP, so the boundaries are worth pinning down.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cpe_config.h"
#include "test_harness.h"

/* A configuration that must pass every check. */
static void make_valid(cpe_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->ap_ssid, "CPE-TEST");
    strcpy(cfg->ap_password, "test-password");
    cfg->ap_channel = 6;
    cfg->ap_bandwidth = CPE_BANDWIDTH_20;
    cfg->ap_hidden = false;
    cfg->max_sta = 4;
    strcpy(cfg->admin_user, "admin");
    strcpy(cfg->device_name, "esp-cpe-1");
}

/* String of `len` times 'a', NUL terminated (buffers are 1 larger). */
static void fill(char *dst, size_t dst_size, size_t len)
{
    size_t n = len < dst_size - 1 ? len : dst_size - 1;
    memset(dst, 'a', n);
    dst[n] = '\0';
}

static void test_validate_ssid(void)
{
    HT_CHECK_INT(cpe_config_validate_ap_ssid("CPE"), ESP_OK);
    HT_CHECK_INT(cpe_config_validate_ap_ssid("a"), ESP_OK);

    char buf[CPE_SSID_MAX_LEN + 2];
    fill(buf, sizeof(buf), CPE_SSID_MAX_LEN);
    HT_CHECK_INT(cpe_config_validate_ap_ssid(buf), ESP_OK);
    fill(buf, sizeof(buf), CPE_SSID_MAX_LEN + 1);
    HT_CHECK_INT(cpe_config_validate_ap_ssid(buf), ESP_ERR_INVALID_ARG);

    HT_CHECK_INT(cpe_config_validate_ap_ssid(""), ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_config_validate_ap_ssid(NULL), ESP_ERR_INVALID_ARG);
    /* Printable ASCII only: a tab or a UTF-8 byte would not survive the AP. */
    HT_CHECK_INT(cpe_config_validate_ap_ssid("tab\there"), ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_config_validate_ap_ssid("\xe4\xb8\xad"), ESP_ERR_INVALID_ARG);
}

static void test_validate_wifi_password(void)
{
    char buf[CPE_WIFI_PASS_MAX_LEN + 2];

    fill(buf, sizeof(buf), CPE_WIFI_PASS_MIN_LEN - 1);
    HT_CHECK_INT(cpe_config_validate_wifi_password(buf), ESP_ERR_INVALID_ARG);
    fill(buf, sizeof(buf), CPE_WIFI_PASS_MIN_LEN);
    HT_CHECK_INT(cpe_config_validate_wifi_password(buf), ESP_OK);
    fill(buf, sizeof(buf), CPE_WIFI_PASS_MAX_LEN);
    HT_CHECK_INT(cpe_config_validate_wifi_password(buf), ESP_OK);
    fill(buf, sizeof(buf), CPE_WIFI_PASS_MAX_LEN + 1);
    HT_CHECK_INT(cpe_config_validate_wifi_password(buf), ESP_ERR_INVALID_ARG);

    HT_CHECK_INT(cpe_config_validate_wifi_password(""), ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_config_validate_wifi_password(NULL), ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_config_validate_wifi_password("space not\tallowed"), ESP_ERR_INVALID_ARG);
}

static void test_validate_channel_and_bandwidth(void)
{
    HT_CHECK_INT(cpe_config_validate_channel(CPE_CHANNEL_MIN - 1), ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_config_validate_channel(CPE_CHANNEL_MIN), ESP_OK);
    HT_CHECK_INT(cpe_config_validate_channel(CPE_CHANNEL_MAX), ESP_OK);
    HT_CHECK_INT(cpe_config_validate_channel(CPE_CHANNEL_MAX + 1), ESP_ERR_INVALID_ARG);

    HT_CHECK_INT(cpe_config_validate_bandwidth(CPE_BANDWIDTH_20), ESP_OK);
    HT_CHECK_INT(cpe_config_validate_bandwidth(CPE_BANDWIDTH_40), ESP_OK);
    HT_CHECK_INT(cpe_config_validate_bandwidth(10), ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_config_validate_bandwidth(80), ESP_ERR_INVALID_ARG);

    HT_CHECK_INT(cpe_config_validate_max_sta(CPE_MAX_STA_MIN - 1), ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_config_validate_max_sta(CPE_MAX_STA_MIN), ESP_OK);
    HT_CHECK_INT(cpe_config_validate_max_sta(CPE_MAX_STA_MAX), ESP_OK);
    HT_CHECK_INT(cpe_config_validate_max_sta(CPE_MAX_STA_MAX + 1), ESP_ERR_INVALID_ARG);
}

static void test_validate_device_name(void)
{
    HT_CHECK_INT(cpe_config_validate_device_name("esp-cpe"), ESP_OK);

    char buf[CPE_DEVICE_NAME_MAX_LEN + 2];
    fill(buf, sizeof(buf), CPE_DEVICE_NAME_MAX_LEN);
    HT_CHECK_INT(cpe_config_validate_device_name(buf), ESP_OK);
    fill(buf, sizeof(buf), CPE_DEVICE_NAME_MAX_LEN + 1);
    HT_CHECK_INT(cpe_config_validate_device_name(buf), ESP_ERR_INVALID_ARG);

    HT_CHECK_INT(cpe_config_validate_device_name(""), ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_config_validate_device_name(NULL), ESP_ERR_INVALID_ARG);
}

static void test_validate_ap(void)
{
    cpe_config_t cfg;
    make_valid(&cfg);
    HT_CHECK_INT(cpe_config_validate_ap(&cfg), ESP_OK);

    HT_CHECK_INT(cpe_config_validate_ap(NULL), ESP_ERR_INVALID_ARG);

    /* One broken field at a time; ap_hidden is a choice, not a validation. */
    cpe_config_t broken;
    memcpy(&broken, &cfg, sizeof(broken));
    strcpy(broken.ap_ssid, "");
    HT_CHECK_INT(cpe_config_validate_ap(&broken), ESP_ERR_INVALID_ARG);

    memcpy(&broken, &cfg, sizeof(broken));
    strcpy(broken.ap_password, "short");
    HT_CHECK_INT(cpe_config_validate_ap(&broken), ESP_ERR_INVALID_ARG);

    memcpy(&broken, &cfg, sizeof(broken));
    broken.ap_channel = 0;
    HT_CHECK_INT(cpe_config_validate_ap(&broken), ESP_ERR_INVALID_ARG);

    memcpy(&broken, &cfg, sizeof(broken));
    broken.ap_bandwidth = 80;
    HT_CHECK_INT(cpe_config_validate_ap(&broken), ESP_ERR_INVALID_ARG);

    memcpy(&broken, &cfg, sizeof(broken));
    broken.max_sta = 0;
    HT_CHECK_INT(cpe_config_validate_ap(&broken), ESP_ERR_INVALID_ARG);

    memcpy(&broken, &cfg, sizeof(broken));
    broken.ap_hidden = true;
    HT_CHECK_INT(cpe_config_validate_ap(&broken), ESP_OK);
}

static void test_validate_full(void)
{
    cpe_config_t cfg;
    make_valid(&cfg);
    HT_CHECK_INT(cpe_config_validate(&cfg), ESP_OK);

    /* Everything the AP part covers, plus the device name and the admin user. */
    cpe_config_t broken;
    memcpy(&broken, &cfg, sizeof(broken));
    broken.device_name[0] = '\0';
    HT_CHECK_INT(cpe_config_validate(&broken), ESP_ERR_INVALID_ARG);

    memcpy(&broken, &cfg, sizeof(broken));
    broken.admin_user[0] = '\0';
    HT_CHECK_INT(cpe_config_validate(&broken), ESP_ERR_INVALID_ARG);

    HT_CHECK_INT(cpe_config_validate(NULL), ESP_ERR_INVALID_ARG);
}

void test_config_validate(void)
{
    test_validate_ssid();
    test_validate_wifi_password();
    test_validate_channel_and_bandwidth();
    test_validate_device_name();
    test_validate_ap();
    test_validate_full();
}
