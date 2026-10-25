/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * System status endpoint.
 */

#include <inttypes.h>

#include "cpe_config.h"
#include "cpe_status.h"
#include "http_internal.h"

esp_err_t api_status_get_handler(httpd_req_t *req)
{
    cpe_status_t status = { 0 };
    cpe_status_get(&status);

    cpe_config_t cfg = { 0 };
    bool cfg_ok = (cpe_config_get(&cfg) == ESP_OK);

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 1024);
    cpe_web_sbuf_appendf(&sb, "{\"status\":\"ok\",\"must_change\":%s,",
                         cpe_config_admin_must_change() ? "true" : "false");
    cpe_web_sbuf_appendf(&sb, "\"state\":%d,\"state_text\":", (int)status.state);
    cpe_web_sbuf_append_json_string(&sb, status.state_text);
    cpe_web_sbuf_appendf(&sb, ",\"usb_connected\":%s,\"link_up\":%s,\"got_ip\":%s,\"ip\":",
                         status.usb_connected ? "true" : "false",
                         status.link_up ? "true" : "false",
                         status.got_ip ? "true" : "false");
    cpe_web_sbuf_append_json_string(&sb, status.ip);
    cpe_web_sbuf_appendf(&sb, ",\"rssi\":%d,\"rssi_valid\":%s,\"modem_model\":",
                         (int)status.rssi, status.rssi_valid ? "true" : "false");
    cpe_web_sbuf_append_json_string(&sb, status.modem_model);
    cpe_web_sbuf_append(&sb, ",\"modem_fw\":");
    cpe_web_sbuf_append_json_string(&sb, status.modem_fw);
    cpe_web_sbuf_append(&sb, ",\"imei\":");
    cpe_web_sbuf_append_json_string(&sb, status.imei);
    cpe_web_sbuf_append(&sb, ",\"carrier\":");
    cpe_web_sbuf_append_json_string(&sb, status.carrier);
    cpe_web_sbuf_appendf(&sb, ",\"modem_registered\":%s",
                         status.modem_registered ? "true" : "false");
    cpe_web_sbuf_appendf(&sb, ",\"sim_valid\":%s,\"sim_inserted\":%s",
                         status.sim_valid ? "true" : "false",
                         status.sim_inserted ? "true" : "false");
    cpe_web_sbuf_append(&sb, ",\"phone_number\":");
    cpe_web_sbuf_append_json_string(&sb, status.phone_number);
    cpe_web_sbuf_appendf(&sb, ",\"ps_attached\":%s,\"ps_valid\":%s",
                         status.ps_attached ? "true" : "false",
                         status.ps_valid ? "true" : "false");
    cpe_web_sbuf_appendf(&sb, ",\"pdp_active\":%s,\"pdp_valid\":%s",
                         status.pdp_active ? "true" : "false",
                         status.pdp_valid ? "true" : "false");
    cpe_web_sbuf_appendf(&sb, ",\"rsrp\":%d,\"rsrq\":%d,\"sinr\":%d,\"qcsq_valid\":%s",
                         (int)status.rsrp, (int)status.rsrq, (int)status.sinr,
                         status.qcsq_valid ? "true" : "false");
    cpe_web_sbuf_appendf(&sb, ",\"modem_temp\":%d,\"modem_temp_valid\":%s",
                         (int)status.modem_temp,
                         status.modem_temp_valid ? "true" : "false");
    cpe_web_sbuf_appendf(&sb, ",\"uptime_s\":%" PRIu32 ",\"recovery_attempts\":%" PRIu32
                         ",\"recovery_stage\":%d,\"boot_count\":%" PRIu32 ",\"last_error\":",
                         status.uptime_s, status.recovery_attempts,
                         (int)status.recovery_stage, status.boot_count);
    cpe_web_sbuf_append_json_string(&sb, status.last_error);
    cpe_web_sbuf_append(&sb, ",\"device_name\":");
    cpe_web_sbuf_append_json_string(&sb, cfg_ok ? cfg.device_name : "");
    cpe_web_sbuf_append(&sb, ",\"ap\":{\"ssid\":");
    cpe_web_sbuf_append_json_string(&sb, cfg_ok ? cfg.ap_ssid : "");
    cpe_web_sbuf_appendf(&sb, ",\"channel\":%u,\"bandwidth\":%u,\"hidden\":%s,\"max_sta\":%u}}",
                         (unsigned)(cfg_ok ? cfg.ap_channel : 0),
                         (unsigned)(cfg_ok ? cfg.ap_bandwidth : 0),
                         (cfg_ok && cfg.ap_hidden) ? "true" : "false",
                         (unsigned)(cfg_ok ? cfg.max_sta : 0));
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}
