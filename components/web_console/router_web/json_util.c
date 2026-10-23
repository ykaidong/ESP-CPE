/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * JSON input/output helpers for the management web server.
 */

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_internal.h"

/* ------------------------------------------------------------------------- */
/* Growable string buffer with JSON escaping                                 */
/* ------------------------------------------------------------------------- */

void cpe_web_sbuf_init(cpe_web_sbuf_t *sb, size_t cap)
{
    sb->data = calloc(1, cap);
    sb->len = 0;
    sb->cap = sb->data ? cap : 0;
    sb->failed = (sb->data == NULL);
}

void cpe_web_sbuf_free(cpe_web_sbuf_t *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->failed = true;
}

/* Make room for `extra` bytes plus the terminating NUL. Returns false when the
 * buffer cannot be grown; sb->failed is then set, so every later append is a
 * no-op and the caller answers 500. */
static bool sbuf_reserve(cpe_web_sbuf_t *sb, size_t extra)
{
    size_t required = sb->len + extra + 1;
    if (required <= sb->cap) {
        return true;
    }
    size_t new_cap = sb->cap ? sb->cap : 128;
    while (new_cap < required) {
        new_cap *= 2;
    }
    char *grown = realloc(sb->data, new_cap);
    if (grown == NULL) {
        sb->failed = true;
        return false;
    }
    sb->data = grown;
    sb->cap = new_cap;
    return true;
}

/* Copy bytes verbatim. Formatting one character at a time cost two vsnprintf()
 * calls per character, which a 1 KB status response does a few thousand times. */
static void sbuf_append_raw(cpe_web_sbuf_t *sb, const char *data, size_t len)
{
    if (sb->failed || len == 0) {
        return;
    }
    if (!sbuf_reserve(sb, len)) {
        return;
    }
    memcpy(sb->data + sb->len, data, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

void cpe_web_sbuf_appendf(cpe_web_sbuf_t *sb, const char *fmt, ...)
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
    if (!sbuf_reserve(sb, (size_t)need)) {
        va_end(ap2);
        return;
    }
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)need;
}

void cpe_web_sbuf_append(cpe_web_sbuf_t *sb, const char *str)
{
    if (str == NULL) {
        return;
    }
    sbuf_append_raw(sb, str, strlen(str));
}

/* One entry per byte that has a short escape; NULL means "copy the byte as it
 * is". Bytes >= 0x80 are UTF-8 and are always copied verbatim. */
static const char *const JSON_ESCAPES[0x80] = {
    ['"']  = "\\\"",
    ['\\'] = "\\\\",
    ['\b'] = "\\b",
    ['\f'] = "\\f",
    ['\n'] = "\\n",
    ['\r'] = "\\r",
    ['\t'] = "\\t",
};

/* Append a JSON string literal with every escape JSON requires. The values come
 * from modem strings and station names, none of which is trusted. */
void cpe_web_sbuf_append_json_string(cpe_web_sbuf_t *sb, const char *src)
{
    if (sb->failed) {
        return;
    }
    sbuf_append_raw(sb, "\"", 1);
    if (src != NULL) {
        const unsigned char *p = (const unsigned char *)src;
        while (*p != '\0') {
            /* Copy the whole run of characters that need no escape in one go. */
            const unsigned char *run = p;
            while (*p >= 0x80 ||
                   (*p >= 0x20 && *p != 0x7f && JSON_ESCAPES[*p] == NULL)) {
                p++;
            }
            sbuf_append_raw(sb, (const char *)run, (size_t)(p - run));

            if (*p == '\0') {
                break;
            }
            unsigned char c = *p++;
            if (c < 0x80 && JSON_ESCAPES[c] != NULL) {
                sbuf_append_raw(sb, JSON_ESCAPES[c], strlen(JSON_ESCAPES[c]));
            } else {
                /* Remaining control characters and DEL: they never appear in a
                 * station name or a modem string, so the formatting cost of the
                 * \u00xx form is irrelevant here. */
                cpe_web_sbuf_appendf(sb, "\\u%04x", (unsigned)c);
            }
        }
    }
    sbuf_append_raw(sb, "\"", 1);
}

