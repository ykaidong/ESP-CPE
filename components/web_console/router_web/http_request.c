/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * HTTP request validation and bounded body reading.
 */

#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "http_internal.h"

static const char *TAG = "cpe_web";

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
        return cpe_web_send_error(req, "body_too_large");
    case CPE_WEB_BODY_TIMEOUT:
        return cpe_web_send_error(req, "body_timeout");
    case CPE_WEB_BODY_EMPTY:
        return cpe_web_send_error(req, "empty_body");
    default:
        return cpe_web_send_error(req, "invalid_body");
    }
}

/*
 * Common pre-body checks. Everything here runs before authentication and
 * before a single byte of the body is consumed, which is what prevents the
 * unauthenticated content-length DoS.
 */
esp_err_t cpe_web_precheck_post(httpd_req_t *req, bool require_body)
{
    if (req->content_len > CPE_WEB_MAX_JSON_BODY) {
        ESP_LOGW(TAG, "Rejecting %s: body of %u bytes exceeds limit",
                 req->uri, (unsigned)req->content_len);
        return cpe_web_send_error(req, "body_too_large");
    }
    if (require_body && req->content_len == 0) {
        return cpe_web_send_error(req, "empty_body");
    }
    if (req->content_len > 0 && !request_content_type_is_json(req)) {
        return cpe_web_send_error(req, "unsupported_media_type");
    }
    if (!cpe_web_request_origin_is_same_origin(req)) {
        ESP_LOGW(TAG, "Rejecting cross-origin request to %s", req->uri);
        return cpe_web_send_error(req, "cross_origin_rejected");
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

/* GET handlers do not accept a body. Rejecting it up front also keeps the
 * ESP-IDF "purge unread body" loop out of reach for these handlers. */
esp_err_t cpe_web_reject_unexpected_body(httpd_req_t *req)
{
    if (req->content_len == 0) {
        return ESP_OK;
    }
    return cpe_web_send_error(req, "unexpected_body");
}
