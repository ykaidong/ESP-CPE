/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SoftAP configuration endpoints (/wlan_general and /wlan_advance).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "app_wifi.h"
#include "cpe_config.h"
#include "http_internal.h"

static const char *TAG = "cpe_web";

typedef struct {
    const cpe_config_t *next;
    bool set_password;
} wlan_general_update_t;

static esp_err_t wlan_general_mutate(cpe_config_t *cfg, void *arg)
{
    const wlan_general_update_t *update = arg;
    const cpe_config_t *next = update->next;
    snprintf(cfg->ap_ssid, sizeof(cfg->ap_ssid), "%s", next->ap_ssid);
    if (update->set_password) {
        snprintf(cfg->ap_password, sizeof(cfg->ap_password), "%s", next->ap_password);
    }
    cfg->ap_hidden = next->ap_hidden;
    cfg->max_sta = next->max_sta;
    snprintf(cfg->device_name, sizeof(cfg->device_name), "%s", next->device_name);
    return ESP_OK;
}

static esp_err_t wlan_advance_mutate(cpe_config_t *cfg, void *arg)
{
    const cpe_config_t *next = arg;
    cfg->ap_channel = next->ap_channel;
    cfg->ap_bandwidth = next->ap_bandwidth;
    return ESP_OK;
}

/* Saving /wlan_general restarts the SoftAP, and the HTTP listener must be
 * closed before the radio goes down. httpd_stop() waits for the task that runs
 * the current handler to exit, so it cannot be called from the handler itself:
 * the apply sequence runs in this short-lived helper task. */
#define WLAN_APPLY_TASK_STACK        4096
#define WLAN_APPLY_TASK_PRIO         5

/* Time the queued reply gets to leave the radio before the listener closes its
 * socket. */
#define WLAN_APPLY_REPLY_GRACE_MS    120

/* Time WIFI_EVENT_AP_START gets to arrive before the task restores the
 * listener itself (the radio does not restart when the apply fails). */
#define WLAN_APPLY_SETTLE_MS         200

/* Shared by /wlan_general (SSID/password/max_sta restart the AP and its netif,
 * so the listener has to go down first) and /wlan_advance (channel/bandwidth
 * keep the AP running, so the listener stays up). */
typedef struct {
    cpe_config_t prev;
    cpe_config_t next;
    wlan_general_update_t update;   /* /wlan_general only */
    cpe_config_mutator_t mutate;
    void *mutate_arg;
    bool suspend_listener;
} wlan_apply_job_t;

static void wlan_apply_task(void *arg)
{
    wlan_apply_job_t *job = arg;

    /* Let the response leave the radio before its socket is closed. */
    vTaskDelay(pdMS_TO_TICKS(WLAN_APPLY_REPLY_GRACE_MS));

    /* Close the listener ahead of a radio restart so the browser sees a clean
     * close instead of a half-open socket stuck in its connection pool. */
    if (job->suspend_listener) {
        cpe_web_listener_suspend_for_ap_restart();
    }

    /* app_wifi_apply() reads the persisted configuration as its rollback
     * baseline, so it must run before cpe_config_update(). Errors can no longer
     * be reported to the client, only logged. */
    esp_err_t err = app_wifi_apply(&job->next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply new WLAN settings: %s", esp_err_to_name(err));
    } else {
        err = cpe_config_update(job->mutate, job->mutate_arg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist WLAN settings: %s", esp_err_to_name(err));
            (void)app_wifi_apply(&job->prev); /* roll the radio back */
        }
    }

    /* WIFI_EVENT_AP_START re-creates the listener on its own; this call only
     * matters when the radio never restarted (failed apply). */
    vTaskDelay(pdMS_TO_TICKS(WLAN_APPLY_SETTLE_MS));
    cpe_web_listener_ensure_up();

    cpe_web_secure_wipe(job->next.ap_password, sizeof(job->next.ap_password));
    cpe_web_secure_wipe(job->prev.ap_password, sizeof(job->prev.ap_password));
    free(job);
    vTaskDelete(NULL);
}