esp_err_t cpe_web_send_sbuf_json(httpd_req_t *req, const char *status_line,
                                 cpe_web_sbuf_t *sb, bool close_socket)
{
    if (sb->failed || sb->data == NULL) {
        cpe_web_sbuf_free(sb);
        return cpe_web_send_error(req, "out_of_memory");
    }
    esp_err_t ret = cpe_web_send_json_body(req, status_line, sb->data, close_socket);
    cpe_web_sbuf_free(sb);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* JSON input                                                                */
/* ------------------------------------------------------------------------- */

/* Reject deeply nested documents before calling the recursive parser. */
bool cpe_web_json_depth_ok(const char *body, size_t len, int max_depth)
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

/* cJSON stores numbers as doubles; accept only values that fit in an int. */
static bool json_number_to_int(const cJSON *item, int *out)
{
    double value = item->valuedouble;
    if (value < (double)INT_MIN || value > (double)INT_MAX) {
        return false;
    }
    int ival = (int)value;
    if ((double)ival != value) {
        return false;
    }
    *out = ival;
    return true;
}

/* Copy a string field; cJSON has already decoded the JSON escapes. */
esp_err_t cpe_web_json_get_string_unescaped(const cJSON *root, const char *name,
                                            char *out, size_t out_len)
{
    if (root == NULL || name == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    /* A JSON null is treated as a missing field, matching the previous
     * json_parser based implementation. */
    if (item == NULL || cJSON_IsNull(item)) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Distinguish a missing field from a present but malformed/oversized
     * value: callers must reject the latter with 400, not
     * silently ignore it. */
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(item->valuestring);
    if (len >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, item->valuestring, len + 1);
    return ESP_OK;
}

/* Accept both a JSON number and a legacy JSON string ("6"). */
esp_err_t cpe_web_json_get_int_flexible(const cJSON *root, const char *name,
                                        int *out, bool *present)
{
    if (root == NULL || name == NULL || out == NULL || present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *present = false;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    /* A JSON null is treated as a missing field, matching the previous
     * json_parser based implementation. */
    if (item == NULL || cJSON_IsNull(item)) {
        return ESP_OK;
    }
    *present = true;

    if (cJSON_IsNumber(item)) {
        int value = 0;
        if (!json_number_to_int(item, &value)) {
            return ESP_ERR_INVALID_ARG;
        }
        *out = value;
        return ESP_OK;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        char *end = NULL;
        long long parsed = strtoll(item->valuestring, &end, 10);
        if (end == item->valuestring || *end != '\0' ||
                parsed < INT_MIN || parsed > INT_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        *out = (int)parsed;
        return ESP_OK;
    }
    /* Present but of the wrong type (e.g. 1.5, true or an object): reject
     * instead of pretending the field was absent. */
    return ESP_ERR_INVALID_ARG;
}

/* Accept JSON boolean and the legacy "true"/"false"/"1"/"0" strings. */
esp_err_t cpe_web_json_get_bool_flexible(const cJSON *root, const char *name,
                                         bool *out, bool *present)
{
    if (root == NULL || name == NULL || out == NULL || present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *present = false;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    /* A JSON null is treated as a missing field, matching the previous
     * json_parser based implementation. */
    if (item == NULL || cJSON_IsNull(item)) {
        return ESP_OK;
    }
    *present = true;

    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        return ESP_OK;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        if (strcmp(item->valuestring, "true") == 0 ||
                strcmp(item->valuestring, "1") == 0) {
            *out = true;
            return ESP_OK;
        }
        if (strcmp(item->valuestring, "false") == 0 ||
                strcmp(item->valuestring, "0") == 0) {
            *out = false;
            return ESP_OK;
        }
        return ESP_ERR_INVALID_ARG;
    }
    /* Legacy clients may send 0/1 as a JSON number. */
    if (cJSON_IsNumber(item)) {
        int value = 0;
        if (!json_number_to_int(item, &value) || (value != 0 && value != 1)) {
            return ESP_ERR_INVALID_ARG;
        }
        *out = (value != 0);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}
