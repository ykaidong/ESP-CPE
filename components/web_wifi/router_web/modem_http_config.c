/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Embedded web management server for the ESP-CPE product.
 *
 * This file replaces the upstream HTTP Basic example with a session based
 * management interface. The security fixes implemented here map to the audit
 * report as follows:
 *
 *  - P0-3   : no compile time credentials, no HTTP Basic, session cookie
 *             (CPE_SID, HttpOnly + SameSite=Strict) + X-CSRF-Token, forced
 *             password change on first login.
 *  - P1-12  : Content-Length is validated before the body is read, oversized
 *             bodies get 413 and the connection is closed, 401 responses use
 *             Connection: close, body reads are bounded in size and time.
 *  - P1-13  : every Wi-Fi/device parameter is validated with cpe_config helpers
 *             before it reaches the driver/NVS, and a failed apply is rolled
 *             back.
 *  - P1-14  : CSRF token, JSON Content-Type, Origin/Referer validation and all
 *             wildcard CORS headers removed.
 *  - P1-15  : passwords are never echoed in GET responses or logs, responses
 *             carry Cache-Control: no-store.
 *  - P1-16  : station names are whitelisted server side, JSON-escaped on
 *             output, and the frontend renders them with textContent only.
 *  - P1-17  : the station list is protected by a mutex created before any
 *             event handler/HTTP handler is registered, SLIST_FOREACH_SAFE is
 *             used for removal and handlers serialize a snapshot.
 *  - 6.4    : a single read_json_body() helper, json_parse_end() on every path,
 *             JSON output escaping, no ESP_ERROR_CHECK around network I/O,
 *             static path normalization, login lockout delegated to cpe_auth.
 */

#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <sys/param.h>
#include <sys/queue.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "esp_wifi.h"
#include "json_parser.h"
#include "nvs.h"

#include "app_wifi.h"
#include "cpe_auth.h"
#include "cpe_config.h"
#include "cpe_ota.h"
#include "cpe_status.h"
#include "modem_http_config.h"

static const char *TAG = "cpe_web";

/* Set once modem_http_start() completed; guards event handlers that may race
 * with modem_http_stop(). */
static bool s_started;
static bool s_events_registered;

/* ------------------------------------------------------------------------- */
/* Constants                                                                 */
/* ------------------------------------------------------------------------- */

#define SESSION_COOKIE_NAME         "CPE_SID"
#define CSRF_COOKIE_NAME            "CPE_CSRF"
#define CSRF_HEADER_NAME            "X-CSRF-Token"

#define SESSION_BUF_LEN             (CPE_SESSION_TOKEN_LEN + 8)
#define CSRF_BUF_LEN                (CPE_CSRF_TOKEN_LEN + 8)

/* Maximum accepted JSON request body (audit P1-12). */
#define MAX_JSON_BODY               2048
/* Upper bound for the whole body read, regardless of the socket timeouts. */
#define BODY_READ_TIMEOUT_US        (2 * 1000 * 1000)
/* json_parser's json_skip_elem() is recursive (audit R-12); reject deeply
 * nested documents before handing them to the parser. */
#define JSON_MAX_DEPTH              6

#define STA_NAME_MAX_LEN            32
#define STA_NAME_NVS_NAMESPACE      "sta_names"
#define STA_SNAPSHOT_MAX            10

#define FILE_PATH_MAX               (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE             10240

#define HTTPD_401_STR               "401 Unauthorized"
#define HTTPD_403_STR               "403 Forbidden"
#define HTTPD_404_STR               "404 Not Found"
#define HTTPD_408_STR               "408 Request Timeout"
#define HTTPD_409_STR               "409 Conflict"
#define HTTPD_413_STR               "413 Content Too Large"
#define HTTPD_414_STR               "414 URI Too Long"
#define HTTPD_415_STR               "415 Unsupported Media Type"
#define HTTPD_429_STR               "429 Too Many Requests"
#define HTTPD_500_STR               "500 Internal Server Error"

#define CSP_VALUE                                                              \
    "default-src 'self'; script-src 'self'; style-src 'self'; "                \
    "img-src 'self' data:; font-src 'self'; connect-src 'self'; "              \
    "object-src 'none'; base-uri 'none'; form-action 'self'; "                 \
    "frame-ancestors 'none'"

typedef enum {
    BODY_OK = 0,
    BODY_EMPTY,
    BODY_TOO_LARGE,
    BODY_TIMEOUT,
    BODY_INVALID,
} body_result_t;

