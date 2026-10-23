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

void cpe_web_sbuf_append(cpe_web_sbuf_t *sb, const char *str)
{
    cpe_web_sbuf_appendf(sb, "%s", str);
}

/* Append a JSON string literal with all mandatory escapes (P1-16 / 6.4). */
void cpe_web_sbuf_append_json_string(cpe_web_sbuf_t *sb, const char *src)
{
    if (sb->failed) {
        return;
    }
    cpe_web_sbuf_append(sb, "\"");
    if (src != NULL) {
        for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
            switch (*p) {
            case '"':
                cpe_web_sbuf_append(sb, "\\\"");
                break;
            case '\\':
                cpe_web_sbuf_append(sb, "\\\\");
                break;
            case '\b':
                cpe_web_sbuf_append(sb, "\\b");
                break;
            case '\f':
                cpe_web_sbuf_append(sb, "\\f");
                break;
            case '\n':
                cpe_web_sbuf_append(sb, "\\n");
                break;
            case '\r':
                cpe_web_sbuf_append(sb, "\\r");
                break;
            case '\t':
                cpe_web_sbuf_append(sb, "\\t");
                break;
            default:
                if (*p < 0x20 || *p == 0x7f) {
                    cpe_web_sbuf_appendf(sb, "\\u%04x", (unsigned)*p);
                } else {
                    cpe_web_sbuf_appendf(sb, "%c", (char)*p);
                }
                break;
            }
        }
    }
    cpe_web_sbuf_append(sb, "\"");
}

esp_err_t cpe_web_send_sbuf_json(httpd_req_t *req, const char *status_line,
                                 cpe_web_sbuf_t *sb, bool close_socket)
{
    if (sb->failed || sb->data == NULL) {
        cpe_web_sbuf_free(sb);
        return cpe_web_send_error_response(req, CPE_WEB_HTTPD_500_STR,
                                           "out_of_memory", true);
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
