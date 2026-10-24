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
/* Plain mutex: every section copies a few scalars and never calls another
 * module, so no other lock is ever taken while this one is held. */
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

void cpe_status_deinit(void)
{
    if (!s_inited) {
        return;
    }
    /* Take the lock one last time so no setter is half way through, then leave
     * the module in its pre-init state (a setter afterwards is a no-op again). */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_status, 0, sizeof(s_status));
    s_inited = false;
    xSemaphoreGive(s_lock);
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
}

/* ------------------------------------------------------------------------- */
/* Setter helpers                                                            */
/* ------------------------------------------------------------------------- */

/* Every setter is the same three steps: check that the module is up, take the
 * lock, write the field. Keeping that boilerplate in these typed helpers (no
 * void *, so passing the wrong field is still a compile error) means a new
 * field cannot forget one of the steps. */
static bool status_enter(void)
{
    if (!s_inited) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return true;
}

static void status_leave(void)
{
    xSemaphoreGive(s_lock);
}

static void status_set_bool(bool *slot, bool value)
{
    if (!status_enter()) {
        return;
    }
    *slot = value;
    status_leave();
}

static void status_set_bool_pair(bool *slot, bool *valid_slot, bool value,
                                 bool valid)
{
    if (!status_enter()) {
        return;
    }
    *slot = value;
    *valid_slot = valid;
    status_leave();
}

static void status_set_i8_pair(int8_t *slot, bool *valid_slot, int value,
                               bool valid)
{
    if (!status_enter()) {
        return;
    }
    *slot = (int8_t)value;
    *valid_slot = valid;
    status_leave();
}

static void status_set_u32(uint32_t *slot, uint32_t value)
{
    if (!status_enter()) {
        return;
    }
    *slot = value;
    status_leave();
}

static void status_set_str(char *dst, size_t size, const char *src)
{
    if (!status_enter()) {
        return;
    }
    snprintf(dst, size, "%s", src != NULL ? src : "");
    status_leave();
}

void cpe_status_set_state(cpe_state_t state, const char *fmt, ...)
{
    if (!status_enter()) {
        return;
    }
    s_status.state = state;
    if (fmt != NULL) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(s_status.state_text, sizeof(s_status.state_text), fmt, args);
        va_end(args);
    }
    status_leave();
}

void cpe_status_set_usb_connected(bool connected)
{
    status_set_bool(&s_status.usb_connected, connected);
}

void cpe_status_set_link(bool up)
{
    status_set_bool(&s_status.link_up, up);
}

void cpe_status_set_ip(const char *ip)
{
    if (!status_enter()) {
        return;
    }
    /* Address and flag belong together: an empty address means the WAN is down,
     * so a reader can never see an IP without got_ip (or the other way round). */
    if (ip != NULL && ip[0] != '\0') {
        snprintf(s_status.ip, sizeof(s_status.ip), "%s", ip);
        s_status.got_ip = true;
    } else {
        s_status.ip[0] = '\0';
        s_status.got_ip = false;
    }
    status_leave();
}

void cpe_status_set_rssi(int rssi, bool valid)
{
    status_set_i8_pair(&s_status.rssi, &s_status.rssi_valid, rssi, valid);
}

void cpe_status_set_modem_info(const char *model, const char *fw, const char *imei)
{
    if (!status_enter()) {
        return;
    }
    /* One lock for all three: the console must never read a model from one
     * modem and an IMEI from another. */
    snprintf(s_status.modem_model, sizeof(s_status.modem_model), "%s", model != NULL ? model : "");
    snprintf(s_status.modem_fw, sizeof(s_status.modem_fw), "%s", fw != NULL ? fw : "");
    snprintf(s_status.imei, sizeof(s_status.imei), "%s", imei != NULL ? imei : "");
    status_leave();
}

void cpe_status_set_carrier(const char *carrier)
{
    status_set_str(s_status.carrier, sizeof(s_status.carrier), carrier);
}

void cpe_status_set_modem_registered(bool registered)
{
    status_set_bool(&s_status.modem_registered, registered);
}

void cpe_status_set_sim(bool inserted, bool valid)
{
    status_set_bool_pair(&s_status.sim_inserted, &s_status.sim_valid, inserted, valid);
}

void cpe_status_set_phone_number(const char *number)
{
    status_set_str(s_status.phone_number, sizeof(s_status.phone_number), number);
}

void cpe_status_set_ps(bool attached, bool valid)
{
    status_set_bool_pair(&s_status.ps_attached, &s_status.ps_valid, attached, valid);
}

void cpe_status_set_pdp(bool active, bool valid)
{
    status_set_bool_pair(&s_status.pdp_active, &s_status.pdp_valid, active, valid);
}

void cpe_status_set_signal_quality(int rsrp, int rsrq, int sinr, bool valid)
{
    if (!status_enter()) {
        return;
    }
    s_status.rsrp = (int16_t)rsrp;
    s_status.rsrq = (int8_t)rsrq;
    s_status.sinr = (int8_t)sinr;
    s_status.qcsq_valid = valid;
    status_leave();
}

void cpe_status_set_modem_temp(int temp_c, bool valid)
{
    status_set_i8_pair(&s_status.modem_temp, &s_status.modem_temp_valid, temp_c, valid);
}

void cpe_status_set_recovery_attempts(uint32_t attempts)
{
    status_set_u32(&s_status.recovery_attempts, attempts);
}

void cpe_status_set_recovery_stage(cpe_recovery_stage_t stage)
{
    if (!status_enter()) {
        return;
    }
    s_status.recovery_stage = stage;
    status_leave();
}

void cpe_status_set_boot_count(uint32_t boot_count)
{
    status_set_u32(&s_status.boot_count, boot_count);
}

void cpe_status_set_error(const char *fmt, ...)
{
    if (!status_enter()) {
        return;
    }
    if (fmt != NULL) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(s_status.last_error, sizeof(s_status.last_error), fmt, args);
        va_end(args);
    } else {
        s_status.last_error[0] = '\0';
    }
    status_leave();
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