typedef struct {
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

/* ------------------------------------------------------------------------- */
/* Small generic helpers                                                     */
/* ------------------------------------------------------------------------- */

/* Overwrite sensitive buffers so credentials do not linger on the stack. */
static void secure_wipe(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) {
        *p++ = 0;
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void mac_to_hex(const uint8_t mac[6], char out[13])
{
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x",
             (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
             (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
}

static void mac_to_colon(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
             (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
}

/* Strict MAC parser: accepts "aa:bb:cc:dd:ee:ff" or "aabbccddeeff". */
static bool mac_parse(const char *str, uint8_t mac[6])
{
    if (str == NULL) {
        return false;
    }
    size_t len = strlen(str);
    if (len == 17) {
        for (int i = 0; i < 6; i++) {
            int hi = hex_nibble(str[i * 3]);
            int lo = hex_nibble(str[i * 3 + 1]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            if (i < 5 && str[i * 3 + 2] != ':') {
                return false;
            }
            mac[i] = (uint8_t)((hi << 4) | lo);
        }
        return true;
    }
    if (len == 12) {
        for (int i = 0; i < 6; i++) {
            int hi = hex_nibble(str[i * 2]);
            int lo = hex_nibble(str[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            mac[i] = (uint8_t)((hi << 4) | lo);
        }
        return true;
    }
    return false;
}

/* Whitelist for the user visible station name (audit P1-16). */
static bool station_name_valid(const char *name)
{
    if (name == NULL) {
        return false;
    }
    size_t len = strlen(name);
    if (len == 0 || len > STA_NAME_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '.' ||
                c == '@' || c == '(' || c == ')') {
            continue;
        }
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Growable string buffer with JSON escaping                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} sbuf_t;

static void sbuf_init(sbuf_t *sb, size_t cap)
{
    sb->data = calloc(1, cap);
    sb->len = 0;
    sb->cap = sb->data ? cap : 0;
    sb->failed = (sb->data == NULL);
}

static void sbuf_free(sbuf_t *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->failed = true;
}

static void sbuf_appendf(sbuf_t *sb, const char *fmt, ...)
{
    if (sb->failed) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        sb->failed = true;
        return;
    }
    size_t required = sb->len + (size_t)need + 1;
    if (required > sb->cap) {
        size_t new_cap = sb->cap ? sb->cap : 128;
        while (new_cap < required) {
            new_cap *= 2;
        }
        char *grown = realloc(sb->data, new_cap);
        if (grown == NULL) {
            va_end(ap2);
            sb->failed = true;
            return;
        }
        sb->data = grown;
        sb->cap = new_cap;
    }
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)need;
}

static void sbuf_append(sbuf_t *sb, const char *str)
{
    sbuf_appendf(sb, "%s", str);
}

/* Append a JSON string literal with all mandatory escapes (P1-16 / 6.4). */
static void sbuf_append_json_string(sbuf_t *sb, const char *src)
{
    if (sb->failed) {
        return;
    }
    sbuf_append(sb, "\"");
    if (src != NULL) {
        for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
            switch (*p) {
            case '"':
                sbuf_append(sb, "\\\"");
                break;
            case '\\':
                sbuf_append(sb, "\\\\");
                break;
            case '\b':
                sbuf_append(sb, "\\b");
                break;
            case '\f':
                sbuf_append(sb, "\\f");
                break;
            case '\n':
                sbuf_append(sb, "\\n");
                break;
            case '\r':
                sbuf_append(sb, "\\r");
                break;
            case '\t':
                sbuf_append(sb, "\\t");
                break;
            default:
                if (*p < 0x20 || *p == 0x7f) {
                    sbuf_appendf(sb, "\\u%04x", (unsigned)*p);
                } else {
                    sbuf_appendf(sb, "%c", (char)*p);
                }
                break;
            }
        }
    }
    sbuf_append(sb, "\"");
}

/* Decode the raw token bytes returned by json_parser (it does not unescape).
 * Only printable ASCII escapes are accepted; \uXXXX is rejected because the
 * validated values are ASCII only and decoding it could hide control chars. */
static esp_err_t json_unescape(const char *src, char *dst, size_t dst_len)
{
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\\') {
            char esc = src[++i];
            switch (esc) {
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case '/':
                c = '/';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            default:
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (out + 1 >= dst_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        dst[out++] = (char)c;
    }
    dst[out] = '\0';
    return ESP_OK;
}

/* Reject deeply nested documents before calling the recursive parser. */
static bool json_depth_ok(const char *body, size_t len, int max_depth)
{
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < len; i++) {
        char c = body[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            if (++depth > max_depth) {
                return false;
            }
        } else if (c == '}' || c == ']') {
            if (depth > 0) {
                depth--;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* HTTP response helpers                                                     */
/* ------------------------------------------------------------------------- */

static esp_err_t set_common_headers(httpd_req_t *req, bool no_store)
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
static esp_err_t send_json_body(httpd_req_t *req, const char *status_line,
                                const char *json, bool close_socket)
{
    esp_err_t err = httpd_resp_set_status(req, status_line);
    if (err == ESP_OK) {
        err = httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    }
    if (err == ESP_OK) {
        err = set_common_headers(req, true);
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

static esp_err_t send_error_response(httpd_req_t *req, const char *status_line,
                                     const char *code, bool close_socket)
{
    /* code is always an internal literal, safe to embed. */
    char json[96];
    snprintf(json, sizeof(json), "{\"status\":\"error\",\"error\":\"%s\"}", code);
    return send_json_body(req, status_line, json, close_socket);
}

static esp_err_t send_sbuf_json(httpd_req_t *req, const char *status_line,
                                sbuf_t *sb, bool close_socket)
{
    if (sb->failed || sb->data == NULL) {
        sbuf_free(sb);
        return send_error_response(req, HTTPD_500_STR, "out_of_memory", true);
    }
    esp_err_t ret = send_json_body(req, status_line, sb->data, close_socket);
    sbuf_free(sb);
    return ret;
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
static bool request_origin_is_same_origin(httpd_req_t *req)
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

static esp_err_t send_body_error(httpd_req_t *req, body_result_t result)
{
    switch (result) {
    case BODY_TOO_LARGE:
        return send_error_response(req, HTTPD_413_STR, "body_too_large", true);
    case BODY_TIMEOUT:
        return send_error_response(req, HTTPD_408_STR, "body_timeout", true);
    case BODY_EMPTY:
        return send_error_response(req, HTTPD_400, "empty_body", true);
    default:
        return send_error_response(req, HTTPD_400, "invalid_body", true);
    }
}

/*
 * Common pre-body checks. Everything here runs before authentication and
 * before a single byte of the body is consumed, which is what prevents the
 * unauthenticated content-length DoS (audit P1-12).
 */
static esp_err_t precheck_post(httpd_req_t *req, bool require_body)
{
    if (req->content_len > MAX_JSON_BODY) {
        ESP_LOGW(TAG, "Rejecting %s: body of %u bytes exceeds limit",
                 req->uri, (unsigned)req->content_len);
        return send_error_response(req, HTTPD_413_STR, "body_too_large", true);
    }
    if (require_body && req->content_len == 0) {
        return send_error_response(req, HTTPD_400, "empty_body", true);
    }
    if (req->content_len > 0 && !request_content_type_is_json(req)) {
        return send_error_response(req, HTTPD_415_STR, "unsupported_media_type", true);
    }
    if (!request_origin_is_same_origin(req)) {
        ESP_LOGW(TAG, "Rejecting cross-origin request to %s", req->uri);
        return send_error_response(req, HTTPD_403_STR, "cross_origin_rejected", true);
    }
    return ESP_OK;
}

/*
 * Read the complete request body into buf with a hard size and time bound.
 * buf must be at least req->content_len + 1 bytes and the body is always NUL
 * terminated. Fragmented TCP traffic is handled by advancing the offset.
 */
static body_result_t read_json_body(httpd_req_t *req, char *buf, size_t buf_len, size_t *out_len)
{
    if (req->content_len == 0) {
        return BODY_EMPTY;
    }
    if (req->content_len >= buf_len) {
        return BODY_TOO_LARGE;
    }

    size_t total = 0;
    int64_t start_us = esp_timer_get_time();
    while (total < req->content_len) {
        size_t remaining = req->content_len - total;
        int received = httpd_req_recv(req, buf + total, remaining);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (esp_timer_get_time() - start_us > BODY_READ_TIMEOUT_US) {
                return BODY_TIMEOUT;
            }
            continue;
        }
        if (received <= 0) {
            return BODY_INVALID;
        }
        total += (size_t)received;
        if (esp_timer_get_time() - start_us > BODY_READ_TIMEOUT_US) {
            return BODY_TIMEOUT;
        }
    }
    if (total != req->content_len) {
        return BODY_INVALID;
    }
    buf[total] = '\0';
    /* A raw NUL byte inside a JSON document is never valid. */
    if (memchr(buf, '\0', total) != NULL) {
        return BODY_INVALID;
    }
    *out_len = total;
    return BODY_OK;
}

/* ------------------------------------------------------------------------- */
/* JSON field helpers                                                        */
/* ------------------------------------------------------------------------- */

static esp_err_t json_get_string_unescaped(jparse_ctx_t *jctx, const char *name,
                                           char *out, size_t out_len)
{
    char raw[160];
    if (out_len == 0 || out_len > sizeof(raw)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (json_obj_get_string(jctx, name, raw, sizeof(raw)) != OS_SUCCESS) {
        /* Distinguish a missing field from a present but malformed/oversized
         * value: callers must reject the latter with 400 (audit P1-13), not
         * silently ignore it. */
        int len = 0;
        if (json_obj_get_strlen(jctx, name, &len) == OS_SUCCESS) {
            return ESP_ERR_INVALID_SIZE;
        }
        int ival = 0;
        if (json_obj_get_int(jctx, name, &ival) == OS_SUCCESS) {
            return ESP_ERR_INVALID_ARG;
        }
        float fval = 0;
        if (json_obj_get_float(jctx, name, &fval) == OS_SUCCESS) {
            return ESP_ERR_INVALID_ARG;
        }
        bool bval = false;
        if (json_obj_get_bool(jctx, name, &bval) == OS_SUCCESS) {
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_ERR_NOT_FOUND;
    }
    return json_unescape(raw, out, out_len);
}

/* Accept both a JSON number and a legacy JSON string ("6"). */
static esp_err_t json_get_int_flexible(jparse_ctx_t *jctx, const char *name,
                                       int *out, bool *present)
{
    *present = false;
    int value;
    if (json_obj_get_int(jctx, name, &value) == OS_SUCCESS) {
        *present = true;
        *out = value;
        return ESP_OK;
    }
    char str[16];
    if (json_obj_get_string(jctx, name, str, sizeof(str)) == OS_SUCCESS) {
        *present = true;
        char *end = NULL;
        long long parsed = strtoll(str, &end, 10);
        if (end == str || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        *out = (int)parsed;
        return ESP_OK;
    }
    /* Present but not an integer (e.g. 1.5 or true): reject instead of
     * pretending the field was absent. */
    float fval = 0;
    if (json_obj_get_float(jctx, name, &fval) == OS_SUCCESS) {
        *present = true;
        return ESP_ERR_INVALID_ARG;
    }
    bool bval = false;
    if (json_obj_get_bool(jctx, name, &bval) == OS_SUCCESS) {
        *present = true;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* Accept JSON boolean and the legacy "true"/"false"/"1"/"0" strings. */
static esp_err_t json_get_bool_flexible(jparse_ctx_t *jctx, const char *name,
                                        bool *out, bool *present)
{
    *present = false;
    bool value;
    if (json_obj_get_bool(jctx, name, &value) == OS_SUCCESS) {
        *present = true;
        *out = value;
        return ESP_OK;
    }
    char str[8];
    if (json_obj_get_string(jctx, name, str, sizeof(str)) == OS_SUCCESS) {
        *present = true;
        if (strcmp(str, "true") == 0 || strcmp(str, "1") == 0) {
            *out = true;
            return ESP_OK;
        }
        if (strcmp(str, "false") == 0 || strcmp(str, "0") == 0) {
            *out = false;
            return ESP_OK;
        }
        return ESP_ERR_INVALID_ARG;
    }
    /* Legacy clients may send 0/1 as a JSON number. */
    int ival = 0;
    if (json_obj_get_int(jctx, name, &ival) == OS_SUCCESS) {
        *present = true;
        if (ival == 0 || ival == 1) {
            *out = (ival != 0);
            return ESP_OK;
        }
        return ESP_ERR_INVALID_ARG;
    }
    float fval = 0;
    if (json_obj_get_float(jctx, name, &fval) == OS_SUCCESS) {
        *present = true;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
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
    if (httpd_req_get_cookie_val(req, SESSION_COOKIE_NAME, session, &len) != ESP_OK) {
        return false;
    }
    return len > 0 && len < session_len;
}

static bool request_csrf_token(httpd_req_t *req, char *csrf, size_t csrf_len)
{
    size_t len = httpd_req_get_hdr_value_len(req, CSRF_HEADER_NAME);
    if (len == 0 || len >= csrf_len) {
        return false;
    }
    return httpd_req_get_hdr_value_str(req, CSRF_HEADER_NAME, csrf, csrf_len) == ESP_OK;
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    return send_error_response(req, HTTPD_401_STR, "unauthorized", true);
}

/**
 * @brief Require a valid session (and CSRF token for state changes).
 *
 * On failure the error response has already been sent and ESP_FAIL is
 * returned; the handler must return that value directly so the connection is
 * closed without purging an unread body.
 */
static esp_err_t require_session(httpd_req_t *req, char session[SESSION_BUF_LEN], bool need_csrf)
{
    if (!request_session_token(req, session, SESSION_BUF_LEN) ||
            !cpe_auth_session_valid(session)) {
        return send_unauthorized(req);
    }
    if (need_csrf) {
        char csrf[CSRF_BUF_LEN];
        if (!request_csrf_token(req, csrf, sizeof(csrf)) ||
                !cpe_auth_csrf_valid(session, csrf)) {
            ESP_LOGW(TAG, "Rejected %s: missing or invalid CSRF token", req->uri);
            return send_error_response(req, HTTPD_403_STR, "csrf_invalid", true);
        }
    }
    return ESP_OK;
}

/* Same as require_session(), but refuses to change anything until the forced
 * first-login password change has been performed (audit P0-3). */
static esp_err_t require_session_mutable(httpd_req_t *req, char session[SESSION_BUF_LEN])
{
    esp_err_t err = require_session(req, session, true);
    if (err != ESP_OK) {
        return err;
    }
    if (cpe_config_admin_must_change()) {
        return send_error_response(req, HTTPD_403_STR, "password_change_required", true);
    }
    return ESP_OK;
}

/* httpd_resp_set_hdr() keeps the pointer, it does not copy the string, and the
 * header is only serialized when the response is sent. The HTTP server is a
 * single task, so a static buffer is safe here (a stack buffer would dangle). */
static char s_session_cookie_hdr[96];
static char s_clear_cookie_hdr[96];
static char s_csrf_cookie_hdr[96];
static char s_clear_csrf_cookie_hdr[96];

static esp_err_t set_session_cookie(httpd_req_t *req, const char *session)
{
    /* Secure is intentionally absent: the management plane is HTTP only at
     * this stage (see IMPLEMENTATION_PLAN.md phase 3). HttpOnly keeps the
     * token away from JavaScript; SameSite=Strict blocks cross-site use. */
    int n = snprintf(s_session_cookie_hdr, sizeof(s_session_cookie_hdr),
                     "%s=%s; Path=/; HttpOnly; SameSite=Strict",
                     SESSION_COOKIE_NAME, session);
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
static esp_err_t set_csrf_cookie(httpd_req_t *req, const char *csrf)
{
    int n = snprintf(s_csrf_cookie_hdr, sizeof(s_csrf_cookie_hdr),
                     "%s=%s; Path=/; SameSite=Strict",
                     CSRF_COOKIE_NAME, csrf);
    if (n < 0 || (size_t)n >= sizeof(s_csrf_cookie_hdr)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return httpd_resp_set_hdr(req, "Set-Cookie", s_csrf_cookie_hdr);
}

static void clear_csrf_cookie(httpd_req_t *req)
{
    int n = snprintf(s_clear_csrf_cookie_hdr, sizeof(s_clear_csrf_cookie_hdr),
                     "%s=; Path=/; SameSite=Strict; Max-Age=0",
                     CSRF_COOKIE_NAME);
    if (n > 0 && (size_t)n < sizeof(s_clear_csrf_cookie_hdr)) {
        (void)httpd_resp_set_hdr(req, "Set-Cookie", s_clear_csrf_cookie_hdr);
    }
}

static esp_err_t clear_session_cookie(httpd_req_t *req)
{
    int n = snprintf(s_clear_cookie_hdr, sizeof(s_clear_cookie_hdr),
                     "%s=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0",
                     SESSION_COOKIE_NAME);
    if (n < 0 || (size_t)n >= sizeof(s_clear_cookie_hdr)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return httpd_resp_set_hdr(req, "Set-Cookie", s_clear_cookie_hdr);
}

/* GET handlers do not accept a body. Rejecting it up front also keeps the
 * ESP-IDF "purge unread body" loop out of reach for these handlers. */
static esp_err_t reject_unexpected_body(httpd_req_t *req)
{
    if (req->content_len == 0) {
        return ESP_OK;
    }
    return send_error_response(req, HTTPD_400, "unexpected_body", true);
}

/* ------------------------------------------------------------------------- */
/* Station list (audit P1-17)                                                */
/* ------------------------------------------------------------------------- */

typedef struct sta_node {
    SLIST_ENTRY(sta_node) field;
    uint8_t mac[6];
    char name[STA_NAME_MAX_LEN + 1];
    esp_ip4_addr_t ip;
    int64_t start_time;
} sta_node_t;

static SLIST_HEAD(sta_list_head, sta_node) s_sta_list = SLIST_HEAD_INITIALIZER(s_sta_list);
static SemaphoreHandle_t s_sta_mutex;

#define STA_LOCK_TIMEOUT_MS     100

static esp_err_t station_name_load(const uint8_t mac[6], char *out, size_t out_len)
{
    char key[13];
    mac_to_hex(mac, key);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STA_NAME_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = out_len;
    err = nvs_get_str(handle, key, out, &len);
    nvs_close(handle);
    if (err == ESP_OK && !station_name_valid(out)) {
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

static esp_err_t station_name_save(const uint8_t mac[6], const char *name)
{
    char key[13];
    mac_to_hex(mac, key);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STA_NAME_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, key, name);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void sta_add_node(const uint8_t mac[6])
{
    if (s_sta_mutex == NULL) {
        return;
    }
    sta_node_t *node = calloc(1, sizeof(*node));
    if (node == NULL) {
        ESP_LOGE(TAG, "Out of memory while adding station");
        return;
    }
    memcpy(node->mac, mac, sizeof(node->mac));
    node->start_time = esp_timer_get_time();
    if (station_name_load(mac, node->name, sizeof(node->name)) != ESP_OK) {
        mac_to_hex(mac, node->name);
    }

    /* Best effort IP lookup: the DHCP lease may already exist. */
    esp_netif_t *ap = app_wifi_get_ap_netif();
    if (ap != NULL) {
        esp_netif_pair_mac_ip_t pair = { 0 };
        memcpy(pair.mac, mac, sizeof(pair.mac));
        if (esp_netif_dhcps_get_clients_by_mac(ap, 1, &pair) == ESP_OK) {
            node->ip = pair.ip;
        }
    }

    if (xSemaphoreTake(s_sta_mutex, pdMS_TO_TICKS(STA_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Station list lock timeout while adding");
        free(node);
        return;
    }
    sta_node_t *it;
    SLIST_FOREACH(it, &s_sta_list, field) {
        if (memcmp(it->mac, mac, sizeof(it->mac)) == 0) {
            /* Already tracked (duplicate event), keep the existing node. */
            xSemaphoreGive(s_sta_mutex);
            free(node);
            return;
        }
    }
    SLIST_INSERT_HEAD(&s_sta_list, node, field);
    xSemaphoreGive(s_sta_mutex);
    ESP_LOGI(TAG, "Tracking station " MACSTR, MAC2STR(mac));
}

static void sta_remove_node(const uint8_t mac[6])
{
    if (s_sta_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_sta_mutex, pdMS_TO_TICKS(STA_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Station list lock timeout while removing");
        return;
    }
    sta_node_t *node;
    sta_node_t *tmp;
    SLIST_FOREACH_SAFE(node, &s_sta_list, field, tmp) {
        if (memcmp(node->mac, mac, sizeof(node->mac)) == 0) {
            SLIST_REMOVE(&s_sta_list, node, sta_node, field);
            free(node);
            break;
        }
    }
    xSemaphoreGive(s_sta_mutex);
}

static void sta_update_ip(const uint8_t mac[6], uint32_t ip)
{
    if (s_sta_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_sta_mutex, pdMS_TO_TICKS(STA_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    sta_node_t *node;
    SLIST_FOREACH(node, &s_sta_list, field) {
        if (memcmp(node->mac, mac, sizeof(node->mac)) == 0) {
            node->ip.addr = ip;
            break;
        }
    }
    xSemaphoreGive(s_sta_mutex);
}

static bool sta_node_exists(const uint8_t mac[6])
{
    bool found = false;
    if (s_sta_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_sta_mutex, pdMS_TO_TICKS(STA_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    sta_node_t *node;
    SLIST_FOREACH(node, &s_sta_list, field) {
        if (memcmp(node->mac, mac, sizeof(node->mac)) == 0) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_sta_mutex);
    return found;
}

static bool sta_node_rename(const uint8_t mac[6], const char *name)
{
    bool found = false;
    if (s_sta_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_sta_mutex, pdMS_TO_TICKS(STA_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    sta_node_t *node;
    SLIST_FOREACH(node, &s_sta_list, field) {
        if (memcmp(node->mac, mac, sizeof(node->mac)) == 0) {
            snprintf(node->name, sizeof(node->name), "%s", name);
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_sta_mutex);
    return found;
}

static void sta_free_all(void)
{
    if (s_sta_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_sta_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    sta_node_t *node;
    while ((node = SLIST_FIRST(&s_sta_list)) != NULL) {
        SLIST_REMOVE_HEAD(&s_sta_list, field);
        free(node);
    }
    xSemaphoreGive(s_sta_mutex);
}

typedef struct {
    char name[STA_NAME_MAX_LEN + 1];
    char mac[18];
    char ip[16];
    int64_t start_time;
} sta_snapshot_t;

static size_t sta_snapshot(sta_snapshot_t *out, size_t max)
{
    size_t count = 0;
    if (s_sta_mutex == NULL) {
        return 0;
    }
    if (xSemaphoreTake(s_sta_mutex, pdMS_TO_TICKS(STA_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return 0;
    }
    sta_node_t *node;
    SLIST_FOREACH(node, &s_sta_list, field) {
        if (count >= max) {
            break;
        }
        snprintf(out[count].name, sizeof(out[count].name), "%s", node->name);
        mac_to_colon(node->mac, out[count].mac);
        esp_ip4_addr_t addr = node->ip;
        if (addr.addr != 0) {
            esp_ip4addr_ntoa(&addr, out[count].ip, sizeof(out[count].ip));
        } else {
            snprintf(out[count].ip, sizeof(out[count].ip), "0.0.0.0");
        }
        out[count].start_time = node->start_time;
        count++;
    }
    xSemaphoreGive(s_sta_mutex);
    return count;
}

/* ------------------------------------------------------------------------- */
/* Authentication endpoints                                                  */
/* ------------------------------------------------------------------------- */

static esp_err_t api_login_post_handler(httpd_req_t *req)
{
    esp_err_t pre = precheck_post(req, true);
    if (pre != ESP_OK) {
        return pre;
    }

    char body[MAX_JSON_BODY + 1];
    size_t body_len = 0;
    body_result_t body_result = read_json_body(req, body, sizeof(body), &body_len);
    if (body_result != BODY_OK) {
        secure_wipe(body, sizeof(body));
        return send_body_error(req, body_result);
    }
    if (!json_depth_ok(body, body_len, JSON_MAX_DEPTH)) {
        secure_wipe(body, sizeof(body));
        return send_error_response(req, HTTPD_400, "invalid_json", true);
    }

    jparse_ctx_t jctx = { 0 };
    char username[CPE_ADMIN_USER_MAX_LEN + 1] = { 0 };
    char password[CPE_WIFI_PASS_MAX_LEN + 1] = { 0 };
    esp_err_t err = ESP_OK;
    if (json_parse_start(&jctx, body, (int)body_len) != OS_SUCCESS) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        err = json_get_string_unescaped(&jctx, "username", username, sizeof(username));
        if (err == ESP_OK) {
            err = json_get_string_unescaped(&jctx, "password", password, sizeof(password));
        }
    }
    json_parse_end(&jctx);
    secure_wipe(body, sizeof(body));

    if (err != ESP_OK) {
        secure_wipe(password, sizeof(password));
        return send_error_response(req, HTTPD_400, "invalid_request", true);
    }

    char session[CPE_SESSION_TOKEN_LEN] = { 0 };
    char csrf[CPE_CSRF_TOKEN_LEN] = { 0 };
    bool must_change = false;
    cpe_auth_result_t result = cpe_auth_login(username, password,
                                              session, sizeof(session),
                                              csrf, sizeof(csrf), &must_change);
    secure_wipe(password, sizeof(password));

    switch (result) {
    case CPE_AUTH_OK: {
        if (set_session_cookie(req, session) != ESP_OK) {
            return send_error_response(req, HTTPD_500_STR, "internal_error", true);
        }
        (void)set_csrf_cookie(req, csrf);
        sbuf_t sb;
        sbuf_init(&sb, 128);
        sbuf_appendf(&sb, "{\"status\":\"ok\",\"must_change\":%s,\"csrf\":",
                     must_change ? "true" : "false");
        sbuf_append_json_string(&sb, csrf);
        sbuf_append(&sb, "}");
        ESP_LOGI(TAG, "Administrator logged in (forced password change: %s)",
                 must_change ? "yes" : "no");
        return send_sbuf_json(req, HTTPD_200, &sb, false);
    }
    case CPE_AUTH_ERR_LOCKED: {
        uint32_t remaining = cpe_auth_lockout_remaining_s();
        char retry_after[16];
        snprintf(retry_after, sizeof(retry_after), "%u", (unsigned)remaining);
        (void)httpd_resp_set_hdr(req, "Retry-After", retry_after);
        ESP_LOGW(TAG, "Login rejected: locked out for %u s", (unsigned)remaining);
        return send_error_response(req, HTTPD_429_STR, "locked_out", false);
    }
    case CPE_AUTH_ERR_NO_MEM:
        return send_error_response(req, HTTPD_500_STR, "out_of_memory", false);
    default:
        /* Do not log the attempted credential. */
        ESP_LOGW(TAG, "Login failed");
        return send_error_response(req, HTTPD_401_STR, "invalid_credentials", false);
    }
}

static esp_err_t api_logout_post_handler(httpd_req_t *req)
{
    esp_err_t pre = precheck_post(req, false);
    if (pre != ESP_OK) {
        return pre;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    /* Logout is idempotent and only logs the user out; accepting a valid
     * session without a CSRF token makes "logout" always work even if the
     * front-end lost its token (e.g. after a browser restart). */
    esp_err_t err = require_session(req, session, false);
    if (err != ESP_OK) {
        return err;
    }

    char body[MAX_JSON_BODY + 1];
    size_t body_len = 0;
    if (req->content_len > 0) {
        body_result_t body_result = read_json_body(req, body, sizeof(body), &body_len);
        secure_wipe(body, sizeof(body));
        if (body_result != BODY_OK) {
            return send_body_error(req, body_result);
        }
        (void)body_len;
    }

    cpe_auth_logout(session);
    (void)clear_session_cookie(req);
    clear_csrf_cookie(req);
    ESP_LOGI(TAG, "Administrator logged out");
    sbuf_t sb;
    sbuf_init(&sb, 64);
    sbuf_append(&sb, "{\"status\":\"ok\"}");
    return send_sbuf_json(req, HTTPD_200, &sb, false);
}

static esp_err_t api_password_post_handler(httpd_req_t *req)
{
    esp_err_t pre = precheck_post(req, true);
    if (pre != ESP_OK) {
        return pre;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    esp_err_t err = require_session(req, session, true);
    if (err != ESP_OK) {
        return err;
    }

    char body[MAX_JSON_BODY + 1];
    size_t body_len = 0;
    body_result_t body_result = read_json_body(req, body, sizeof(body), &body_len);
    if (body_result != BODY_OK) {
        secure_wipe(body, sizeof(body));
        return send_body_error(req, body_result);
    }
    if (!json_depth_ok(body, body_len, JSON_MAX_DEPTH)) {
        secure_wipe(body, sizeof(body));
        return send_error_response(req, HTTPD_400, "invalid_json", false);
    }

    jparse_ctx_t jctx = { 0 };
    char old_password[CPE_WIFI_PASS_MAX_LEN + 1] = { 0 };
    char new_password[CPE_WIFI_PASS_MAX_LEN + 1] = { 0 };
    if (json_parse_start(&jctx, body, (int)body_len) != OS_SUCCESS) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        err = json_get_string_unescaped(&jctx, "old_password", old_password, sizeof(old_password));
        if (err == ESP_OK) {
            err = json_get_string_unescaped(&jctx, "new_password", new_password, sizeof(new_password));
        }
    }
    json_parse_end(&jctx);
    secure_wipe(body, sizeof(body));

    if (err != ESP_OK) {
        secure_wipe(old_password, sizeof(old_password));
        secure_wipe(new_password, sizeof(new_password));
        return send_error_response(req, HTTPD_400, "invalid_request", false);
    }
    if (cpe_config_validate_wifi_password(new_password) != ESP_OK) {
        secure_wipe(old_password, sizeof(old_password));
        secure_wipe(new_password, sizeof(new_password));
        return send_error_response(req, HTTPD_400, "weak_password", false);
    }

    err = cpe_auth_change_password(old_password, new_password);
    secure_wipe(old_password, sizeof(old_password));
    secure_wipe(new_password, sizeof(new_password));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Password change rejected: %s", esp_err_to_name(err));
        return send_error_response(req, HTTPD_400, "password_change_failed", false);
    }

    /* cpe_auth_change_password() invalidated every session, including the
     * current one; clear the cookie on the client as well. */
    (void)clear_session_cookie(req);
    clear_csrf_cookie(req);
    sbuf_t sb;
    sbuf_init(&sb, 64);
    sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"password_changed\"}");
    return send_sbuf_json(req, HTTPD_200, &sb, false);
}

/* ------------------------------------------------------------------------- */
/* Status endpoint                                                           */
/* ------------------------------------------------------------------------- */

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    esp_err_t body_check = reject_unexpected_body(req);
    if (body_check != ESP_OK) {
        return body_check;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    if (require_session(req, session, false) != ESP_OK) {
        return ESP_FAIL;
    }

    cpe_status_t status = { 0 };
    cpe_status_get(&status);

    cpe_config_t cfg = { 0 };
    bool cfg_ok = (cpe_config_get(&cfg) == ESP_OK);

    sbuf_t sb;
    sbuf_init(&sb, 768);
    sbuf_appendf(&sb, "{\"status\":\"ok\",\"must_change\":%s,",
                 cpe_config_admin_must_change() ? "true" : "false");
    sbuf_appendf(&sb, "\"state\":%d,\"state_text\":", (int)status.state);
    sbuf_append_json_string(&sb, status.state_text);
    sbuf_appendf(&sb, ",\"usb_connected\":%s,\"link_up\":%s,\"got_ip\":%s,\"ip\":",
                 status.usb_connected ? "true" : "false",
                 status.link_up ? "true" : "false",
                 status.got_ip ? "true" : "false");
    sbuf_append_json_string(&sb, status.ip);
    sbuf_appendf(&sb, ",\"has_public_ip\":%s,\"public_ip\":",
                 status.has_public_ip ? "true" : "false");
    sbuf_append_json_string(&sb, status.public_ip);
    sbuf_appendf(&sb, ",\"rssi\":%d,\"rssi_valid\":%s,\"modem_model\":",
                 (int)status.rssi, status.rssi_valid ? "true" : "false");
    sbuf_append_json_string(&sb, status.modem_model);
    sbuf_append(&sb, ",\"modem_fw\":");
    sbuf_append_json_string(&sb, status.modem_fw);
    sbuf_appendf(&sb, ",\"uptime_s\":%" PRIu32 ",\"recovery_attempts\":%" PRIu32
                 ",\"boot_count\":%" PRIu32 ",\"last_error\":",
                 status.uptime_s, status.recovery_attempts, status.boot_count);
    sbuf_append_json_string(&sb, status.last_error);
    sbuf_append(&sb, ",\"device_name\":");
    sbuf_append_json_string(&sb, cfg_ok ? cfg.device_name : "");
    sbuf_append(&sb, ",\"ap\":{\"ssid\":");
    sbuf_append_json_string(&sb, cfg_ok ? cfg.ap_ssid : "");
    sbuf_appendf(&sb, ",\"channel\":%u,\"bandwidth\":\"%u\",\"hidden\":%s,\"max_sta\":%u}}",
                 (unsigned)(cfg_ok ? cfg.ap_channel : 0),
                 (unsigned)(cfg_ok ? cfg.ap_bandwidth : 0),
                 (cfg_ok && cfg.ap_hidden) ? "true" : "false",
                 (unsigned)(cfg_ok ? cfg.max_sta : 0));
    return send_sbuf_json(req, HTTPD_200, &sb, false);
}

/* ------------------------------------------------------------------------- */
/* Factory reset                                                             */
/* ------------------------------------------------------------------------- */

static esp_err_t api_factory_reset_post_handler(httpd_req_t *req)
{
    esp_err_t pre = precheck_post(req, false);
    if (pre != ESP_OK) {
        return pre;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    /* Factory reset is allowed even while a forced password change is pending
     * (it is the recovery path), but it still requires a session + CSRF. */
    esp_err_t err = require_session(req, session, true);
    if (err != ESP_OK) {
        return err;
    }

    char body[MAX_JSON_BODY + 1];
    size_t body_len = 0;
    if (req->content_len > 0) {
        body_result_t body_result = read_json_body(req, body, sizeof(body), &body_len);
        secure_wipe(body, sizeof(body));
        if (body_result != BODY_OK) {
            return send_body_error(req, body_result);
        }
        (void)body_len;
    }

    err = cpe_config_factory_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset failed: %s", esp_err_to_name(err));
        return send_error_response(req, HTTPD_500_STR, "factory_reset_failed", false);
    }

    /* Also drop the station nicknames stored in their own namespace. */
    nvs_handle_t names_handle;
    if (nvs_open(STA_NAME_NVS_NAMESPACE, NVS_READWRITE, &names_handle) == ESP_OK) {
        if (nvs_erase_all(names_handle) == ESP_OK) {
            (void)nvs_commit(names_handle);
        }
        nvs_close(names_handle);
    }

    cpe_auth_logout_all();
    (void)clear_session_cookie(req);
    clear_csrf_cookie(req);
    ESP_LOGW(TAG, "Factory reset requested from the web console, restarting");

    sbuf_t sb;
    sbuf_init(&sb, 96);
    sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"factory_reset\"}");
    err = send_sbuf_json(req, HTTPD_200, &sb, false);

    /* Give the response a chance to leave the socket before restarting. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return err;
}

/* ------------------------------------------------------------------------- */
/* Firmware update (OTA endpoints) and system reboot                         */
/* ------------------------------------------------------------------------- */

#define OTA_CHUNK_SIZE              4096
#define OTA_UPLOAD_TIMEOUT_US       (180LL * 1000 * 1000)

/*
 * The OTA upload is a binary body (not JSON), so precheck_post() cannot be
 * used: it rejects anything larger than MAX_JSON_BODY. Only the origin check
 * and the presence of a body are enforced here; the size limit is applied by
 * cpe_ota_begin() against the inactive OTA partition.
 */
static esp_err_t precheck_ota_post(httpd_req_t *req)
{
    if (req->content_len == 0) {
        return send_error_response(req, HTTPD_400, "empty_body", true);
    }
    if (!request_origin_is_same_origin(req)) {
        ESP_LOGW(TAG, "Rejecting cross-origin firmware upload");
        return send_error_response(req, HTTPD_403_STR, "cross_origin_rejected", true);
    }
    return ESP_OK;
}

static esp_err_t api_ota_status_get_handler(httpd_req_t *req)
{
    esp_err_t body_check = reject_unexpected_body(req);
    if (body_check != ESP_OK) {
        return body_check;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    if (require_session(req, session, false) != ESP_OK) {
        return ESP_FAIL;
    }

    cpe_ota_info_t info;
    if (cpe_ota_get_info(&info) != ESP_OK) {
        return send_error_response(req, HTTPD_500_STR, "ota_info_failed", false);
    }

    sbuf_t sb;
    sbuf_init(&sb, 512);
    sbuf_append(&sb, "{\"status\":\"200\",\"version\":");
    sbuf_append_json_string(&sb, info.running_version);
    sbuf_append(&sb, ",\"partition\":");
    sbuf_append_json_string(&sb, info.running_partition);
    sbuf_append(&sb, ",\"next_partition\":");
    sbuf_append_json_string(&sb, info.next_partition);
    sbuf_appendf(&sb, ",\"max_image_size\":%u,\"pending_verify\":%s,\"upload_in_progress\":%s}",
                 (unsigned)info.max_image_size,
                 info.pending_verify ? "true" : "false",
                 info.upload_in_progress ? "true" : "false");
    return send_sbuf_json(req, HTTPD_200, &sb, false);
}

static esp_err_t api_ota_upload_post_handler(httpd_req_t *req)
{
    esp_err_t pre = precheck_ota_post(req);
    if (pre != ESP_OK) {
        return pre;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    esp_err_t err = require_session_mutable(req, session);
    if (err != ESP_OK) {
        return err;
    }

    cpe_ota_info_t info;
    if (cpe_ota_get_info(&info) != ESP_OK) {
        return send_error_response(req, HTTPD_500_STR, "ota_info_failed", true);
    }
    if (info.upload_in_progress) {
        return send_error_response(req, HTTPD_409_STR, "ota_busy", true);
    }
    if (info.max_image_size > 0 && req->content_len > info.max_image_size) {
        ESP_LOGW(TAG, "Rejecting firmware upload of %u bytes (partition is %u bytes)",
                 (unsigned)req->content_len, (unsigned)info.max_image_size);
        return send_error_response(req, HTTPD_413_STR, "image_too_large", true);
    }

    uint8_t *buf = malloc(OTA_CHUNK_SIZE);
    if (buf == NULL) {
        return send_error_response(req, HTTPD_500_STR, "out_of_memory", true);
    }

    /* Read the first chunk before touching flash: it carries the image magic
     * byte, so a wrong file (HTML page, wrong binary, ...) is rejected without
     * erasing the inactive OTA partition. */
    size_t remaining = req->content_len;
    int64_t start_us = esp_timer_get_time();
    int first_len = 0;
    for (;;) {
        first_len = httpd_req_recv(req, (char *)buf,
                                   remaining > OTA_CHUNK_SIZE ? OTA_CHUNK_SIZE : remaining);
        if (first_len == HTTPD_SOCK_ERR_TIMEOUT) {
            if (esp_timer_get_time() - start_us > OTA_UPLOAD_TIMEOUT_US) {
                free(buf);
                return send_error_response(req, HTTPD_408_STR, "upload_timeout", true);
            }
            continue;
        }
        break;
    }
    if (first_len <= 0) {
        free(buf);
        return send_error_response(req, HTTPD_400, "upload_interrupted", true);
    }
    if (buf[0] != 0xE9) { /* ESP_IMAGE_HEADER_MAGIC */
        ESP_LOGW(TAG, "Rejected upload: not an ESP32 firmware image (magic 0x%02x)", buf[0]);
        free(buf);
        return send_error_response(req, HTTPD_400, "invalid_firmware_image", true);
    }

    err = cpe_ota_begin(req->content_len);
    if (err == ESP_ERR_INVALID_SIZE) {
        free(buf);
        return send_error_response(req, HTTPD_413_STR, "image_too_large", true);
    }
    if (err == CPE_OTA_ERR_PENDING_VERIFY) {
        /* The running image was installed by OTA and has not been validated
         * yet (cpe_ota validates it CPE_OTA_VALIDATE_DELAY_MS after boot). */
        free(buf);
        return send_error_response(req, HTTPD_409_STR, "pending_verify", true);
    }
    if (err == CPE_OTA_ERR_BUSY) {
        free(buf);
        return send_error_response(req, HTTPD_409_STR, "ota_busy", true);
    }
    if (err != ESP_OK) {
        free(buf);
        return send_error_response(req, HTTPD_500_STR, "ota_begin_failed", true);
    }

    esp_err_t upload_err = ESP_OK;
    while (first_len > 0 && remaining > 0) {
        upload_err = cpe_ota_write(buf, (size_t)first_len);
        if (upload_err != ESP_OK) {
            break;
        }
        remaining -= (size_t)first_len;
        if (remaining == 0) {
            break;
        }
        int received = httpd_req_recv(req, (char *)buf,
                                      remaining > OTA_CHUNK_SIZE ? OTA_CHUNK_SIZE : remaining);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (esp_timer_get_time() - start_us > OTA_UPLOAD_TIMEOUT_US) {
                upload_err = ESP_ERR_TIMEOUT;
                break;
            }
            continue;
        }
        if (received <= 0) {
            upload_err = ESP_FAIL;
            break;
        }
        first_len = received;
    }
    free(buf);

    if (upload_err != ESP_OK) {
        cpe_ota_abort();
        ESP_LOGE(TAG, "Firmware upload failed: %s", esp_err_to_name(upload_err));
        if (upload_err == CPE_OTA_ERR_INVALID_IMAGE) {
            return send_error_response(req, HTTPD_400, "invalid_firmware_image", true);
        }
        return send_error_response(req,
                                   upload_err == ESP_ERR_TIMEOUT ? HTTPD_408_STR : HTTPD_400,
                                   "upload_interrupted", true);
    }

    err = cpe_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Firmware image rejected: %s", esp_err_to_name(err));
        return send_error_response(req, HTTPD_400, "invalid_firmware_image", true);
    }

    sbuf_t sb;
    sbuf_init(&sb, 96);
    sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"ota_ready\",\"reboot\":true}");
    err = send_sbuf_json(req, HTTPD_200, &sb, false);

    /* Let the response leave the socket, then boot the new image. */
    ESP_LOGW(TAG, "Firmware update stored, restarting into the new image");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return err;
}

static esp_err_t api_system_reboot_post_handler(httpd_req_t *req)
{
    esp_err_t pre = precheck_post(req, false);
    if (pre != ESP_OK) {
        return pre;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    esp_err_t err = require_session_mutable(req, session);
    if (err != ESP_OK) {
        return err;
    }
    if (req->content_len > 0) {
        char body[MAX_JSON_BODY + 1];
        size_t body_len = 0;
        if (read_json_body(req, body, sizeof(body), &body_len) != BODY_OK) {
            return send_error_response(req, HTTPD_400, "invalid_body", true);
        }
    }

    sbuf_t sb;
    sbuf_init(&sb, 64);
    sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"rebooting\"}");
    err = send_sbuf_json(req, HTTPD_200, &sb, false);

    ESP_LOGW(TAG, "Reboot requested from the web console");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return err;
}

/* ------------------------------------------------------------------------- */
/* WLAN basic settings (/wlan_general)                                       */
/* ------------------------------------------------------------------------- */

static esp_err_t wlan_general_get_handler(httpd_req_t *req)
{
    esp_err_t body_check = reject_unexpected_body(req);
    if (body_check != ESP_OK) {
        return body_check;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    if (require_session(req, session, false) != ESP_OK) {
        return ESP_FAIL;
    }

    cpe_config_t cfg;
    esp_err_t err = cpe_config_get(&cfg);
    if (err != ESP_OK) {
        return send_error_response(req, HTTPD_500_STR, "config_unavailable", false);
    }

    sbuf_t sb;
    sbuf_init(&sb, 384);
    sbuf_append(&sb, "{\"status\":\"200\",\"ssid\":");
    sbuf_append_json_string(&sb, cfg.ap_ssid);
    sbuf_appendf(&sb, ",\"if_hide_ssid\":\"%s\",\"auth_mode\":\"WPA2_PSK\",\"has_password\":%s",
                 cfg.ap_hidden ? "true" : "false",
                 cfg.ap_password[0] != '\0' ? "true" : "false");
    sbuf_append(&sb, ",\"device_name\":");
    sbuf_append_json_string(&sb, cfg.device_name);
    sbuf_appendf(&sb, ",\"max_sta\":%u,\"channel\":%u,\"bandwidth\":\"%u\"}",
                 (unsigned)cfg.max_sta, (unsigned)cfg.ap_channel, (unsigned)cfg.ap_bandwidth);
    return send_sbuf_json(req, HTTPD_200, &sb, false);
}

static esp_err_t wlan_general_post_handler(httpd_req_t *req)
{
    esp_err_t pre = precheck_post(req, true);
    if (pre != ESP_OK) {
        return pre;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    esp_err_t err = require_session_mutable(req, session);
    if (err != ESP_OK) {
        return err;
    }

    char body[MAX_JSON_BODY + 1];
    size_t body_len = 0;
    body_result_t body_result = read_json_body(req, body, sizeof(body), &body_len);
    if (body_result != BODY_OK) {
        secure_wipe(body, sizeof(body));
        return send_body_error(req, body_result);
    }
    if (!json_depth_ok(body, body_len, JSON_MAX_DEPTH)) {
        secure_wipe(body, sizeof(body));
        return send_error_response(req, HTTPD_400, "invalid_json", false);
    }

    jparse_ctx_t jctx = { 0 };
    char ssid[CPE_SSID_MAX_LEN + 1] = { 0 };
    char password[CPE_WIFI_PASS_MAX_LEN + 1] = { 0 };
    char auth_mode[16] = { 0 };
    char device_name[CPE_DEVICE_NAME_MAX_LEN + 1] = { 0 };
    bool has_ssid = false;
    bool has_password = false;
    bool has_auth_mode = false;
    bool has_device_name = false;
    bool has_hidden = false;
    bool hidden = false;
    bool has_max_sta = false;
    int max_sta = 0;

    if (json_parse_start(&jctx, body, (int)body_len) != OS_SUCCESS) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        err = json_get_string_unescaped(&jctx, "ssid", ssid, sizeof(ssid));
        has_ssid = (err == ESP_OK);
        if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = json_get_string_unescaped(&jctx, "password", password, sizeof(password));
            has_password = (err == ESP_OK);
            if (err == ESP_ERR_NOT_FOUND) {
                err = ESP_OK;
            }
        }
        if (err == ESP_OK) {
            err = json_get_string_unescaped(&jctx, "auth_mode", auth_mode, sizeof(auth_mode));
            has_auth_mode = (err == ESP_OK);
            if (err == ESP_ERR_NOT_FOUND) {
                err = ESP_OK;
            }
        }
        if (err == ESP_OK) {
            err = json_get_string_unescaped(&jctx, "device_name", device_name, sizeof(device_name));
            has_device_name = (err == ESP_OK);
            if (err == ESP_ERR_NOT_FOUND) {
                err = ESP_OK;
            }
        }
        if (err == ESP_OK) {
            err = json_get_bool_flexible(&jctx, "if_hide_ssid", &hidden, &has_hidden);
        }
        if (err == ESP_OK) {
            err = json_get_int_flexible(&jctx, "max_sta", &max_sta, &has_max_sta);
        }
    }
    json_parse_end(&jctx);
    secure_wipe(body, sizeof(body));

    if (err != ESP_OK) {
        secure_wipe(password, sizeof(password));
        return send_error_response(req, HTTPD_400, "invalid_request", false);
    }

    /* ---- server side validation (audit P1-13) ---- */
    if (!has_ssid || cpe_config_validate_ap_ssid(ssid) != ESP_OK) {
        secure_wipe(password, sizeof(password));
        return send_error_response(req, HTTPD_400, "invalid_ssid", false);
    }
    bool password_changed = false;
    if (has_password && password[0] != '\0') {
        if (cpe_config_validate_wifi_password(password) != ESP_OK) {
            secure_wipe(password, sizeof(password));
            return send_error_response(req, HTTPD_400, "invalid_password", false);
        }
        password_changed = true;
    }
    if (has_auth_mode && strcasecmp(auth_mode, "WPA2_PSK") != 0) {
        secure_wipe(password, sizeof(password));
        return send_error_response(req, HTTPD_400, "unsupported_auth_mode", false);
    }
    if (has_device_name && cpe_config_validate_device_name(device_name) != ESP_OK) {
        secure_wipe(password, sizeof(password));
        return send_error_response(req, HTTPD_400, "invalid_device_name", false);
    }
    if (has_max_sta && cpe_config_validate_max_sta(max_sta) != ESP_OK) {
        secure_wipe(password, sizeof(password));
        return send_error_response(req, HTTPD_400, "invalid_max_sta", false);
    }

    cpe_config_t current;
    err = cpe_config_get(&current);
    if (err != ESP_OK) {
        secure_wipe(password, sizeof(password));
        return send_error_response(req, HTTPD_500_STR, "config_unavailable", false);
    }

    cpe_config_t next = current;
    snprintf(next.ap_ssid, sizeof(next.ap_ssid), "%s", ssid);
    if (password_changed) {
        snprintf(next.ap_password, sizeof(next.ap_password), "%s", password);
    }
    if (has_hidden) {
        next.ap_hidden = hidden;
    }
    if (has_max_sta) {
        next.max_sta = (uint8_t)max_sta;
    }
    if (has_device_name) {
        snprintf(next.device_name, sizeof(next.device_name), "%s", device_name);
    }

    /* Persist the device name first: it does not touch the radio, so a failure
     * here must not leave the SoftAP changed (review P2). */
    bool name_persisted = false;
    if (has_device_name && strcmp(current.device_name, next.device_name) != 0) {
        err = cpe_config_set_device_name(next.device_name);
        if (err != ESP_OK) {
            secure_wipe(password, sizeof(password));
            ESP_LOGE(TAG, "Failed to persist device name: %s", esp_err_to_name(err));
            return send_error_response(req, HTTPD_500_STR, "persist_failed", false);
        }
        name_persisted = true;
    }

    /* Apply to the radio next; only persist the AP settings when the driver
     * accepted them. A device-name-only change must not restart the SoftAP. */
    bool ap_changed = (strcmp(current.ap_ssid, next.ap_ssid) != 0) ||
                      password_changed ||
                      (current.ap_hidden != next.ap_hidden) ||
                      (current.max_sta != next.max_sta);
    if (ap_changed) {
        err = app_wifi_apply(&next);
        if (err != ESP_OK) {
            if (name_persisted) {
                (void)cpe_config_set_device_name(current.device_name);
            }
            secure_wipe(password, sizeof(password));
            ESP_LOGE(TAG, "Failed to apply new WLAN settings: %s", esp_err_to_name(err));
            return send_error_response(req, HTTPD_400, "apply_failed", false);
        }

        err = cpe_config_set_ap(next.ap_ssid,
                                password_changed ? next.ap_password : NULL,
                                next.ap_channel, next.ap_bandwidth,
                                next.ap_hidden, next.max_sta);
        secure_wipe(password, sizeof(password));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist WLAN settings: %s", esp_err_to_name(err));
            (void)app_wifi_apply(&current); /* roll the radio back */
            if (name_persisted) {
                (void)cpe_config_set_device_name(current.device_name);
            }
            return send_error_response(req, HTTPD_500_STR, "persist_failed", false);
        }
    } else {
        secure_wipe(password, sizeof(password));
    }
    secure_wipe(next.ap_password, sizeof(next.ap_password));

    ESP_LOGI(TAG, "WLAN settings updated (SSID changed: %s, password changed: %s, hidden: %d)",
             strcmp(current.ap_ssid, next.ap_ssid) != 0 ? "yes" : "no",
             password_changed ? "yes" : "no",
             next.ap_hidden ? 1 : 0);

    sbuf_t sb;
    sbuf_init(&sb, 96);
    sbuf_append(&sb, "{\"status\":\"200\",\"message\":\"settings_applied\"}");
    return send_sbuf_json(req, HTTPD_200, &sb, false);
}

/* ------------------------------------------------------------------------- */
/* WLAN advanced settings (/wlan_advance)                                    */
/* ------------------------------------------------------------------------- */

static esp_err_t wlan_advance_get_handler(httpd_req_t *req)
{
    esp_err_t body_check = reject_unexpected_body(req);
    if (body_check != ESP_OK) {
        return body_check;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    if (require_session(req, session, false) != ESP_OK) {
        return ESP_FAIL;
    }
    cpe_config_t cfg;
    esp_err_t err = cpe_config_get(&cfg);
    if (err != ESP_OK) {
        return send_error_response(req, HTTPD_500_STR, "config_unavailable", false);
    }
    sbuf_t sb;
    sbuf_init(&sb, 96);
    sbuf_appendf(&sb, "{\"status\":\"200\",\"bandwidth\":\"%u\",\"channel\":\"%u\"}",
                 (unsigned)cfg.ap_bandwidth, (unsigned)cfg.ap_channel);
    return send_sbuf_json(req, HTTPD_200, &sb, false);
}

static esp_err_t wlan_advance_post_handler(httpd_req_t *req)
{
    esp_err_t pre = precheck_post(req, true);
    if (pre != ESP_OK) {
        return pre;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    esp_err_t err = require_session_mutable(req, session);
    if (err != ESP_OK) {
        return err;
    }

    char body[MAX_JSON_BODY + 1];
    size_t body_len = 0;
    body_result_t body_result = read_json_body(req, body, sizeof(body), &body_len);
    if (body_result != BODY_OK) {
        return send_body_error(req, body_result);
    }
    if (!json_depth_ok(body, body_len, JSON_MAX_DEPTH)) {
        return send_error_response(req, HTTPD_400, "invalid_json", false);
    }

    jparse_ctx_t jctx = { 0 };
    bool has_channel = false;
    bool has_bandwidth = false;
    int channel = 0;
    int bandwidth = 0;
    if (json_parse_start(&jctx, body, (int)body_len) != OS_SUCCESS) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        err = json_get_int_flexible(&jctx, "channel", &channel, &has_channel);
        if (err == ESP_OK) {
            err = json_get_int_flexible(&jctx, "bandwidth", &bandwidth, &has_bandwidth);
        }
    }
    json_parse_end(&jctx);

    if (err != ESP_OK || !has_channel || !has_bandwidth) {
        return send_error_response(req, HTTPD_400, "invalid_request", false);
    }
    if (cpe_config_validate_channel(channel) != ESP_OK) {
        return send_error_response(req, HTTPD_400, "invalid_channel", false);
    }
    if (cpe_config_validate_bandwidth(bandwidth) != ESP_OK) {
        return send_error_response(req, HTTPD_400, "invalid_bandwidth", false);
    }

    cpe_config_t current;
    err = cpe_config_get(&current);
    if (err != ESP_OK) {
        return send_error_response(req, HTTPD_500_STR, "config_unavailable", false);
    }
    cpe_config_t next = current;
    next.ap_channel = (uint8_t)channel;
    next.ap_bandwidth = (uint8_t)bandwidth;

    err = app_wifi_apply(&next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply advanced WLAN settings: %s", esp_err_to_name(err));
        return send_error_response(req, HTTPD_400, "apply_failed", false);
    }
    err = cpe_config_set_ap(next.ap_ssid, NULL, next.ap_channel, next.ap_bandwidth,
                            next.ap_hidden, next.max_sta);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist advanced WLAN settings: %s", esp_err_to_name(err));
        (void)app_wifi_apply(&current);
        return send_error_response(req, HTTPD_500_STR, "persist_failed", false);
    }

    ESP_LOGI(TAG, "WLAN advanced settings updated (channel %d, bandwidth %d MHz)",
             channel, bandwidth);
    sbuf_t sb;
    sbuf_init(&sb, 96);
    sbuf_append(&sb, "{\"status\":\"200\",\"message\":\"settings_applied\"}");
    return send_sbuf_json(req, HTTPD_200, &sb, false);
}

/* ------------------------------------------------------------------------- */
/* Station list endpoints                                                    */
/* ------------------------------------------------------------------------- */

static esp_err_t system_station_get_handler(httpd_req_t *req)
{
    esp_err_t body_check = reject_unexpected_body(req);
    if (body_check != ESP_OK) {
        return body_check;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    if (require_session(req, session, false) != ESP_OK) {
        return ESP_FAIL;
    }

    sta_snapshot_t snapshot[STA_SNAPSHOT_MAX];
    size_t count = sta_snapshot(snapshot, STA_SNAPSHOT_MAX);

    cpe_config_t cfg = { 0 };
    bool cfg_ok = (cpe_config_get(&cfg) == ESP_OK);

    sbuf_t sb;
    sbuf_init(&sb, 512);
    sbuf_append(&sb, "{\"status\":\"200\",\"station_list\":[");
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            sbuf_append(&sb, ",");
        }
        sbuf_append(&sb, "{\"name_str\":");
        sbuf_append_json_string(&sb, snapshot[i].name);
        sbuf_append(&sb, ",\"mac_str\":");
        sbuf_append_json_string(&sb, snapshot[i].mac);
        sbuf_append(&sb, ",\"ip_str\":");
        sbuf_append_json_string(&sb, snapshot[i].ip);
        sbuf_appendf(&sb, ",\"online_time_s\":%lld}", (long long)snapshot[i].start_time);
    }
    sbuf_appendf(&sb, "],\"now_time\":%lld,\"max_sta\":%u}",
                 (long long)esp_timer_get_time(),
                 (unsigned)(cfg_ok ? cfg.max_sta : CPE_MAX_STA_MAX));
    return send_sbuf_json(req, HTTPD_200, &sb, false);
}

static esp_err_t system_station_delete_device_post_handler(httpd_req_t *req)
{
    esp_err_t pre = precheck_post(req, true);
    if (pre != ESP_OK) {
        return pre;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    esp_err_t err = require_session_mutable(req, session);
    if (err != ESP_OK) {
        return err;
    }

    char body[MAX_JSON_BODY + 1];
    size_t body_len = 0;
    body_result_t body_result = read_json_body(req, body, sizeof(body), &body_len);
    if (body_result != BODY_OK) {
        return send_body_error(req, body_result);
    }
    if (!json_depth_ok(body, body_len, JSON_MAX_DEPTH)) {
        return send_error_response(req, HTTPD_400, "invalid_json", false);
    }

    jparse_ctx_t jctx = { 0 };
    char mac_str[24] = { 0 };
    if (json_parse_start(&jctx, body, (int)body_len) != OS_SUCCESS) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        err = json_get_string_unescaped(&jctx, "mac_str", mac_str, sizeof(mac_str));
    }
    json_parse_end(&jctx);

    uint8_t mac[6] = { 0 };
    if (err != ESP_OK || !mac_parse(mac_str, mac)) {
        return send_error_response(req, HTTPD_400, "invalid_mac", false);
    }
    if (!sta_node_exists(mac)) {
        return send_error_response(req, HTTPD_404_STR, "station_not_found", false);
    }

    uint16_t aid = 0;
    err = esp_wifi_ap_get_sta_aid(mac, &aid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Station " MACSTR " is not associated: %s", MAC2STR(mac), esp_err_to_name(err));
        return send_error_response(req, HTTPD_404_STR, "station_not_found", false);
    }
    err = esp_wifi_deauth_sta(aid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deauthenticate station: %s", esp_err_to_name(err));
        return send_error_response(req, HTTPD_500_STR, "deauth_failed", false);
    }

    ESP_LOGI(TAG, "Station " MACSTR " was disconnected from the web console", MAC2STR(mac));
    sbuf_t sb;
    sbuf_init(&sb, 64);
    sbuf_append(&sb, "{\"status\":\"200\",\"message\":\"station_disconnected\"}");
    return send_sbuf_json(req, HTTPD_200, &sb, false);
}

static esp_err_t system_station_change_name_post_handler(httpd_req_t *req)
{
    esp_err_t pre = precheck_post(req, true);
    if (pre != ESP_OK) {
        return pre;
    }
    char session[SESSION_BUF_LEN] = { 0 };
    esp_err_t err = require_session_mutable(req, session);
    if (err != ESP_OK) {
        return err;
    }

    char body[MAX_JSON_BODY + 1];
    size_t body_len = 0;
    body_result_t body_result = read_json_body(req, body, sizeof(body), &body_len);
    if (body_result != BODY_OK) {
        return send_body_error(req, body_result);
    }
    if (!json_depth_ok(body, body_len, JSON_MAX_DEPTH)) {
        return send_error_response(req, HTTPD_400, "invalid_json", false);
    }

    jparse_ctx_t jctx = { 0 };
    char name_str[STA_NAME_MAX_LEN + 1] = { 0 };
    char mac_str[24] = { 0 };
    if (json_parse_start(&jctx, body, (int)body_len) != OS_SUCCESS) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        err = json_get_string_unescaped(&jctx, "name_str", name_str, sizeof(name_str));
        if (err == ESP_OK) {
            err = json_get_string_unescaped(&jctx, "mac_str", mac_str, sizeof(mac_str));
        }
    }
    json_parse_end(&jctx);

    uint8_t mac[6] = { 0 };
    if (err != ESP_OK || !mac_parse(mac_str, mac)) {
        return send_error_response(req, HTTPD_400, "invalid_request", false);
    }
    if (!station_name_valid(name_str)) {
        return send_error_response(req, HTTPD_400, "invalid_station_name", false);
    }
    if (!sta_node_rename(mac, name_str)) {
        return send_error_response(req, HTTPD_404_STR, "station_not_found", false);
    }

    /* Best effort persistence: the live list already carries the new name. */
    esp_err_t save_err = station_name_save(mac, name_str);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist station name: %s", esp_err_to_name(save_err));
    }

    ESP_LOGI(TAG, "Station " MACSTR " renamed from the web console", MAC2STR(mac));
    sbuf_t sb;
    sbuf_init(&sb, 64);
    sbuf_append(&sb, "{\"status\":\"200\",\"message\":\"station_renamed\"}");
    return send_sbuf_json(req, HTTPD_200, &sb, false);
}

/* ------------------------------------------------------------------------- */
/* Static files (SPIFFS)                                                     */
/* ------------------------------------------------------------------------- */

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
 * handler stays safe if the assets move to FAT/SD later (audit 6.4). */
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

static esp_err_t rest_common_get_handler(httpd_req_t *req)
{
    esp_err_t body_check = reject_unexpected_body(req);
    if (body_check != ESP_OK) {
        return body_check;
    }
    if (!uri_is_safe(req->uri)) {
        return send_error_response(req, HTTPD_400, "invalid_path", false);
    }

    rest_server_context_t *context = (rest_server_context_t *)req->user_ctx;
    if (context == NULL) {
        return send_error_response(req, HTTPD_500_STR, "internal_error", false);
    }

    char filepath[FILE_PATH_MAX];
    size_t uri_len = strlen(req->uri);
    int written;
    if (uri_len > 0 && req->uri[uri_len - 1] == '/') {
        written = snprintf(filepath, sizeof(filepath), "%s%sindex.html", context->base_path, req->uri);
    } else {
        written = snprintf(filepath, sizeof(filepath), "%s%s", context->base_path, req->uri);
    }
    if (written < 0 || (size_t)written >= sizeof(filepath)) {
        return send_error_response(req, HTTPD_414_STR, "uri_too_long", false);
    }

    int fd = open(filepath, O_RDONLY, 0);
    if (fd < 0) {
        ESP_LOGD(TAG, "Static file not found: %s", filepath);
        return send_error_response(req, HTTPD_404_STR, "not_found", false);
    }

    esp_err_t err = httpd_resp_set_type(req, content_type_for_path(filepath));
    if (err == ESP_OK) {
        err = set_common_headers(req, false);
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
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
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

/* ------------------------------------------------------------------------- */
/* Wi-Fi events for the station list                                         */
/* ------------------------------------------------------------------------- */

static void cpe_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    (void)arg;

    if (!s_events_registered) {
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
        sta_add_node(event->mac);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d", MAC2STR(event->mac), event->aid);
        sta_remove_node(event->mac);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        ip_event_assigned_ip_to_client_t *event = (ip_event_assigned_ip_to_client_t *)event_data;
        sta_update_ip(event->mac, event->ip.addr);
    }
}

/* ------------------------------------------------------------------------- */
/* Server lifecycle                                                          */
/* ------------------------------------------------------------------------- */

static esp_err_t init_fs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_CPE_WEB_MOUNT_POINT,
        .partition_label = NULL,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_ERR_INVALID_STATE) {
        /* Already mounted by a previous start - but only report success when
         * SPIFFS really is mounted; other INVALID_STATE causes (encrypted
         * partition, exhausted VFS slots, init failure) must be surfaced. */
        if (esp_spiffs_mounted(NULL)) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "SPIFFS register returned INVALID_STATE but no SPIFFS is mounted");
        return ret;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0;
    size_t used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: total %u, used %u", (unsigned)total, (unsigned)used);
    } else {
        ESP_LOGW(TAG, "Failed to read SPIFFS info: %s", esp_err_to_name(ret));
    }
    return ESP_OK;
}

static httpd_handle_t s_server;
static rest_server_context_t *s_rest_context;

static const httpd_uri_t s_uri_handlers[] = {
    { .uri = "/api/login",       .method = HTTP_POST, .handler = api_login_post_handler },
    { .uri = "/api/logout",      .method = HTTP_POST, .handler = api_logout_post_handler },
    { .uri = "/api/password",    .method = HTTP_POST, .handler = api_password_post_handler },
    { .uri = "/api/status",      .method = HTTP_GET,  .handler = api_status_get_handler },
    { .uri = "/api/factory_reset", .method = HTTP_POST, .handler = api_factory_reset_post_handler },
    { .uri = "/api/ota/status",  .method = HTTP_GET,  .handler = api_ota_status_get_handler },
    { .uri = "/api/ota/upload",  .method = HTTP_POST, .handler = api_ota_upload_post_handler },
    { .uri = "/api/system/reboot", .method = HTTP_POST, .handler = api_system_reboot_post_handler },
    { .uri = "/wlan_general",    .method = HTTP_GET,  .handler = wlan_general_get_handler },
    { .uri = "/wlan_general",    .method = HTTP_POST, .handler = wlan_general_post_handler },
    { .uri = "/wlan_advance",    .method = HTTP_GET,  .handler = wlan_advance_get_handler },
    { .uri = "/wlan_advance",    .method = HTTP_POST, .handler = wlan_advance_post_handler },
    { .uri = "/system/station_state", .method = HTTP_GET, .handler = system_station_get_handler },
    { .uri = "/system/station_state/delete_device", .method = HTTP_POST,
      .handler = system_station_delete_device_post_handler },
    { .uri = "/system/station_state/change_name", .method = HTTP_POST,
      .handler = system_station_change_name_post_handler },
};

static void free_rest_context(void)
{
    free(s_rest_context);
    s_rest_context = NULL;
}

esp_err_t modem_http_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* cpe_core_init() already did this at boot; calling it again is a no-op
     * that keeps this component independently testable. */
    esp_err_t err = cpe_auth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cpe_auth_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = init_fs();
    if (err != ESP_OK) {
        return err;
    }

    /* The mutex MUST exist before any Wi-Fi event handler or HTTP handler is
     * registered (audit P1-17). */
    s_sta_mutex = xSemaphoreCreateMutex();
    if (s_sta_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create station list mutex");
        return ESP_ERR_NO_MEM;
    }
    SLIST_INIT(&s_sta_list);

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, cpe_wifi_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register Wi-Fi event handler: %s", esp_err_to_name(err));
        goto fail;
    }
    /* Mark before the second registration so the error path unregisters both
     * (unregistering a handler that was never added is harmless). */
    s_events_registered = true;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                     cpe_wifi_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
        goto fail;
    }

    s_rest_context = calloc(1, sizeof(*s_rest_context));
    if (s_rest_context == NULL) {
        ESP_LOGE(TAG, "Out of memory for web server context");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    snprintf(s_rest_context->base_path, sizeof(s_rest_context->base_path),
             "%s", CONFIG_CPE_WEB_MOUNT_POINT);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.max_resp_headers = 12;
    config.max_open_sockets = 7; /* matches the browser per-origin connection limit */
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 3;
    config.lru_purge_enable = true;
    config.keep_alive_enable = false;
    /* Two extra KiB for the 2 KiB JSON body buffer used by the handlers. */
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    /* The management console must only be reachable from the SoftAP side. The
     * ECM WAN interface can get a routable address from the modem (e.g.
     * 192.168.225.x); binding the listening socket to the AP netif keeps the
     * console off the WAN (audit item 7.6). The ifreq only has to stay valid
     * until httpd_start() returns. */
    struct ifreq ap_ifr;
    memset(&ap_ifr, 0, sizeof(ap_ifr));
    esp_netif_t *ap_netif = app_wifi_get_ap_netif();
    if (ap_netif == NULL ||
            esp_netif_get_netif_impl_name(ap_netif, ap_ifr.ifr_name) != ESP_OK ||
            ap_ifr.ifr_name[0] == '\0') {
        ESP_LOGE(TAG, "SoftAP netif unavailable; refusing to expose the web server "
                 "on the WAN interface");
        err = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    config.if_name = &ap_ifr;
    ESP_LOGI(TAG, "Web server will listen on SoftAP interface \"%s\" only", ap_ifr.ifr_name);

    err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        s_server = NULL;
        goto fail;
    }

    for (size_t i = 0; i < sizeof(s_uri_handlers) / sizeof(s_uri_handlers[0]); i++) {
        err = httpd_register_uri_handler(s_server, &s_uri_handlers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register handler %s: %s",
                     s_uri_handlers[i].uri, esp_err_to_name(err));
            goto fail;
        }
    }

    httpd_uri_t static_handler = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = rest_common_get_handler,
        .user_ctx = s_rest_context,
    };
    err = httpd_register_uri_handler(s_server, &static_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register static file handler: %s", esp_err_to_name(err));
        goto fail;
    }

    s_started = true;
    ESP_LOGI(TAG, "Management web server started on port %d", config.server_port);
    return ESP_OK;

fail:
    if (s_server != NULL) {
        (void)httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_events_registered) {
        (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                           cpe_wifi_event_handler);
        (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, cpe_wifi_event_handler);
        s_events_registered = false;
    }
    free_rest_context();
    if (s_sta_mutex != NULL) {
        vSemaphoreDelete(s_sta_mutex);
        s_sta_mutex = NULL;
    }
    return err;
}

esp_err_t modem_http_stop(void)
{
    if (!s_started) {
        return ESP_OK;
    }

    /* Stop accepting event callbacks first, then give an in-flight callback a
     * chance to return before the station list is released. */
    s_started = false;
    bool had_events = s_events_registered;
    s_events_registered = false;
    if (had_events) {
        (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                           cpe_wifi_event_handler);
        (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, cpe_wifi_event_handler);
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    if (s_server != NULL) {
        esp_err_t err = httpd_stop(s_server);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "httpd_stop failed: %s", esp_err_to_name(err));
        }
        s_server = NULL;
    }

    free_rest_context();

    /* No handler can run anymore, so the list can be released safely. */
    sta_free_all();
    if (s_sta_mutex != NULL) {
        vSemaphoreDelete(s_sta_mutex);
        s_sta_mutex = NULL;
    }

    ESP_LOGI(TAG, "Management web server stopped");
    return ESP_OK;
}
