/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * HTTP response construction: common headers, JSON/error bodies and cookies.
 */

#include <stdio.h>

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

esp_err_t cpe_web_send_error_response(httpd_req_t *req, const char *status_line,
                                      const char *code, bool close_socket)
{
    /* code is always an internal literal, safe to embed. */
    char json[96];
    snprintf(json, sizeof(json), "{\"status\":\"error\",\"error\":\"%s\"}", code);
    return cpe_web_send_json_body(req, status_line, json, close_socket);
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
