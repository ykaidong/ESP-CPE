/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared HTTP request/response helpers for the management web server.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "cpe_config.h"
#include "http_internal.h"

static const char *TAG = "cpe_web";

/* Overwrite sensitive buffers so credentials do not linger on the stack. */
void cpe_web_secure_wipe(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) {
        *p++ = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Response helpers                                                          */
/* ------------------------------------------------------------------------- */

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
 * importantly, skips its "purge unread body" loop (audit P1-12).
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
    return close_socket ? ESP_FAIL : ESP_OK;
}

esp_err_t cpe_web_send_error_response(httpd_req_t *req, const char *status_line,
                                      const char *code, bool close_socket)
{
    /* code is always an internal literal, safe to embed. */
    char json[96];
    snprintf(json, sizeof(json), "{\"status\":\"error\",\"error\":\"%s\"}", code);
    return cpe_web_send_json_body(req, status_line, json, close_socket);
}

/* ------------------------------------------------------------------------- */
/* Request validation helpers                                                */
/* ------------------------------------------------------------------------- */

static bool request_content_type_is_json(httpd_req_t *req)
{
    size_t len = httpd_req_get_hdr_value_len(req, "Content-Type");
    if (len == 0 || len > 64) {
        return false;
    }
    char content_type[65];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        return false;
    }
    const char *p = content_type;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (strncasecmp(p, "application/json", strlen("application/json")) != 0) {
        return false;
    }
    p += strlen("application/json");
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return (*p == '\0' || *p == ';');
}

/* Same-origin check using Origin first, then Referer. A missing header is
 * accepted only because the CSRF token is still mandatory for state changes
 * (non-browser clients such as curl do not send Origin). */
bool cpe_web_request_origin_is_same_origin(httpd_req_t *req)
{
    size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
    if (host_len == 0 || host_len >= 64) {
        return true;
    }
    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return true;
    }

    size_t origin_len = httpd_req_get_hdr_value_len(req, "Origin");
    if (origin_len > 0) {
        if (origin_len > 96) {
            return false;
        }
        char origin[97];
        if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) {
            return false;
        }
        if (strncmp(origin, "http://", 7) != 0) {
            return false;
        }
        return strcasecmp(origin + 7, host) == 0;
    }

    size_t referer_len = httpd_req_get_hdr_value_len(req, "Referer");
    if (referer_len > 0) {
        if (referer_len > 192) {
            return false;
        }
        char referer[193];
        if (httpd_req_get_hdr_value_str(req, "Referer", referer, sizeof(referer)) != ESP_OK) {
            return false;
        }
        if (strncmp(referer, "http://", 7) != 0) {
            return false;
        }
        const char *after = referer + 7;
        size_t hlen = strlen(host);
        if (strncasecmp(after, host, hlen) != 0) {
            return false;
        }
        return (after[hlen] == '\0' || after[hlen] == '/');
    }
    return true;
}

esp_err_t cpe_web_send_body_error(httpd_req_t *req, cpe_web_body_result_t result)
{
    switch (result) {
    case CPE_WEB_BODY_TOO_LARGE:
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_413_STR, "body_too_large", true);
    case CPE_WEB_BODY_TIMEOUT:
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_408_STR, "body_timeout", true);
    case CPE_WEB_BODY_EMPTY:
        return cpe_web_send_error_response(req, HTTPD_400, "empty_body", true);
    default:
        return cpe_web_send_error_response(req, HTTPD_400, "invalid_body", true);
    }
}

/*
 * Common pre-body checks. Everything here runs before authentication and
 * before a single byte of the body is consumed, which is what prevents the
 * unauthenticated content-length DoS (audit P1-12).
 */
esp_err_t cpe_web_precheck_post(httpd_req_t *req, bool require_body)
{
    if (req->content_len > CPE_WEB_MAX_JSON_BODY) {
        ESP_LOGW(TAG, "Rejecting %s: body of %u bytes exceeds limit",
                 req->uri, (unsigned)req->content_len);
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_413_STR,
                                           "body_too_large", true);
    }
    if (require_body && req->content_len == 0) {
        return cpe_web_send_error_response(req, HTTPD_400, "empty_body", true);
    }
    if (req->content_len > 0 && !request_content_type_is_json(req)) {
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_415_STR,
                                           "unsupported_media_type", true);
    }
    if (!cpe_web_request_origin_is_same_origin(req)) {
        ESP_LOGW(TAG, "Rejecting cross-origin request to %s", req->uri);
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_403_STR,
                                           "cross_origin_rejected", true);
    }
    return ESP_OK;
}

