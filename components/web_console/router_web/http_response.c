/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * HTTP response construction: common headers, JSON/error bodies and cookies.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "http_internal.h"

static const char *TAG = "cpe_web";

esp_err_t cpe_web_set_common_headers(httpd_req_t *req, bool no_store)
{
    esp_err_t err = httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    }
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    }
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(req, "Cache-Control", no_store ? "no-store" : "no-cache");
    }
    return err;
}

/*
 * Send a JSON response. close_socket == true is implemented by returning
 * ESP_FAIL on purpose: ESP-IDF then closes the session immediately and, most
 * importantly, skips its "purge unread body" loop.
 */
esp_err_t cpe_web_send_json_body(httpd_req_t *req, const char *status_line,
                                 const char *json, bool close_socket)
{
    esp_err_t err = httpd_resp_set_status(req, status_line);
    if (err == ESP_OK) {
        err = httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    }
    if (err == ESP_OK) {
        err = cpe_web_set_common_headers(req, true);
    }
    if (err == ESP_OK && close_socket) {
        err = httpd_resp_set_hdr(req, "Connection", "close");
    }
    if (err == ESP_OK) {
        err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send HTTP response: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    return close_socket ? CPE_WEB_CLOSE_SOCKET : ESP_OK;
}

static esp_err_t send_error_response(httpd_req_t *req, const char *status_line,
                                     const char *code, bool close_socket)
{
    /* code is always an internal literal, safe to embed. */
    char json[96];
    snprintf(json, sizeof(json), "{\"status\":\"error\",\"error\":\"%s\"}", code);
    return cpe_web_send_json_body(req, status_line, json, close_socket);
}

/* One row per error the console answers with. The HTTP status and the
 * connection policy both follow from the error itself, so a call site cannot
 * answer the same problem two different ways, and a new error has to be
 * declared here instead of picking a status at random.
 *
 * close_socket is only true where the request was rejected before its body was
 * consumed (or the upload stream broke): dropping the connection makes the HTTP
 * server skip its "purge unread body" loop, which is the whole point. A request
 * that was read, parsed and answered - including every server side failure -
 * keeps the connection for the next one.
 *
 * The codes are part of the contract with the front-end: ERROR_TEXT in
 * spiffs/js/api.js maps each of them to a Chinese message. */
typedef struct {
    const char *code;
    const char *status;
    bool close_socket;
} cpe_web_error_t;

static const cpe_web_error_t ERROR_SPECS[] = {
    /* Rejected before the request was consumed. */
    { "empty_body",                HTTPD_400,              true  },
    { "invalid_body",              HTTPD_400,              true  },
    { "invalid_json",              HTTPD_400,              true  },
    { "unexpected_body",           HTTPD_400,              true  },
    { "body_timeout",              CPE_WEB_HTTPD_408_STR,  true  },
    { "unauthorized",              CPE_WEB_HTTPD_401_STR,  true  },
    { "csrf_invalid",              CPE_WEB_HTTPD_403_STR,  true  },
    { "password_change_required",  CPE_WEB_HTTPD_403_STR,  true  },
    { "cross_origin_rejected",     CPE_WEB_HTTPD_403_STR,  true  },
    { "body_too_large",            CPE_WEB_HTTPD_413_STR,  true  },
    { "unsupported_media_type",    CPE_WEB_HTTPD_415_STR,  true  },

    /* Interrupted firmware upload: the stream cannot be resumed. */
    { "upload_timeout",            CPE_WEB_HTTPD_408_STR,  true  },
    { "upload_interrupted",        HTTPD_400,              true  },
    { "invalid_firmware_image",    HTTPD_400,              true  },
    { "image_too_large",           CPE_WEB_HTTPD_413_STR,  true  },
    { "pending_verify",            CPE_WEB_HTTPD_409_STR,  true  },
    { "ota_busy",                  CPE_WEB_HTTPD_409_STR,  true  },
    { "ota_begin_failed",          CPE_WEB_HTTPD_500_STR,  true  },
    { "ota_write_failed",          CPE_WEB_HTTPD_500_STR,  true  },
    { "ota_finalize_failed",       CPE_WEB_HTTPD_500_STR,  true  },
    { "out_of_memory",             CPE_WEB_HTTPD_500_STR,  true  },

    /* The request was read and understood, only its content is wrong. */
    { "invalid_request",           HTTPD_400,              false },
    { "invalid_request_body",      HTTPD_400,              false },
    { "invalid_ssid",              HTTPD_400,              false },
    { "invalid_password",          HTTPD_400,              false },
    { "invalid_device_name",       HTTPD_400,              false },
    { "invalid_max_sta",           HTTPD_400,              false },
    { "invalid_channel",           HTTPD_400,              false },
    { "invalid_bandwidth",         HTTPD_400,              false },
    { "invalid_mac",               HTTPD_400,              false },
    { "invalid_station_name",      HTTPD_400,              false },
    { "invalid_maintenance_period", HTTPD_400,             false },
    { "no_fields",                 HTTPD_400,              false },
    { "weak_password",             HTTPD_400,              false },
    { "invalid_path",              HTTPD_400,              false },
    { "invalid_credentials",       CPE_WEB_HTTPD_401_STR,  false },
    { "locked_out",                CPE_WEB_HTTPD_429_STR,  false },
    { "station_not_found",         CPE_WEB_HTTPD_404_STR,  false },
    { "not_found",                 CPE_WEB_HTTPD_404_STR,  false },
    { "uri_too_long",              CPE_WEB_HTTPD_414_STR,  false },

    /* Server side failures: answered normally, the connection is still good. */
    { "internal_error",            CPE_WEB_HTTPD_500_STR,  false },
    { "config_unavailable",        CPE_WEB_HTTPD_500_STR,  false },
    { "persist_failed",            CPE_WEB_HTTPD_500_STR,  false },
    { "factory_reset_failed",      CPE_WEB_HTTPD_500_STR,  false },
    { "deauth_failed",             CPE_WEB_HTTPD_500_STR,  false },
    { "ota_info_failed",           CPE_WEB_HTTPD_500_STR,  false },
};

esp_err_t cpe_web_send_error(httpd_req_t *req, const char *code)
{
    if (code != NULL) {
        for (size_t i = 0; i < sizeof(ERROR_SPECS) / sizeof(ERROR_SPECS[0]); i++) {
            if (strcmp(code, ERROR_SPECS[i].code) == 0) {
                return send_error_response(req, ERROR_SPECS[i].status,
                                           ERROR_SPECS[i].code,
                                           ERROR_SPECS[i].close_socket);
            }
        }
    }
    /* Not in the table: a typo must be loud, not silently served as 400. */
    ESP_LOGE(TAG, "Unknown error code \"%s\", answering 500", code ? code : "(null)");
    return send_error_response(req, CPE_WEB_HTTPD_500_STR, "internal_error", false);
}

/* httpd_resp_set_hdr() keeps the pointer, it does not copy the string, and the
 * header is only serialized when the response is sent. The HTTP server is a
 * single task, so a static buffer is safe here (a stack buffer would dangle).
 * set/clear share one buffer per cookie: a response never both sets and clears
 * the same cookie. */
static char s_session_cookie_hdr[96];
static char s_csrf_cookie_hdr[96];

esp_err_t cpe_web_set_session_cookie(httpd_req_t *req, const char *session)
{
    /* Secure is intentionally absent: the management plane is HTTP only at
     * this stage (see IMPLEMENTATION_PLAN.md phase 3). HttpOnly keeps the
     * token away from JavaScript; SameSite=Strict blocks cross-site use. */
    int n = snprintf(s_session_cookie_hdr, sizeof(s_session_cookie_hdr),
                     "%s=%s; Path=/; HttpOnly; SameSite=Strict",
                     CPE_WEB_SESSION_COOKIE_NAME, session);
    if (n < 0 || (size_t)n >= sizeof(s_session_cookie_hdr)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return httpd_resp_set_hdr(req, "Set-Cookie", s_session_cookie_hdr);
}

/* The CSRF token is also stored in a non-HttpOnly, SameSite=Strict cookie so
 * the front-end can read it after a reload or in a new tab (sessionStorage is
 * per-tab and was the reason "logout"/"change password" silently failed with
 * 403 in a fresh tab). Double-submit: the server still validates the header
 * against the token bound to the session cookie. */
esp_err_t cpe_web_set_csrf_cookie(httpd_req_t *req, const char *csrf)
{
    int n = snprintf(s_csrf_cookie_hdr, sizeof(s_csrf_cookie_hdr),
                     "%s=%s; Path=/; SameSite=Strict",
                     CPE_WEB_CSRF_COOKIE_NAME, csrf);
    if (n < 0 || (size_t)n >= sizeof(s_csrf_cookie_hdr)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return httpd_resp_set_hdr(req, "Set-Cookie", s_csrf_cookie_hdr);
}

void cpe_web_clear_csrf_cookie(httpd_req_t *req)
{
    int n = snprintf(s_csrf_cookie_hdr, sizeof(s_csrf_cookie_hdr),
                     "%s=; Path=/; SameSite=Strict; Max-Age=0",
                     CPE_WEB_CSRF_COOKIE_NAME);
    if (n > 0 && (size_t)n < sizeof(s_csrf_cookie_hdr)) {
        (void)httpd_resp_set_hdr(req, "Set-Cookie", s_csrf_cookie_hdr);
    }
}

esp_err_t cpe_web_clear_session_cookie(httpd_req_t *req)
{
    int n = snprintf(s_session_cookie_hdr, sizeof(s_session_cookie_hdr),
                     "%s=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0",
                     CPE_WEB_SESSION_COOKIE_NAME);
    if (n < 0 || (size_t)n >= sizeof(s_session_cookie_hdr)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return httpd_resp_set_hdr(req, "Set-Cookie", s_session_cookie_hdr);
}
