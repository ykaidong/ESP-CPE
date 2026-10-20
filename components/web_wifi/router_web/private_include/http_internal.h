/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal interfaces of the management web server. This header is private to
 * the web_wifi component; it is not part of the public component API.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_vfs.h"
#include "cJSON.h"

#include "cpe_auth.h"

/* Session / CSRF cookies and headers. */
#define CPE_WEB_SESSION_COOKIE_NAME     "CPE_SID"
#define CPE_WEB_CSRF_COOKIE_NAME        "CPE_CSRF"
#define CPE_WEB_CSRF_HEADER_NAME        "X-CSRF-Token"
#define CPE_WEB_SESSION_BUF_LEN         (CPE_SESSION_TOKEN_LEN + 8)
#define CPE_WEB_CSRF_BUF_LEN            (CPE_CSRF_TOKEN_LEN + 8)

/* Maximum accepted JSON request body (audit P1-12). */
#define CPE_WEB_MAX_JSON_BODY           2048
/* Upper bound for the whole body read, regardless of the socket timeouts. */
#define CPE_WEB_BODY_READ_TIMEOUT_US    (2 * 1000 * 1000)
/* cJSON's parser is recursive (audit R-12); reject deeply nested documents
 * before handing them to it. */
#define CPE_WEB_JSON_MAX_DEPTH          6

#define CPE_WEB_STA_NAME_MAX_LEN        32
#define CPE_WEB_STA_NAME_NVS_NAMESPACE  "sta_names"
#define CPE_WEB_STA_SNAPSHOT_MAX        10
#define CPE_WEB_STA_LOCK_TIMEOUT_MS     100

#define CPE_WEB_FILE_PATH_MAX           (ESP_VFS_PATH_MAX + 128)
#define CPE_WEB_SCRATCH_BUFSIZE         10240

#define CPE_WEB_HTTPD_401_STR           "401 Unauthorized"
#define CPE_WEB_HTTPD_403_STR           "403 Forbidden"
#define CPE_WEB_HTTPD_404_STR           "404 Not Found"
#define CPE_WEB_HTTPD_408_STR           "408 Request Timeout"
#define CPE_WEB_HTTPD_409_STR           "409 Conflict"
#define CPE_WEB_HTTPD_413_STR           "413 Content Too Large"
#define CPE_WEB_HTTPD_414_STR           "414 URI Too Long"
#define CPE_WEB_HTTPD_415_STR           "415 Unsupported Media Type"
#define CPE_WEB_HTTPD_429_STR           "429 Too Many Requests"
#define CPE_WEB_HTTPD_500_STR           "500 Internal Server Error"

typedef enum {
    CPE_WEB_BODY_OK = 0,
    CPE_WEB_BODY_EMPTY,
    CPE_WEB_BODY_TOO_LARGE,
    CPE_WEB_BODY_TIMEOUT,
    CPE_WEB_BODY_INVALID,
} cpe_web_body_result_t;

typedef struct {
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[CPE_WEB_SCRATCH_BUFSIZE];
} cpe_web_rest_context_t;

/* Growable string buffer with JSON escaping (used by the response builders). */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} cpe_web_sbuf_t;

typedef struct {
    char name[CPE_WEB_STA_NAME_MAX_LEN + 1];
    char mac[18];
    char ip[16];
    int64_t start_time_us;
} cpe_web_sta_snapshot_t;

/*
 * Common preamble for JSON POST handlers: origin/content checks, session/CSRF
 * validation, body reading and parsing, and the matching cleanup.
 */
typedef enum {
    CPE_WEB_POST_SESSION_NONE = 0,  /* public endpoint (login) */
    CPE_WEB_POST_SESSION_REQUIRED,  /* valid session, CSRF optional */
    CPE_WEB_POST_SESSION_MUTABLE,   /* session + CSRF + no forced password change */
} cpe_web_post_session_t;

typedef enum {
    CPE_WEB_POST_BODY_NONE = 0,     /* the body is neither read nor required */
    CPE_WEB_POST_BODY_OPTIONAL,     /* read (and wiped) if present */
    CPE_WEB_POST_BODY_REQUIRED,     /* reject an empty body */
} cpe_web_post_body_t;

typedef struct {
    cpe_web_post_session_t session;
    cpe_web_post_body_t body;
    bool csrf;                      /* honoured for SESSION_REQUIRED only */
    bool parse_json;                /* requires BODY_REQUIRED */
} cpe_web_post_opts_t;

typedef struct {
    char session[CPE_WEB_SESSION_BUF_LEN];
    char body[CPE_WEB_MAX_JSON_BODY + 1];
    size_t body_len;
    cJSON *parser;                  /* NULL unless parse_json succeeded */
} cpe_web_post_ctx_t;

