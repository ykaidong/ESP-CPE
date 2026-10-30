/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SMS forwarding endpoints: the broker configuration, the runtime state and
 * the optional custom CA certificate. Without an uploaded CA the broker is
 * verified against the firmware's built-in public bundle; an uploaded PEM
 * replaces that bundle for this connection.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"

#include "cpe_sms_mqtt.h"
#include "http_internal.h"

static const char *TAG = "cpe_web";

#if CONFIG_CPE_SMS_MQTT_ENABLE
#define SMS_MQTT_SUPPORTED true
#else
#define SMS_MQTT_SUPPORTED false
#endif

static const char *mqtt_state_name(cpe_sms_mqtt_state_t state)
{
    switch (state) {
    case CPE_SMS_MQTT_DISABLED:   return "disabled";
    case CPE_SMS_MQTT_IDLE:       return "idle";
    case CPE_SMS_MQTT_CONNECTING: return "connecting";
    case CPE_SMS_MQTT_CONNECTED:  return "connected";
    case CPE_SMS_MQTT_ERROR:      return "error";
    default:                      return "unknown";
    }
}

esp_err_t api_sms_mqtt_get_handler(httpd_req_t *req)
{
    cpe_sms_mqtt_config_t cfg;
    if (cpe_sms_mqtt_get_config(&cfg) != ESP_OK) {
        return cpe_web_send_error(req, "config_unavailable");
    }
    cpe_sms_mqtt_status_t st;
    cpe_sms_mqtt_get_status(&st);

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 768);
    cpe_web_sbuf_appendf(&sb, "{\"status\":\"ok\",\"supported\":%s,\"config\":{",
                         SMS_MQTT_SUPPORTED ? "true" : "false");
    cpe_web_sbuf_appendf(&sb, "\"enabled\":%s,\"allow_remote_send\":%s,",
                         cfg.enabled ? "true" : "false",
                         cfg.allow_remote_send ? "true" : "false");
    cpe_web_sbuf_append(&sb, "\"host\":");
    cpe_web_sbuf_append_json_string(&sb, cfg.host);
    cpe_web_sbuf_appendf(&sb, ",\"port\":%u,\"username\":", (unsigned)cfg.port);
    cpe_web_sbuf_append_json_string(&sb, cfg.username);
    cpe_web_sbuf_append(&sb, ",\"topic_up\":");
    cpe_web_sbuf_append_json_string(&sb, cfg.topic_up);
    cpe_web_sbuf_append(&sb, ",\"topic_down\":");
    cpe_web_sbuf_append_json_string(&sb, cfg.topic_down);

    cpe_web_sbuf_appendf(&sb, "},\"runtime\":{\"state\":\"%s\",\"wan_online\":%s,"
                         "\"time_synced\":%s,\"cert_installed\":%s,\"queued\":%u,"
                         "\"pending\":%u,\"free_heap\":%u,\"last_error\":",
                         mqtt_state_name(st.state),
                         st.wan_online ? "true" : "false",
                         st.time_synced ? "true" : "false",
                         st.cert_installed ? "true" : "false",
                         (unsigned)st.queued, (unsigned)st.pending,
                         (unsigned)st.free_heap);
    cpe_web_sbuf_append_json_string(&sb, st.last_error);
    cpe_web_sbuf_append(&sb, "}}");
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

