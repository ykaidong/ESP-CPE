/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authentication endpoints: login, logout and admin password change.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "cpe_auth.h"
#include "cpe_config.h"
#include "http_internal.h"

static const char *TAG = "cpe_web";

static esp_err_t api_login_post_handler(httpd_req_t *req)
{
    static const cpe_web_post_opts_t opts = {
        .session = CPE_WEB_POST_SESSION_NONE,
        .body = CPE_WEB_POST_BODY_REQUIRED,
        .parse_json = true,
    };
    cpe_web_post_ctx_t ctx;
    esp_err_t err = cpe_web_post_begin(req, &opts, &ctx);
    if (err != ESP_OK) {
        return err;
    }

    char username[CPE_ADMIN_USER_MAX_LEN + 1] = { 0 };
    char password[CPE_WIFI_PASS_MAX_LEN + 1] = { 0 };
    err = cpe_web_json_get_string_unescaped(ctx.parser, "username",
                                            username, sizeof(username));
    if (err == ESP_OK) {
        err = cpe_web_json_get_string_unescaped(ctx.parser, "password",
                                                password, sizeof(password));
    }
    cpe_web_post_end(&ctx);

    if (err != ESP_OK) {
        cpe_web_secure_wipe(password, sizeof(password));
        return cpe_web_send_error_response(req, HTTPD_400, "invalid_request", true);
    }

    char session[CPE_SESSION_TOKEN_LEN] = { 0 };
    char csrf[CPE_CSRF_TOKEN_LEN] = { 0 };
    bool must_change = false;
    cpe_auth_result_t result = cpe_auth_login(username, password,
                                              session, sizeof(session),
                                              csrf, sizeof(csrf), &must_change);
    cpe_web_secure_wipe(password, sizeof(password));

    switch (result) {
    case CPE_AUTH_OK: {
        if (cpe_web_set_session_cookie(req, session) != ESP_OK) {
            return cpe_web_send_error_response(req, CPE_WEB_HTTPD_500_STR,
                                               "internal_error", true);
        }
        (void)cpe_web_set_csrf_cookie(req, csrf);
        cpe_web_sbuf_t sb;
        cpe_web_sbuf_init(&sb, 128);
        cpe_web_sbuf_appendf(&sb, "{\"status\":\"ok\",\"must_change\":%s,\"csrf\":",
                             must_change ? "true" : "false");
        cpe_web_sbuf_append_json_string(&sb, csrf);
        cpe_web_sbuf_append(&sb, "}");
        ESP_LOGI(TAG, "Administrator logged in (forced password change: %s)",
                 must_change ? "yes" : "no");
        return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
    }
    case CPE_AUTH_ERR_LOCKED: {
        uint32_t remaining = cpe_auth_lockout_remaining_s();
        char retry_after[16];
        snprintf(retry_after, sizeof(retry_after), "%u", (unsigned)remaining);
        (void)httpd_resp_set_hdr(req, "Retry-After", retry_after);
        ESP_LOGW(TAG, "Login rejected: locked out for %u s", (unsigned)remaining);
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_429_STR,
                                           "locked_out", false);
    }
    case CPE_AUTH_ERR_NO_MEM:
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_500_STR,
                                           "out_of_memory", false);
    default:
        /* Do not log the attempted credential. */
        ESP_LOGW(TAG, "Login failed");
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_401_STR,
                                           "invalid_credentials", false);
    }
}

static esp_err_t api_logout_post_handler(httpd_req_t *req)
{
    /* Logout is idempotent and only logs the user out; accepting a valid
     * session without a CSRF token makes "logout" always work even if the
     * front-end lost its token (e.g. after a browser restart). */
    static const cpe_web_post_opts_t opts = {
        .session = CPE_WEB_POST_SESSION_REQUIRED,
        .body = CPE_WEB_POST_BODY_OPTIONAL,
    };
    cpe_web_post_ctx_t ctx;
    esp_err_t err = cpe_web_post_begin(req, &opts, &ctx);
    if (err != ESP_OK) {
        return err;
    }

    cpe_auth_logout(ctx.session);
    cpe_web_post_end(&ctx);
    (void)cpe_web_clear_session_cookie(req);
    cpe_web_clear_csrf_cookie(req);
    ESP_LOGI(TAG, "Administrator logged out");
    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 64);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\"}");
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

static esp_err_t api_password_post_handler(httpd_req_t *req)
{
    /* REQUIRED, not MUTABLE: this is the endpoint that clears the forced
     * password change, so it must stay usable while must_change is set. The
     * session and CSRF token are still required. */
    static const cpe_web_post_opts_t opts = {
        .session = CPE_WEB_POST_SESSION_REQUIRED,
        .csrf = true,
        .body = CPE_WEB_POST_BODY_REQUIRED,
        .parse_json = true,
    };
    cpe_web_post_ctx_t ctx;
    esp_err_t err = cpe_web_post_begin(req, &opts, &ctx);
    if (err != ESP_OK) {
        return err;
    }

    char old_password[CPE_WIFI_PASS_MAX_LEN + 1] = { 0 };
    char new_password[CPE_WIFI_PASS_MAX_LEN + 1] = { 0 };
    err = cpe_web_json_get_string_unescaped(ctx.parser, "old_password",
                                            old_password, sizeof(old_password));
    if (err == ESP_OK) {
        err = cpe_web_json_get_string_unescaped(ctx.parser, "new_password",
                                                new_password, sizeof(new_password));
    }
    cpe_web_post_end(&ctx);

    if (err != ESP_OK) {
        cpe_web_secure_wipe(old_password, sizeof(old_password));
        cpe_web_secure_wipe(new_password, sizeof(new_password));
        return cpe_web_send_error_response(req, HTTPD_400, "invalid_request", false);
    }
    if (cpe_config_validate_wifi_password(new_password) != ESP_OK) {
        cpe_web_secure_wipe(old_password, sizeof(old_password));
        cpe_web_secure_wipe(new_password, sizeof(new_password));
        return cpe_web_send_error_response(req, HTTPD_400, "weak_password", false);
    }

    err = cpe_auth_change_password(old_password, new_password);
    cpe_web_secure_wipe(old_password, sizeof(old_password));
    cpe_web_secure_wipe(new_password, sizeof(new_password));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Password change rejected: %s", esp_err_to_name(err));
        return cpe_web_send_error_response(req, HTTPD_400,
                                           "password_change_failed", false);
    }

    /* cpe_auth_change_password() invalidated every session, including the
     * current one; clear the cookie on the client as well. */
    (void)cpe_web_clear_session_cookie(req);
    cpe_web_clear_csrf_cookie(req);
    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 64);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"password_changed\"}");
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

esp_err_t cpe_web_api_auth_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/login",    .method = HTTP_POST, .handler = api_login_post_handler },
        { .uri = "/api/logout",   .method = HTTP_POST, .handler = api_logout_post_handler },
        { .uri = "/api/password", .method = HTTP_POST, .handler = api_password_post_handler },
    };
    return cpe_web_register_uris(server, uris, sizeof(uris) / sizeof(uris[0]));
}
