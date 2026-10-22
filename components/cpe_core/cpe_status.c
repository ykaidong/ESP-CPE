/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "cpe_status.h"

static const char *TAG = "cpe_status";

static cpe_status_t s_status;
static SemaphoreHandle_t s_lock;
static int64_t s_boot_us;
static bool s_inited;

esp_err_t cpe_status_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "Failed to create status mutex");
        return ESP_ERR_NO_MEM;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = CPE_STATE_BOOT;
    snprintf(s_status.state_text, sizeof(s_status.state_text), "booting");
    s_boot_us = esp_timer_get_time();
    s_inited = true;
    return ESP_OK;
}

void cpe_status_set_state(cpe_state_t state, const char *fmt, ...)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = state;
    if (fmt != NULL) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(s_status.state_text, sizeof(s_status.state_text), fmt, args);
        va_end(args);
    }
    xSemaphoreGive(s_lock);
}

void cpe_status_set_usb_connected(bool connected)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.usb_connected = connected;
    xSemaphoreGive(s_lock);
}

void cpe_status_set_link(bool up)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.link_up = up;
    xSemaphoreGive(s_lock);
}

void cpe_status_set_ip(const char *ip)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ip != NULL && ip[0] != '\0') {
        snprintf(s_status.ip, sizeof(s_status.ip), "%s", ip);
        s_status.got_ip = true;
    } else {
        s_status.ip[0] = '\0';
        s_status.got_ip = false;
    }
    xSemaphoreGive(s_lock);
}

void cpe_status_set_public_ip(const char *ip)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ip != NULL && ip[0] != '\0') {
        snprintf(s_status.public_ip, sizeof(s_status.public_ip), "%s", ip);
        s_status.has_public_ip = true;
    } else {
        s_status.public_ip[0] = '\0';
        s_status.has_public_ip = false;
    }
    xSemaphoreGive(s_lock);
}

void cpe_status_set_rssi(int rssi, bool valid)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.rssi = (int8_t)rssi;
    s_status.rssi_valid = valid;
    xSemaphoreGive(s_lock);
}

void cpe_status_set_modem_info(const char *model, const char *fw, const char *imei)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status.modem_model, sizeof(s_status.modem_model), "%s", model != NULL ? model : "");
    snprintf(s_status.modem_fw, sizeof(s_status.modem_fw), "%s", fw != NULL ? fw : "");
    snprintf(s_status.imei, sizeof(s_status.imei), "%s", imei != NULL ? imei : "");
    xSemaphoreGive(s_lock);
}

void cpe_status_set_carrier(const char *carrier)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status.carrier, sizeof(s_status.carrier), "%s", carrier != NULL ? carrier : "");
    xSemaphoreGive(s_lock);
}

void cpe_status_set_modem_registered(bool registered)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.modem_registered = registered;
    xSemaphoreGive(s_lock);
}

void cpe_status_set_sim(bool inserted, bool valid)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.sim_inserted = inserted;
    s_status.sim_valid = valid;
    xSemaphoreGive(s_lock);
}

void cpe_status_set_phone_number(const char *number)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status.phone_number, sizeof(s_status.phone_number), "%s",
             number != NULL ? number : "");
    xSemaphoreGive(s_lock);
}

void cpe_status_set_ps(bool attached, bool valid)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.ps_attached = attached;
    s_status.ps_valid = valid;
    xSemaphoreGive(s_lock);
}

void cpe_status_set_pdp(bool active, bool valid)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.pdp_active = active;
    s_status.pdp_valid = valid;
    xSemaphoreGive(s_lock);
}

void cpe_status_set_signal_quality(int rsrp, int rsrq, int sinr, bool valid)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.rsrp = (int16_t)rsrp;
    s_status.rsrq = (int8_t)rsrq;
    s_status.sinr = (int8_t)sinr;
    s_status.qcsq_valid = valid;
    xSemaphoreGive(s_lock);
}

void cpe_status_set_modem_temp(int temp_c, bool valid)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.modem_temp = (int8_t)temp_c;
    s_status.modem_temp_valid = valid;
    xSemaphoreGive(s_lock);
}

void cpe_status_set_recovery_attempts(uint32_t attempts)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.recovery_attempts = attempts;
    xSemaphoreGive(s_lock);
}

void cpe_status_set_recovery_stage(cpe_recovery_stage_t stage)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.recovery_stage = stage;
    xSemaphoreGive(s_lock);
}

void cpe_status_set_boot_count(uint32_t boot_count)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.boot_count = boot_count;
    xSemaphoreGive(s_lock);
}

void cpe_status_set_error(const char *fmt, ...)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (fmt != NULL) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(s_status.last_error, sizeof(s_status.last_error), fmt, args);
        va_end(args);
    } else {
        s_status.last_error[0] = '\0';
    }
    xSemaphoreGive(s_lock);
}

void cpe_status_get(cpe_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    out->uptime_s = (uint32_t)((esp_timer_get_time() - s_boot_us) / 1000000LL);
    xSemaphoreGive(s_lock);
}
