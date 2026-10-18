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

void cpe_status_init(void)
{
    if (s_inited) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = CPE_STATE_BOOT;
    snprintf(s_status.state_text, sizeof(s_status.state_text), "booting");
    s_boot_us = esp_timer_get_time();
    s_inited = (s_lock != NULL);
    if (!s_inited) {
        ESP_LOGE(TAG, "Failed to create status mutex");
    }
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

void cpe_status_set_recovery_attempts(uint32_t attempts)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.recovery_attempts = attempts;
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
