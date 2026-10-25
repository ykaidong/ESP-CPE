/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Static asset handler for the SPIFFS web console.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_log.h"

#include "http_internal.h"

static const char *TAG = "cpe_web";

#define CSP_VALUE                                                              \
    "default-src 'self'; script-src 'self'; style-src 'self'; "                \
    "img-src 'self' data:; font-src 'self'; connect-src 'self'; "              \
    "object-src 'none'; base-uri 'none'; form-action 'self'; "                 \
    "frame-ancestors 'none'"

static const char *content_type_for_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return "application/octet-stream";
    }
    if (strcasecmp(dot, ".html") == 0) {
        return "text/html";
    }
    if (strcasecmp(dot, ".js") == 0) {
        return "application/javascript";
    }
    if (strcasecmp(dot, ".css") == 0) {
        return "text/css";
    }
    if (strcasecmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(dot, ".ico") == 0) {
        return "image/x-icon";
    }
    if (strcasecmp(dot, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(dot, ".woff") == 0) {
        return "font/woff";
    }
    if (strcasecmp(dot, ".woff2") == 0) {
        return "font/woff2";
    }
    if (strcasecmp(dot, ".ttf") == 0) {
        return "font/ttf";
    }
    return "application/octet-stream";
}

/* The SPIFFS namespace is flat today, but the URI is normalized anyway so the
 * handler stays safe if the assets move to FAT/SD later. */
static bool uri_is_safe(const char *uri)
{
    if (uri == NULL || uri[0] != '/') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)uri; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7f || *p == '\\' || *p == '%' || *p == '?' || *p == '#') {
            return false;
        }
    }
    if (strstr(uri, "..") != NULL) {
        return false;
    }
    return true;
}

/* Anonymous access is limited to the page shells and their assets.
 * Everything else in the SPIFFS partition needs a session, so a file dropped
 * into the image later does not silently become world-readable. The data behind
 * the pages always comes from the authenticated /api endpoints, so serving the
 * HTML/JS/CSS anonymously leaks nothing. */
static bool path_is_public(const char *uri)
{
    if (uri == NULL || uri[0] != '/') {
        return false;
    }
    if (strcmp(uri, "/") == 0) {
        return true;
    }

    static const char *const dirs[] = { "/css/", "/js/", "/img/" };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (strncmp(uri, dirs[i], strlen(dirs[i])) == 0) {
            return true;
        }
    }

    /* Root-level pages: login.html, index.html, wlan.html, upgrade.html. */
    if (strchr(uri + 1, '/') == NULL) {
        const char *dot = strrchr(uri, '.');
        return dot != NULL && strcasecmp(dot, ".html") == 0;
    }
    return false;
}

static esp_err_t rest_common_get_handler(httpd_req_t *req)
{
    static const cpe_web_get_opts_t session_opts = {
        .session = CPE_WEB_GET_SESSION_REQUIRED,
        .csrf = false,
    };
    static const cpe_web_get_opts_t public_opts = {
        .session = CPE_WEB_GET_SESSION_NONE,
        .csrf = false,
    };
    esp_err_t gate_err = cpe_web_get_begin(req, path_is_public(req->uri) ? &public_opts
                                                                       : &session_opts, NULL);
    if (gate_err != ESP_OK) {
        return gate_err;
    }
    if (!uri_is_safe(req->uri)) {
        return cpe_web_send_error(req, "invalid_path");
    }

    cpe_web_rest_context_t *context = (cpe_web_rest_context_t *)req->user_ctx;
    if (context == NULL) {
        return cpe_web_send_error(req, "internal_error");
    }

    char filepath[CPE_WEB_FILE_PATH_MAX];
    size_t uri_len = strlen(req->uri);
    int written;
    if (uri_len > 0 && req->uri[uri_len - 1] == '/') {
        written = snprintf(filepath, sizeof(filepath), "%s%sindex.html",
                           context->base_path, req->uri);
    } else {
        written = snprintf(filepath, sizeof(filepath), "%s%s",
                           context->base_path, req->uri);
    }
    if (written < 0 || (size_t)written >= sizeof(filepath)) {
        return cpe_web_send_error(req, "uri_too_long");
    }

    int fd = open(filepath, O_RDONLY, 0);
    if (fd < 0) {
        ESP_LOGD(TAG, "Static file not found: %s", filepath);
        return cpe_web_send_error(req, "not_found");
    }

    esp_err_t err = httpd_resp_set_type(req, content_type_for_path(filepath));
    if (err == ESP_OK) {
        err = cpe_web_set_common_headers(req, false);
    }
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(req, "Content-Security-Policy", CSP_VALUE);
    }
    if (err != ESP_OK) {
        close(fd);
        ESP_LOGE(TAG, "Failed to set response headers: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    char *chunk = context->scratch;
    ssize_t read_bytes;
    do {
        read_bytes = read(fd, chunk, CPE_WEB_SCRATCH_BUFSIZE);
        if (read_bytes < 0) {
            ESP_LOGE(TAG, "Failed to read static file %s", filepath);
            close(fd);
            return ESP_FAIL;
        }
        if (read_bytes > 0) {
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                ESP_LOGW(TAG, "Client aborted static file transfer: %s", filepath);
                close(fd);
                /* Terminate the chunked response, ignore the error. */
                (void)httpd_resp_send_chunk(req, NULL, 0);
                return ESP_FAIL;
            }
        }
    } while (read_bytes > 0);
    close(fd);

    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t cpe_web_static_files_register(httpd_handle_t server,
                                        cpe_web_rest_context_t *context)
{
    httpd_uri_t static_handler = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = rest_common_get_handler,
        .user_ctx = context,
    };
    return cpe_web_register_uris(server, &static_handler, 1);
}
