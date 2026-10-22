/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * System maintenance endpoints: factory reset and reboot.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"

#include "app_wifi.h"
#include "cpe_at.h"
#include "cpe_auth.h"
#include "cpe_config.h"
#include "cpe_web.h"
#include "http_internal.h"

static const char *TAG = "cpe_web";

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
    cpe_auth_logout_all();
    (void)cpe_web_clear_session_cookie(req);
    cpe_web_clear_csrf_cookie(req);
    ESP_LOGW(TAG, "Factory reset requested from the web console, restarting");

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 96);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"factory_reset\"}");
    err = cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);

    /* Give the response a chance to leave the socket before restarting. */
    vTaskDelay(pdMS_TO_TICKS(500));
    /* Reset the modem first so it re-enumerates cleanly after the ESP boots;
     * best effort, never blocks the restart beyond the bounded wait. */
    esp_err_t modem_err = cpe_at_modem_reset(CPE_AT_MODEM_RESET_TIMEOUT_MS);
    if (modem_err != ESP_OK) {
        ESP_LOGW(TAG, "Modem reset did not succeed (%s)", esp_err_to_name(modem_err));
    }
    app_wifi_deauth_all_stations();
    esp_restart();
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
    vTaskDelay(pdMS_TO_TICKS(500));
    /* Reset the modem first so it re-enumerates cleanly after the ESP boots. */
    esp_err_t modem_err = cpe_at_modem_reset(CPE_AT_MODEM_RESET_TIMEOUT_MS);
    if (modem_err != ESP_OK) {
        ESP_LOGW(TAG, "Modem reset did not succeed (%s)", esp_err_to_name(modem_err));
    }
    app_wifi_deauth_all_stations();
    esp_restart();
    return err;
}

esp_err_t cpe_web_api_system_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/factory_reset", .method = HTTP_POST,
          .handler = api_factory_reset_post_handler },
        { .uri = "/api/system/reboot", .method = HTTP_POST,
          .handler = api_system_reboot_post_handler },
    };
    return cpe_web_register_uris(server, uris, sizeof(uris) / sizeof(uris[0]));
}
