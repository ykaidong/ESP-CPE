/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Connected station endpoints.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "cpe_config.h"
#include "http_internal.h"

static const char *TAG = "cpe_web";

static esp_err_t system_station_get_handler(httpd_req_t *req)
{
    esp_err_t body_check = cpe_web_reject_unexpected_body(req);
    if (body_check != ESP_OK) {
        return body_check;
    }
    char session[CPE_WEB_SESSION_BUF_LEN] = { 0 };
    if (cpe_web_require_session(req, session, false) != ESP_OK) {
        return ESP_FAIL;
    }

    cpe_web_sta_snapshot_t snapshot[CPE_WEB_STA_SNAPSHOT_MAX];
    size_t count = cpe_web_sta_snapshot(snapshot, CPE_WEB_STA_SNAPSHOT_MAX);

    cpe_config_t cfg = { 0 };
    bool cfg_ok = (cpe_config_get(&cfg) == ESP_OK);

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 512);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"station_list\":[");
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            cpe_web_sbuf_append(&sb, ",");
        }
        cpe_web_sbuf_append(&sb, "{\"name_str\":");
        cpe_web_sbuf_append_json_string(&sb, snapshot[i].name);
        cpe_web_sbuf_append(&sb, ",\"mac_str\":");
        cpe_web_sbuf_append_json_string(&sb, snapshot[i].mac);
        cpe_web_sbuf_append(&sb, ",\"ip_str\":");
        cpe_web_sbuf_append_json_string(&sb, snapshot[i].ip);
        int64_t online_s = (esp_timer_get_time() - snapshot[i].start_time_us) / 1000000;
        if (online_s < 0) {
            online_s = 0;
        }
        cpe_web_sbuf_appendf(&sb, ",\"online_time_s\":%lld}", (long long)online_s);
    }
    cpe_web_sbuf_appendf(&sb, "],\"max_sta\":%u}",
                         (unsigned)(cfg_ok ? cfg.max_sta : CPE_MAX_STA_MAX));
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

static esp_err_t system_station_delete_device_post_handler(httpd_req_t *req)
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

    char mac_str[24] = { 0 };
    err = cpe_web_json_get_string_unescaped(ctx.parser, "mac_str",
                                            mac_str, sizeof(mac_str));
    cpe_web_post_end(&ctx);

    uint8_t mac[6] = { 0 };
    if (err != ESP_OK || !cpe_web_mac_parse(mac_str, mac)) {
        return cpe_web_send_error_response(req, HTTPD_400, "invalid_mac", false);
    }
    if (!cpe_web_sta_node_exists(mac)) {
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_404_STR,
                                           "station_not_found", false);
    }

    uint16_t aid = 0;
    err = esp_wifi_ap_get_sta_aid(mac, &aid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Station " MACSTR " is not associated: %s",
                 MAC2STR(mac), esp_err_to_name(err));
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_404_STR,
                                           "station_not_found", false);
    }

    err = esp_wifi_deauth_sta(aid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deauthenticate station: %s", esp_err_to_name(err));
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_500_STR,
                                           "deauth_failed", false);
    }

    ESP_LOGI(TAG, "Station " MACSTR " was disconnected from the web console", MAC2STR(mac));
    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 64);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"station_disconnected\"}");
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

static esp_err_t system_station_change_name_post_handler(httpd_req_t *req)
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

    char name_str[CPE_WEB_STA_NAME_MAX_LEN + 1] = { 0 };
    char mac_str[24] = { 0 };
    err = cpe_web_json_get_string_unescaped(ctx.parser, "name_str",
                                            name_str, sizeof(name_str));
    if (err == ESP_OK) {
        err = cpe_web_json_get_string_unescaped(ctx.parser, "mac_str",
                                                mac_str, sizeof(mac_str));
    }
    cpe_web_post_end(&ctx);

    uint8_t mac[6] = { 0 };
    if (err != ESP_OK || !cpe_web_mac_parse(mac_str, mac)) {
        return cpe_web_send_error_response(req, HTTPD_400, "invalid_request", false);
    }
    if (!cpe_web_station_name_valid(name_str)) {
        return cpe_web_send_error_response(req, HTTPD_400,
                                           "invalid_station_name", false);
    }
    if (!cpe_web_sta_node_rename(mac, name_str)) {
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_404_STR,
                                           "station_not_found", false);
    }

    /* Best effort persistence: the live list already carries the new name. */
    esp_err_t save_err = cpe_web_station_name_save(mac, name_str);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist station name: %s", esp_err_to_name(save_err));
    }

    ESP_LOGI(TAG, "Station " MACSTR " renamed from the web console", MAC2STR(mac));
    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 64);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"station_renamed\"}");
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

esp_err_t cpe_web_api_station_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/system/station_state", .method = HTTP_GET,
          .handler = system_station_get_handler },
        { .uri = "/system/station_state/delete_device", .method = HTTP_POST,
          .handler = system_station_delete_device_post_handler },
        { .uri = "/system/station_state/change_name", .method = HTTP_POST,
          .handler = system_station_change_name_post_handler },
    };
    return cpe_web_register_uris(server, uris, sizeof(uris) / sizeof(uris[0]));
}
