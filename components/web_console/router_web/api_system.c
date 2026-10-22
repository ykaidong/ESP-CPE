/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * System maintenance endpoints: factory reset, reboot and the periodic
 * maintenance policy (ECM rebuild / reboot periods).
 */

#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"

#include "cpe_auth.h"
#include "cpe_config.h"
#include "cpe_maintenance.h"
#include "cpe_wan.h"
#include "cpe_web.h"
#include "http_internal.h"

static const char *TAG = "cpe_web";

/* Restarting is requested from a handler, but it must not run there: the grace
 * delay and the modem reset inside cpe_wan_reboot() would keep the single HTTP
 * server task busy after the reply has been queued. The shared helper waits for
 * the queued reply to leave the socket first. */
static void schedule_system_reboot(void)
{
    if (cpe_wan_reboot_deferred(true, "web console") != ESP_OK) {
        ESP_LOGW(TAG, "No memory for the reboot task, restarting from the handler");
        cpe_wan_reboot(true);
    }
}

/* Append the maintenance policy and the schedule the device is following. */
static void append_maintenance_status(cpe_web_sbuf_t *sb)
{
    cpe_maint_status_t st;
    if (cpe_maint_status_get(&st) != ESP_OK) {
        st.config.ecm_period_min = 0;
        st.config.reboot_period_min = 0;
        st.config.reset_modem = false;
        st.next_ecm_in_s = -1;
        st.next_reboot_in_s = -1;
        st.sched_reboot_count = 0;
        st.reboot_frozen = false;
    }
    cpe_web_sbuf_appendf(sb, ",\"ecm_period_min\":%" PRIu32
                         ",\"reboot_period_min\":%" PRIu32
                         ",\"reset_modem\":%s,\"period_max_min\":%d"
                         ",\"next_ecm_in_s\":%" PRIi64
                         ",\"next_reboot_in_s\":%" PRIi64
                         ",\"sched_reboot_count\":%" PRIu32
                         ",\"reboot_frozen\":%s",
                         st.config.ecm_period_min, st.config.reboot_period_min,
                         st.config.reset_modem ? "true" : "false",
                         CPE_MAINT_PERIOD_MAX_MIN,
                         st.next_ecm_in_s, st.next_reboot_in_s,
                         st.sched_reboot_count,
                         st.reboot_frozen ? "true" : "false");
}

static esp_err_t api_factory_reset_post_handler(httpd_req_t *req)
{
    /* Factory reset is allowed even while a forced password change is pending
     * (it is the recovery path), but it still requires a session + CSRF. */
    static const cpe_web_post_opts_t opts = {
        .session = CPE_WEB_POST_SESSION_REQUIRED,
        .body = CPE_WEB_POST_BODY_OPTIONAL,
        .csrf = true,
    };
    cpe_web_post_ctx_t ctx;
    esp_err_t err = cpe_web_post_begin(req, &opts, &ctx);
    if (err != ESP_OK) {
        return err;
    }
    cpe_web_post_end(&ctx);

    err = cpe_config_factory_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset failed: %s", esp_err_to_name(err));
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_500_STR,
                                           "factory_reset_failed", false);
    }

    cpe_web_station_names_erase_all();
    /* The maintenance policy is not part of the security configuration, but a
     * factory reset must return every user visible setting to its default. */
    (void)cpe_maint_config_reset();
    cpe_auth_logout_all();
    (void)cpe_web_clear_session_cookie(req);
    cpe_web_clear_csrf_cookie(req);
    ESP_LOGW(TAG, "Factory reset requested from the web console, restarting");

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 96);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"factory_reset\"}");
    err = cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);

    /* Give the response a chance to leave the socket before restarting; the
     * wait itself runs on its own task so the console stays responsive. */
    schedule_system_reboot();
    return err;
}

static esp_err_t api_system_reboot_post_handler(httpd_req_t *req)
{
    static const cpe_web_post_opts_t opts = {
        .session = CPE_WEB_POST_SESSION_MUTABLE,
        .body = CPE_WEB_POST_BODY_OPTIONAL,
    };
    cpe_web_post_ctx_t ctx;
    esp_err_t err = cpe_web_post_begin(req, &opts, &ctx);
    if (err != ESP_OK) {
        return err;
    }
    cpe_web_post_end(&ctx);

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 64);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"rebooting\"}");
    err = cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);

    ESP_LOGW(TAG, "Reboot requested from the web console");
    schedule_system_reboot();
    return err;
}

static esp_err_t api_maintenance_get_handler(httpd_req_t *req)
{
    esp_err_t body_check = cpe_web_reject_unexpected_body(req);
    if (body_check != ESP_OK) {
        return body_check;
    }
    char session[CPE_WEB_SESSION_BUF_LEN] = { 0 };
    if (cpe_web_require_session(req, session, false) != ESP_OK) {
        return ESP_FAIL;
    }

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 256);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\"");
    append_maintenance_status(&sb);
    cpe_web_sbuf_append(&sb, "}");
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

static esp_err_t api_maintenance_post_handler(httpd_req_t *req)
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

    int ecm_period = 0;
    int reboot_period = 0;
    bool has_ecm = false;
    bool has_reboot = false;

    err = cpe_web_json_get_int_flexible(ctx.parser, "ecm_period_min", &ecm_period, &has_ecm);
    if (err == ESP_OK) {
        err = cpe_web_json_get_int_flexible(ctx.parser, "reboot_period_min",
                                            &reboot_period, &has_reboot);
    }
    cpe_web_post_end(&ctx);

    if (err != ESP_OK) {
        return cpe_web_send_error_response(req, HTTPD_400,
                                           "invalid_request_body", false);
    }
    if (!has_ecm && !has_reboot) {
        return cpe_web_send_error_response(req, HTTPD_400, "no_fields", false);
    }
    if ((has_ecm && (ecm_period < 0 || ecm_period > CPE_MAINT_PERIOD_MAX_MIN)) ||
            (has_reboot && (reboot_period < 0 || reboot_period > CPE_MAINT_PERIOD_MAX_MIN))) {
        return cpe_web_send_error_response(req, HTTPD_400,
                                           "invalid_maintenance_period", false);
    }

    cpe_maint_config_t cfg;
    if (cpe_maint_config_get(&cfg) != ESP_OK) {
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_500_STR,
                                           "config_unavailable", false);
    }
    if (has_ecm) {
        cfg.ecm_period_min = (uint32_t)ecm_period;
    }
    if (has_reboot) {
        cfg.reboot_period_min = (uint32_t)reboot_period;
    }

    err = cpe_maint_config_set(&cfg);
    if (err == ESP_ERR_INVALID_ARG) {
        return cpe_web_send_error_response(req, HTTPD_400,
                                           "invalid_maintenance_period", false);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist the maintenance policy: %s", esp_err_to_name(err));
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_500_STR,
                                           "persist_failed", false);
    }

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 256);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\"");
    append_maintenance_status(&sb);
    cpe_web_sbuf_append(&sb, "}");
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

esp_err_t cpe_web_api_system_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/factory_reset", .method = HTTP_POST,
          .handler = api_factory_reset_post_handler },
        { .uri = "/api/system/reboot", .method = HTTP_POST,
          .handler = api_system_reboot_post_handler },
        { .uri = "/api/system/maintenance", .method = HTTP_GET,
          .handler = api_maintenance_get_handler },
        { .uri = "/api/system/maintenance", .method = HTTP_POST,
          .handler = api_maintenance_post_handler },
    };
    return cpe_web_register_uris(server, uris, sizeof(uris) / sizeof(uris[0]));
}