/*
 * Read the complete request body into buf with a hard size and time bound.
 * buf must be at least req->content_len + 1 bytes and the body is always NUL
 * terminated. Fragmented TCP traffic is handled by advancing the offset.
 */
cpe_web_body_result_t cpe_web_read_json_body(httpd_req_t *req, char *buf,
                                             size_t buf_len, size_t *out_len)
{
    if (req->content_len == 0) {
        return CPE_WEB_BODY_EMPTY;
    }
    if (req->content_len >= buf_len) {
        return CPE_WEB_BODY_TOO_LARGE;
    }

    size_t total = 0;
    int64_t start_us = esp_timer_get_time();
    while (total < req->content_len) {
        size_t remaining = req->content_len - total;
        int received = httpd_req_recv(req, buf + total, remaining);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (esp_timer_get_time() - start_us > CPE_WEB_BODY_READ_TIMEOUT_US) {
                return CPE_WEB_BODY_TIMEOUT;
            }
            continue;
        }
        if (received <= 0) {
            return CPE_WEB_BODY_INVALID;
        }
        total += (size_t)received;
        if (esp_timer_get_time() - start_us > CPE_WEB_BODY_READ_TIMEOUT_US) {
            return CPE_WEB_BODY_TIMEOUT;
        }
    }
    if (total != req->content_len) {
        return CPE_WEB_BODY_INVALID;
    }
    buf[total] = '\0';
    /* A raw NUL byte inside a JSON document is never valid. */
    if (memchr(buf, '\0', total) != NULL) {
        return CPE_WEB_BODY_INVALID;
    }
    *out_len = total;
    return CPE_WEB_BODY_OK;
}

/* ------------------------------------------------------------------------- */
/* Session / CSRF helpers                                                    */
/* ------------------------------------------------------------------------- */

static bool request_session_token(httpd_req_t *req, char *session, size_t session_len)
{
    if (session_len < CPE_SESSION_TOKEN_LEN) {
        return false;
    }
    size_t len = session_len;
    if (httpd_req_get_cookie_val(req, CPE_WEB_SESSION_COOKIE_NAME, session, &len) != ESP_OK) {
        return false;
    }
    return len > 0 && len < session_len;
}

static bool request_csrf_token(httpd_req_t *req, char *csrf, size_t csrf_len)
{
    size_t len = httpd_req_get_hdr_value_len(req, CPE_WEB_CSRF_HEADER_NAME);
    if (len == 0 || len >= csrf_len) {
        return false;
    }
    return httpd_req_get_hdr_value_str(req, CPE_WEB_CSRF_HEADER_NAME, csrf, csrf_len) == ESP_OK;
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    return cpe_web_send_error_response(req, CPE_WEB_HTTPD_401_STR, "unauthorized", true);
}

/**
 * @brief Require a valid session (and CSRF token for state changes).
 *
 * On failure the error response has already been sent and ESP_FAIL is
 * returned; the handler must return that value directly so the connection is
 * closed without purging an unread body.
 */
esp_err_t cpe_web_require_session(httpd_req_t *req,
                                  char session[CPE_WEB_SESSION_BUF_LEN], bool need_csrf)
{
    if (!request_session_token(req, session, CPE_WEB_SESSION_BUF_LEN) ||
            !cpe_auth_session_valid(session)) {
        return send_unauthorized(req);
    }
    if (need_csrf) {
        char csrf[CPE_WEB_CSRF_BUF_LEN];
        if (!request_csrf_token(req, csrf, sizeof(csrf)) ||
                !cpe_auth_csrf_valid(session, csrf)) {
            ESP_LOGW(TAG, "Rejected %s: missing or invalid CSRF token", req->uri);
            return cpe_web_send_error_response(req, CPE_WEB_HTTPD_403_STR,
                                               "csrf_invalid", true);
        }
    }
    return ESP_OK;
}

/* Same as require_session(), but refuses to change anything until the forced
 * first-login password change has been performed (audit P0-3). */
esp_err_t cpe_web_require_session_mutable(httpd_req_t *req,
                                          char session[CPE_WEB_SESSION_BUF_LEN])
{
    esp_err_t err = cpe_web_require_session(req, session, true);
    if (err != ESP_OK) {
        return err;
    }
    if (cpe_config_admin_must_change()) {
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_403_STR,
                                           "password_change_required", true);
    }
    return ESP_OK;
}

/*
 * Run the shared POST preamble described by opts. On failure the corresponding
 * error response has already been sent, so the caller returns the result
 * directly. cpe_web_post_end() must be called on every success path; it ends the
 * parser and wipes the credentials carried in the context.
 */