esp_err_t wlan_general_get_handler(httpd_req_t *req)
{
    cpe_config_t cfg;
    esp_err_t err = cpe_config_get(&cfg);
    if (err != ESP_OK) {
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_500_STR,
                                           "config_unavailable", false);
    }

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 384);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"ssid\":");
    cpe_web_sbuf_append_json_string(&sb, cfg.ap_ssid);
    cpe_web_sbuf_appendf(&sb, ",\"if_hide_ssid\":%s,\"auth_mode\":\"WPA2_PSK\",\"has_password\":%s",
                         cfg.ap_hidden ? "true" : "false",
                         cfg.ap_password[0] != '\0' ? "true" : "false");
    cpe_web_sbuf_append(&sb, ",\"device_name\":");
    cpe_web_sbuf_append_json_string(&sb, cfg.device_name);
    cpe_web_sbuf_appendf(&sb, ",\"max_sta\":%u,\"channel\":%u,\"bandwidth\":%u}",
                         (unsigned)cfg.max_sta, (unsigned)cfg.ap_channel,
                         (unsigned)cfg.ap_bandwidth);
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

esp_err_t wlan_general_post_handler(httpd_req_t *req)
{
    static const cpe_web_post_opts_t opts = {
        .session = CPE_WEB_POST_SESSION_MUTABLE,
        .body = CPE_WEB_POST_BODY_REQUIRED,
        .parse_json = true,
    };
    cpe_web_post_ctx_t ctx;
    esp_err_t err = cpe_web_post_begin(req, &opts, &ctx);
    if (err != ESP_OK) {
        return err;
    }

    char ssid[CPE_SSID_MAX_LEN + 1] = { 0 };
    char password[CPE_WIFI_PASS_MAX_LEN + 1] = { 0 };
    char device_name[CPE_DEVICE_NAME_MAX_LEN + 1] = { 0 };
    bool has_ssid = false;
    bool has_password = false;
    bool has_device_name = false;
    bool has_hidden = false;
    bool hidden = false;
    bool has_max_sta = false;
    int max_sta = 0;

    err = cpe_web_json_get_string_unescaped(ctx.parser, "ssid", ssid, sizeof(ssid));
    has_ssid = (err == ESP_OK);
    if (err == ESP_ERR_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = cpe_web_json_get_string_unescaped(ctx.parser, "password",
                                                password, sizeof(password));
        has_password = (err == ESP_OK);
        if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = cpe_web_json_get_string_unescaped(ctx.parser, "device_name",
                                                device_name, sizeof(device_name));
        has_device_name = (err == ESP_OK);
        if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = cpe_web_json_get_bool_flexible(ctx.parser, "if_hide_ssid",
                                             &hidden, &has_hidden);
    }
    if (err == ESP_OK) {
        err = cpe_web_json_get_int_flexible(ctx.parser, "max_sta",
                                             &max_sta, &has_max_sta);
    }
    cpe_web_post_end(&ctx);

    if (err != ESP_OK) {
        cpe_web_secure_wipe(password, sizeof(password));
        return cpe_web_send_error_response(req, HTTPD_400, "invalid_request", false);
    }

    /* ---- server side validation (audit P1-13) ---- */
    if (!has_ssid || cpe_config_validate_ap_ssid(ssid) != ESP_OK) {
        cpe_web_secure_wipe(password, sizeof(password));
        return cpe_web_send_error_response(req, HTTPD_400, "invalid_ssid", false);
    }
    bool password_changed = false;
    if (has_password && password[0] != '\0') {
        if (cpe_config_validate_wifi_password(password) != ESP_OK) {
            cpe_web_secure_wipe(password, sizeof(password));
            return cpe_web_send_error_response(req, HTTPD_400, "invalid_password", false);
        }
        password_changed = true;
    }
    if (has_device_name && cpe_config_validate_device_name(device_name) != ESP_OK) {
        cpe_web_secure_wipe(password, sizeof(password));
        return cpe_web_send_error_response(req, HTTPD_400,
                                           "invalid_device_name", false);
    }
    if (has_max_sta && cpe_config_validate_max_sta(max_sta) != ESP_OK) {
        cpe_web_secure_wipe(password, sizeof(password));
        return cpe_web_send_error_response(req, HTTPD_400, "invalid_max_sta", false);
    }

    cpe_config_t current;
    err = cpe_config_get(&current);
    if (err != ESP_OK) {
        cpe_web_secure_wipe(password, sizeof(password));
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_500_STR,
                                           "config_unavailable", false);
    }

    cpe_config_t next = current;
    snprintf(next.ap_ssid, sizeof(next.ap_ssid), "%s", ssid);
    if (password_changed) {
        snprintf(next.ap_password, sizeof(next.ap_password), "%s", password);
    }
    if (has_hidden) {
        next.ap_hidden = hidden;
    }
    if (has_max_sta) {
        next.max_sta = (uint8_t)max_sta;
    }
    if (has_device_name) {
        snprintf(next.device_name, sizeof(next.device_name), "%s", device_name);
    }

    /* A device-name-only change must not restart the SoftAP. */
    bool ap_changed = (strcmp(current.ap_ssid, next.ap_ssid) != 0) ||
                      password_changed ||
                      (current.ap_hidden != next.ap_hidden) ||
                      (current.max_sta != next.max_sta);

    cpe_web_secure_wipe(password, sizeof(password));
    ESP_LOGI(TAG, "WLAN settings updated (SSID changed: %s, password changed: %s, hidden: %d)",
             strcmp(current.ap_ssid, next.ap_ssid) != 0 ? "yes" : "no",
             password_changed ? "yes" : "no",
             next.ap_hidden ? 1 : 0);

    wlan_general_update_t update = {
        .next = &next,
        .set_password = password_changed,
    };

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 96);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"settings_applied\"}");

    if (!ap_changed) {
        /* Device-name-only change: persist it before replying so a failure can
         * still be reported to the client. */
        if (has_device_name && strcmp(current.device_name, next.device_name) != 0) {
            err = cpe_config_update(wlan_general_mutate, &update);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to persist device name: %s", esp_err_to_name(err));
                cpe_web_sbuf_free(&sb);
                cpe_web_secure_wipe(next.ap_password, sizeof(next.ap_password));
                return cpe_web_send_error_response(req, CPE_WEB_HTTPD_500_STR,
                                                   "persist_failed", false);
            }
        }
        esp_err_t reply_err = cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
        cpe_web_secure_wipe(next.ap_password, sizeof(next.ap_password));
        return reply_err;
    }

    /* Reply before touching the radio. app_wifi_apply() restarts the SoftAP and
     * drops this client, so a reply sent afterwards never reaches the browser:
     * the socket stays half-open in the browser connection pool and enough of
     * them make the console look unreachable until TCP gives up. */
    esp_err_t reply_err = cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);

    /* Hand the radio work to a helper task: the listener has to be closed
     * before the AP restarts, and httpd_stop() cannot run inside a handler. */
    wlan_apply_job_t *job = calloc(1, sizeof(*job));
    if (job != NULL) {
        job->prev = current;
        job->next = next;
        job->update.next = &job->next;
        job->update.set_password = password_changed;
        job->mutate = wlan_general_mutate;
        job->mutate_arg = &job->update;
        job->suspend_listener = true;
        if (xTaskCreate(wlan_apply_task, "cpe_wlan_apply",
                        WLAN_APPLY_TASK_STACK, job, WLAN_APPLY_TASK_PRIO,
                        NULL) == pdPASS) {
            cpe_web_secure_wipe(next.ap_password, sizeof(next.ap_password));
            return reply_err;
        }
        ESP_LOGW(TAG, "No memory for the WLAN apply task, applying inline");
        cpe_web_secure_wipe(job->next.ap_password, sizeof(job->next.ap_password));
        free(job);
    }

    /* Fallback: apply inline. The listener stays up here and is rebuilt by the
     * WIFI_EVENT_AP_START handler once the AP is back. */
    err = app_wifi_apply(&next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply new WLAN settings: %s", esp_err_to_name(err));
    } else {
        err = cpe_config_update(wlan_general_mutate, &update);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist WLAN settings: %s", esp_err_to_name(err));
            (void)app_wifi_apply(&current); /* roll the radio back */
        }
    }
    cpe_web_secure_wipe(next.ap_password, sizeof(next.ap_password));
    return reply_err;
}

