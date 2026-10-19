/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * System status endpoint.
 */

#include <inttypes.h>

#include "cpe_config.h"
#include "cpe_status.h"
#include "http_internal.h"

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    esp_err_t body_check = cpe_web_reject_unexpected_body(req);
    if (body_check != ESP_OK) {
        return body_check;
    }
    char session[CPE_WEB_SESSION_BUF_LEN] = { 0 };
    if (cpe_web_require_session(req, session, false) != ESP_OK) {
        return ESP_FAIL;
    }

    cpe_status_t status = { 0 };
    cpe_status_get(&status);

    cpe_config_t cfg = { 0 };
    bool cfg_ok = (cpe_config_get(&cfg) == ESP_OK);

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 768);
    cpe_web_sbuf_appendf(&sb, "{\"status\":\"ok\",\"must_change\":%s,",
                         cpe_config_admin_must_change() ? "true" : "false");
    cpe_web_sbuf_appendf(&sb, "\"state\":%d,\"state_text\":", (int)status.state);
    cpe_web_sbuf_append_json_string(&sb, status.state_text);
    cpe_web_sbuf_appendf(&sb, ",\"usb_connected\":%s,\"link_up\":%s,\"got_ip\":%s,\"ip\":",
                         status.usb_connected ? "true" : "false",
                         status.link_up ? "true" : "false",
                         status.got_ip ? "true" : "false");
    cpe_web_sbuf_append_json_string(&sb, status.ip);
    cpe_web_sbuf_appendf(&sb, ",\"has_public_ip\":%s,\"public_ip\":",
                         status.has_public_ip ? "true" : "false");
    cpe_web_sbuf_append_json_string(&sb, status.public_ip);
    cpe_web_sbuf_appendf(&sb, ",\"rssi\":%d,\"rssi_valid\":%s,\"modem_model\":",
                         (int)status.rssi, status.rssi_valid ? "true" : "false");
    cpe_web_sbuf_append_json_string(&sb, status.modem_model);
    cpe_web_sbuf_append(&sb, ",\"modem_fw\":");
    cpe_web_sbuf_append_json_string(&sb, status.modem_fw);
    cpe_web_sbuf_appendf(&sb, ",\"uptime_s\":%" PRIu32 ",\"recovery_attempts\":%" PRIu32
                         ",\"boot_count\":%" PRIu32 ",\"last_error\":",
                         status.uptime_s, status.recovery_attempts, status.boot_count);
    cpe_web_sbuf_append_json_string(&sb, status.last_error);
    cpe_web_sbuf_append(&sb, ",\"device_name\":");
    cpe_web_sbuf_append_json_string(&sb, cfg_ok ? cfg.device_name : "");
    cpe_web_sbuf_append(&sb, ",\"ap\":{\"ssid\":");
    cpe_web_sbuf_append_json_string(&sb, cfg_ok ? cfg.ap_ssid : "");
    cpe_web_sbuf_appendf(&sb, ",\"channel\":%u,\"bandwidth\":\"%u\",\"hidden\":%s,\"max_sta\":%u}}",
                         (unsigned)(cfg_ok ? cfg.ap_channel : 0),
                         (unsigned)(cfg_ok ? cfg.ap_bandwidth : 0),
                         (cfg_ok && cfg.ap_hidden) ? "true" : "false",
                         (unsigned)(cfg_ok ? cfg.max_sta : 0));
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

esp_err_t cpe_web_api_status_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_get_handler },
    };
    return cpe_web_register_uris(server, uris, sizeof(uris) / sizeof(uris[0]));
}