esp_err_t cpe_web_post_begin(httpd_req_t *req, const cpe_web_post_opts_t *opts,
                             cpe_web_post_ctx_t *ctx)
{
    if (req == NULL || opts == NULL || ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (opts->parse_json && opts->body != CPE_WEB_POST_BODY_REQUIRED) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));

    bool need_body = (opts->body == CPE_WEB_POST_BODY_REQUIRED);
    esp_err_t err = cpe_web_precheck_post(req, need_body);
    if (err != ESP_OK) {
        return err;
    }

    switch (opts->session) {
    case CPE_WEB_POST_SESSION_REQUIRED:
        err = cpe_web_require_session(req, ctx->session, opts->csrf);
        break;
    case CPE_WEB_POST_SESSION_MUTABLE:
        err = cpe_web_require_session_mutable(req, ctx->session);
        break;
    default:
        err = ESP_OK;
        break;
    }
    if (err != ESP_OK) {
        cpe_web_post_end(ctx);
        return err;
    }

    if (opts->body == CPE_WEB_POST_BODY_NONE ||
            (opts->body == CPE_WEB_POST_BODY_OPTIONAL && req->content_len == 0)) {
        return ESP_OK;
    }

    cpe_web_body_result_t body_result =
        cpe_web_read_json_body(req, ctx->body, sizeof(ctx->body), &ctx->body_len);
    if (body_result != CPE_WEB_BODY_OK) {
        cpe_web_post_end(ctx);
        return cpe_web_send_body_error(req, body_result);
    }
    if (!opts->parse_json) {
        return ESP_OK;
    }

    if (!cpe_web_json_depth_ok(ctx->body, ctx->body_len, CPE_WEB_JSON_MAX_DEPTH)) {
        cpe_web_post_end(ctx);
        return cpe_web_send_error_response(req, HTTPD_400, "invalid_json", true);
    }

    /* Require the document to end after the top-level value (no trailing
     * garbage), matching the previous parser's strictness. The length must
     * include the terminating NUL for that check (cJSON does the same in
     * cJSON_Parse()); the body buffer is always NUL terminated at body_len. */
    cJSON *root = cJSON_ParseWithLengthOpts(ctx->body, ctx->body_len + 1, NULL, true);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        cpe_web_post_end(ctx);
        return cpe_web_send_error_response(req, HTTPD_400, "invalid_json", true);
    }
    ctx->parser = root;
    return ESP_OK;
}

void cpe_web_post_end(cpe_web_post_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    cJSON_Delete(ctx->parser);
    cpe_web_secure_wipe(ctx, sizeof(*ctx));
}

/* httpd_resp_set_hdr() keeps the pointer, it does not copy the string, and the
 * header is only serialized when the response is sent. The HTTP server is a
 * single task, so a static buffer is safe here (a stack buffer would dangle). */
static char s_session_cookie_hdr[96];
static char s_clear_cookie_hdr[96];
static char s_csrf_cookie_hdr[96];
static char s_clear_csrf_cookie_hdr[96];

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
    int n = snprintf(s_clear_csrf_cookie_hdr, sizeof(s_clear_csrf_cookie_hdr),
                     "%s=; Path=/; SameSite=Strict; Max-Age=0",
                     CPE_WEB_CSRF_COOKIE_NAME);
    if (n > 0 && (size_t)n < sizeof(s_clear_csrf_cookie_hdr)) {
        (void)httpd_resp_set_hdr(req, "Set-Cookie", s_clear_csrf_cookie_hdr);
    }
}

esp_err_t cpe_web_clear_session_cookie(httpd_req_t *req)
{
    int n = snprintf(s_clear_cookie_hdr, sizeof(s_clear_cookie_hdr),
                     "%s=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0",
                     CPE_WEB_SESSION_COOKIE_NAME);
    if (n < 0 || (size_t)n >= sizeof(s_clear_cookie_hdr)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return httpd_resp_set_hdr(req, "Set-Cookie", s_clear_cookie_hdr);
}

/* GET handlers do not accept a body. Rejecting it up front also keeps the
 * ESP-IDF "purge unread body" loop out of reach for these handlers. */
esp_err_t cpe_web_reject_unexpected_body(httpd_req_t *req)
{
    if (req->content_len == 0) {
        return ESP_OK;
    }
    return cpe_web_send_error_response(req, HTTPD_400, "unexpected_body", true);
}

esp_err_t cpe_web_register_uris(httpd_handle_t server, const httpd_uri_t *uris,
                                size_t count)
{
    for (size_t i = 0; i < count; i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register handler %s: %s",
                     uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