esp_err_t wlan_advance_get_handler(httpd_req_t *req)
{
    cpe_config_t cfg;
    esp_err_t err = cpe_config_get(&cfg);
    if (err != ESP_OK) {
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_500_STR,
                                           "config_unavailable", false);
    }
    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 96);
    cpe_web_sbuf_appendf(&sb, "{\"status\":\"ok\",\"bandwidth\":%u,\"channel\":%u}",
                         (unsigned)cfg.ap_bandwidth, (unsigned)cfg.ap_channel);
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

esp_err_t wlan_advance_post_handler(httpd_req_t *req)
{
    static const cpe_web_post_opts_t opts = {
        .session = CPE_WEB_POST_SESSION_MUTABLE,
        .body = CPE_WEB_POST_BODY_REQUIRED,
        .parse_json = true,
    };
    cpe_web_post_ctx_t ctx;
    esp_err_t err = cpe_web_post_begin(req, &opts, &ctx);
    if (err != ESP_OK) {
        return err;
    }

    bool has_channel = false;
    bool has_bandwidth = false;
    int channel = 0;
    int bandwidth = 0;
    err = cpe_web_json_get_int_flexible(ctx.parser, "channel", &channel, &has_channel);
    if (err == ESP_OK) {
        err = cpe_web_json_get_int_flexible(ctx.parser, "bandwidth",
                                             &bandwidth, &has_bandwidth);
    }
    cpe_web_post_end(&ctx);

    if (err != ESP_OK || !has_channel || !has_bandwidth) {
        return cpe_web_send_error_response(req, HTTPD_400, "invalid_request", false);
    }
    if (cpe_config_validate_channel(channel) != ESP_OK) {
        return cpe_web_send_error_response(req, HTTPD_400, "invalid_channel", false);
    }
    if (cpe_config_validate_bandwidth(bandwidth) != ESP_OK) {
        return cpe_web_send_error_response(req, HTTPD_400, "invalid_bandwidth", false);
    }

    cpe_config_t current;
    err = cpe_config_get(&current);
    if (err != ESP_OK) {
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_500_STR,
                                           "config_unavailable", false);
    }
    cpe_config_t next = current;
    next.ap_channel = (uint8_t)channel;
    next.ap_bandwidth = (uint8_t)bandwidth;
    bool ap_changed = (current.ap_channel != next.ap_channel) ||
                      (current.ap_bandwidth != next.ap_bandwidth);

    ESP_LOGI(TAG, "WLAN advanced settings updated (channel %d, bandwidth %d MHz)",
             channel, bandwidth);

    /* Same ordering rule as /wlan_general: the reply must be queued before
     * the channel change drops the client. */
    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 96);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"settings_applied\"}");
    esp_err_t reply_err = cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);

    if (ap_changed) {
        /* Channel changes keep the AP (and its netif) running, so the listener
         * survives; only give the reply time to leave the radio. The apply
         * sequence runs in the helper task /wlan_general uses: it writes flash
         * and must not block the HTTP server task. */
        wlan_apply_job_t *job = calloc(1, sizeof(*job));
        if (job != NULL) {
            job->prev = current;
            job->next = next;
            job->mutate = wlan_advance_mutate;
            job->mutate_arg = &job->next;
            job->suspend_listener = false;
            if (xTaskCreate(wlan_apply_task, "cpe_wlan_apply",
                            WLAN_APPLY_TASK_STACK, job, WLAN_APPLY_TASK_PRIO,
                            NULL) == pdPASS) {
                return reply_err;
            }
            ESP_LOGW(TAG, "No memory for the WLAN apply task, applying inline");
            free(job);
        }

        vTaskDelay(pdMS_TO_TICKS(WLAN_APPLY_REPLY_GRACE_MS));
        err = app_wifi_apply(&next);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to apply advanced WLAN settings: %s",
                     esp_err_to_name(err));
        } else {
            err = cpe_config_update(wlan_advance_mutate, &next);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to persist advanced WLAN settings: %s",
                         esp_err_to_name(err));
                (void)app_wifi_apply(&current);
            }
        }
    }
    return reply_err;
}


