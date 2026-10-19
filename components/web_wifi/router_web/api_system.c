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
#include "cpe_auth.h"
#include "cpe_config.h"
#include "http_internal.h"

static const char *TAG = "cpe_web";

static esp_err_t api_factory_reset_post_handler(httpd_req_t *req)
{
    esp_err_t pre = cpe_web_precheck_post(req, false);
    if (pre != ESP_OK) {
        return pre;
    }
    char session[CPE_WEB_SESSION_BUF_LEN] = { 0 };
    /* Factory reset is allowed even while a forced password change is pending
     * (it is the recovery path), but it still requires a session + CSRF. */
    esp_err_t err = cpe_web_require_session(req, session, true);
    if (err != ESP_OK) {
        return err;
    }

    char body[CPE_WEB_MAX_JSON_BODY + 1];
    size_t body_len = 0;
    if (req->content_len > 0) {
        cpe_web_body_result_t body_result =
            cpe_web_read_json_body(req, body, sizeof(body), &body_len);
        cpe_web_secure_wipe(body, sizeof(body));
        if (body_result != CPE_WEB_BODY_OK) {
            return cpe_web_send_body_error(req, body_result);
        }
        (void)body_len;
    }

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
    app_wifi_deauth_all_stations();
    esp_restart();
    return err;
}

static esp_err_t api_system_reboot_post_handler(httpd_req_t *req)
{
    esp_err_t pre = cpe_web_precheck_post(req, false);
    if (pre != ESP_OK) {
        return pre;
    }
    char session[CPE_WEB_SESSION_BUF_LEN] = { 0 };
    esp_err_t err = cpe_web_require_session_mutable(req, session);
    if (err != ESP_OK) {
        return err;
    }
    if (req->content_len > 0) {
        char body[CPE_WEB_MAX_JSON_BODY + 1];
        size_t body_len = 0;
        if (cpe_web_read_json_body(req, body, sizeof(body), &body_len) !=
                CPE_WEB_BODY_OK) {
            return cpe_web_send_error_response(req, HTTPD_400,
                                               "invalid_body", true);
        }
    }

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 64);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"rebooting\"}");
    err = cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);

    ESP_LOGW(TAG, "Reboot requested from the web console");
    vTaskDelay(pdMS_TO_TICKS(500));
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