esp_err_t api_sms_mqtt_post_handler(httpd_req_t *req)
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

    cpe_sms_mqtt_config_t cfg;
    cpe_sms_mqtt_get_config(&cfg);

    char text[CPE_SMS_MQTT_TOPIC_MAX + 1];
    char user[CPE_SMS_MQTT_USER_MAX + 1];
    char pass[CPE_SMS_MQTT_PASS_MAX + 1];
    bool present = false;
    bool flag = false;
    int number = 0;

    err = cpe_web_json_get_bool_flexible(ctx.parser, "enabled", &flag, &present);
    if (err == ESP_OK && present) {
        cfg.enabled = flag;
    }
    if (err == ESP_OK) {
        err = cpe_web_json_get_bool_flexible(ctx.parser, "allow_remote_send", &flag,
                                             &present);
        if (err == ESP_OK && present) {
            cfg.allow_remote_send = flag;
        }
    }
    if (err == ESP_OK) {
        err = cpe_web_json_get_string_unescaped(ctx.parser, "host", cfg.host,
                                                sizeof(cfg.host));
        if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = cpe_web_json_get_int_flexible(ctx.parser, "port", &number, &present);
        if (err == ESP_OK && present) {
            if (number <= 0 || number > 65535) {
                err = ESP_ERR_INVALID_ARG;
            } else {
                cfg.port = (uint16_t)number;
            }
        }
    }
    if (err == ESP_OK) {
        err = cpe_web_json_get_string_unescaped(ctx.parser, "username", user,
                                                sizeof(user));
        if (err == ESP_OK) {
            snprintf(cfg.username, sizeof(cfg.username), "%s", user);
            if (cfg.username[0] == '\0') {
                /* Back to anonymous access: an orphaned password would be
                 * refused by the configuration check. */
                cfg.password[0] = '\0';
            }
        } else if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    /* An empty password means "keep the stored one": the page never renders the
     * existing value, so an untouched field must not wipe it. */
    if (err == ESP_OK) {
        err = cpe_web_json_get_string_unescaped(ctx.parser, "password", pass,
                                                sizeof(pass));
        if (err == ESP_OK) {
            if (pass[0] != '\0') {
                snprintf(cfg.password, sizeof(cfg.password), "%s", pass);
            }
        } else if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = cpe_web_json_get_string_unescaped(ctx.parser, "topic_up", text,
                                                sizeof(text));
        if (err == ESP_OK) {
            snprintf(cfg.topic_up, sizeof(cfg.topic_up), "%s", text);
        } else if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = cpe_web_json_get_string_unescaped(ctx.parser, "topic_down", text,
                                                sizeof(text));
        if (err == ESP_OK) {
            snprintf(cfg.topic_down, sizeof(cfg.topic_down), "%s", text);
        } else if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    cpe_web_post_end(&ctx);

    if (err != ESP_OK) {
        return cpe_web_send_error(req, "invalid_request_body");
    }
    if (!SMS_MQTT_SUPPORTED) {
        return cpe_web_send_error(req, "sms_mqtt_not_supported");
    }
    const char *reason = NULL;
    if (cpe_sms_mqtt_check_config(&cfg, &reason) != ESP_OK) {
        return cpe_web_send_error(req, reason != NULL ? reason : "invalid_request_body");
    }

    err = cpe_sms_mqtt_set_config(&cfg);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return cpe_web_send_error(req, "sms_mqtt_not_supported");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save the forwarding configuration: %s",
                 esp_err_to_name(err));
        return cpe_web_send_error(req, "sms_mqtt_save_failed");
    }

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 64);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"sms_mqtt_saved\"}");
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

/* A certificate arrives as a raw PEM body (the browser sends text/plain), so
 * the JSON POST preamble does not apply: its 2 KB limit and its content-type
 * check would both reject a legitimate file. Session, CSRF, origin and the
 * size bound are enforced here instead, before a byte is read. */
esp_err_t api_sms_mqtt_ca_post_handler(httpd_req_t *req)
{
    char session[CPE_WEB_SESSION_BUF_LEN];
    esp_err_t err = cpe_web_require_session_mutable(req, session);
    cpe_web_secure_wipe(session, sizeof(session));
    if (err != ESP_OK) {
        return err;
    }
    if (!cpe_web_request_origin_is_same_origin(req)) {
        return cpe_web_send_error(req, "cross_origin_rejected");
    }
    if (!SMS_MQTT_SUPPORTED) {
        return cpe_web_send_error(req, "sms_mqtt_not_supported");
    }
    if (req->content_len == 0) {
        return cpe_web_send_error(req, "empty_body");
    }
    if (req->content_len > CPE_SMS_MQTT_PEM_MAX) {
        return cpe_web_send_error(req, "body_too_large");
    }

    char *pem = malloc(CPE_SMS_MQTT_PEM_MAX + 1);
    if (pem == NULL) {
        return cpe_web_send_error(req, "out_of_memory");
    }
    size_t len = 0;
    cpe_web_body_result_t body =
        cpe_web_read_json_body(req, pem, CPE_SMS_MQTT_PEM_MAX + 1, &len);
    if (body != CPE_WEB_BODY_OK) {
        free(pem);
        return cpe_web_send_body_error(req, body);
    }
    bool valid = cpe_sms_mqtt_pem_valid(pem);
    if (valid) {
        err = cpe_sms_mqtt_set_cert(pem);
    }
    free(pem);
    if (!valid) {
        return cpe_web_send_error(req, "invalid_mqtt_cert");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not install the CA certificate: %s",
                 esp_err_to_name(err));
        return cpe_web_send_error(req, "persist_failed");
    }

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 64);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"sms_mqtt_ca_saved\"}");
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

esp_err_t api_sms_mqtt_ca_clear_post_handler(httpd_req_t *req)
{
    static const cpe_web_post_opts_t opts = {
        .session = CPE_WEB_POST_SESSION_MUTABLE,
        .body = CPE_WEB_POST_BODY_OPTIONAL,
        .parse_json = false,
    };
    cpe_web_post_ctx_t ctx;
    esp_err_t err = cpe_web_post_begin(req, &opts, &ctx);
    if (err != ESP_OK) {
        return err;
    }
    cpe_web_post_end(&ctx);

    if (!SMS_MQTT_SUPPORTED) {
        return cpe_web_send_error(req, "sms_mqtt_not_supported");
    }
    err = cpe_sms_mqtt_clear_cert();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not remove the CA certificate: %s", esp_err_to_name(err));
        return cpe_web_send_error(req, "persist_failed");
    }

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 64);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"sms_mqtt_ca_cleared\"}");
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}
