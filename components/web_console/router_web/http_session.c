/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Session/CSRF validation and the shared JSON POST preamble.
 */

#include <string.h>

#include "esp_log.h"

#include "cpe_config.h"
#include "http_internal.h"

static const char *TAG = "cpe_web";

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
