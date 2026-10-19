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

esp_err_t cpe_web_json_get_string_unescaped(jparse_ctx_t *jctx, const char *name,
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
esp_err_t cpe_web_json_get_int_flexible(jparse_ctx_t *jctx, const char *name,
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
esp_err_t cpe_web_json_get_bool_flexible(jparse_ctx_t *jctx, const char *name,
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