/* --- http_common.c ------------------------------------------------------- */

void cpe_web_secure_wipe(void *ptr, size_t len);

esp_err_t cpe_web_send_json_body(httpd_req_t *req, const char *status_line,
                                 const char *json, bool close_socket);
esp_err_t cpe_web_send_error_response(httpd_req_t *req, const char *status_line,
                                      const char *code, bool close_socket);
esp_err_t cpe_web_set_common_headers(httpd_req_t *req, bool no_store);

bool cpe_web_request_origin_is_same_origin(httpd_req_t *req);
esp_err_t cpe_web_precheck_post(httpd_req_t *req, bool require_body);
cpe_web_body_result_t cpe_web_read_json_body(httpd_req_t *req, char *buf,
                                             size_t buf_len, size_t *out_len);
esp_err_t cpe_web_send_body_error(httpd_req_t *req, cpe_web_body_result_t result);
esp_err_t cpe_web_reject_unexpected_body(httpd_req_t *req);

esp_err_t cpe_web_require_session(httpd_req_t *req,
                                  char session[CPE_WEB_SESSION_BUF_LEN], bool need_csrf);
esp_err_t cpe_web_require_session_mutable(httpd_req_t *req,
                                          char session[CPE_WEB_SESSION_BUF_LEN]);

esp_err_t cpe_web_post_begin(httpd_req_t *req, const cpe_web_post_opts_t *opts,
                             cpe_web_post_ctx_t *ctx);
void cpe_web_post_end(cpe_web_post_ctx_t *ctx);

esp_err_t cpe_web_set_session_cookie(httpd_req_t *req, const char *session);
esp_err_t cpe_web_set_csrf_cookie(httpd_req_t *req, const char *csrf);
void cpe_web_clear_csrf_cookie(httpd_req_t *req);
esp_err_t cpe_web_clear_session_cookie(httpd_req_t *req);

esp_err_t cpe_web_register_uris(httpd_handle_t server, const httpd_uri_t *uris,
                                size_t count);

/* --- json_util.c --------------------------------------------------------- */

void cpe_web_sbuf_init(cpe_web_sbuf_t *sb, size_t cap);
void cpe_web_sbuf_free(cpe_web_sbuf_t *sb);
void cpe_web_sbuf_appendf(cpe_web_sbuf_t *sb, const char *fmt, ...);
void cpe_web_sbuf_append(cpe_web_sbuf_t *sb, const char *str);
void cpe_web_sbuf_append_json_string(cpe_web_sbuf_t *sb, const char *src);
esp_err_t cpe_web_send_sbuf_json(httpd_req_t *req, const char *status_line,
                                 cpe_web_sbuf_t *sb, bool close_socket);

bool cpe_web_json_depth_ok(const char *body, size_t len, int max_depth);
esp_err_t cpe_web_json_get_string_unescaped(const cJSON *root, const char *name,
                                            char *out, size_t out_len);
esp_err_t cpe_web_json_get_int_flexible(const cJSON *root, const char *name,
                                        int *out, bool *present);
esp_err_t cpe_web_json_get_bool_flexible(const cJSON *root, const char *name,
                                         bool *out, bool *present);

/* --- station_registry.c -------------------------------------------------- */

esp_err_t cpe_web_station_registry_init(void);
void cpe_web_station_registry_deinit(void);
void cpe_web_station_registry_enable(bool enabled);
void cpe_web_station_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data);

bool cpe_web_mac_parse(const char *str, uint8_t mac[6]);
bool cpe_web_station_name_valid(const char *name);
esp_err_t cpe_web_station_name_save(const uint8_t mac[6], const char *name);
size_t cpe_web_sta_snapshot(cpe_web_sta_snapshot_t *out, size_t max);
bool cpe_web_sta_node_exists(const uint8_t mac[6]);
bool cpe_web_sta_node_rename(const uint8_t mac[6], const char *name);

/* --- API modules --------------------------------------------------------- */

esp_err_t cpe_web_api_auth_register(httpd_handle_t server);
esp_err_t cpe_web_api_status_register(httpd_handle_t server);
esp_err_t cpe_web_api_wlan_register(httpd_handle_t server);
esp_err_t cpe_web_api_station_register(httpd_handle_t server);
esp_err_t cpe_web_api_system_register(httpd_handle_t server);
esp_err_t cpe_web_api_ota_register(httpd_handle_t server);
esp_err_t cpe_web_static_files_register(httpd_handle_t server,
                                        cpe_web_rest_context_t *context);
