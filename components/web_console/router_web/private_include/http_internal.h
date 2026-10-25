/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal interfaces of the management web server. This header is private to
 * the web_console component; it is not part of the public component API.
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

/* Maximum accepted JSON request body. */
#define CPE_WEB_MAX_JSON_BODY           2048
/* Upper bound for the whole body read, regardless of the socket timeouts. */
#define CPE_WEB_BODY_READ_TIMEOUT_US    (2 * 1000 * 1000)
/* cJSON's parser is recursive; reject deeply nested documents
 * before handing them to it. */
#define CPE_WEB_JSON_MAX_DEPTH          6

#define CPE_WEB_STA_NAME_MAX_LEN        32
/* Text forms of a station address: "aa:bb:cc:dd:ee:ff", the same 12 hex digits
 * without separators (used as the NVS key), and a dotted IPv4. Buffer sizes,
 * NUL included: the snapshot fields and the formatters have to agree on them. */
#define CPE_WEB_MAC_COLON_BUFSIZE       18
#define CPE_WEB_MAC_HEX_BUFSIZE         13
#define CPE_WEB_IPV4_BUFSIZE            16
/* The station-nickname namespace lives in cpe_kv.h (CPE_KV_NS_STA_NAMES). */
#define CPE_WEB_STA_SNAPSHOT_MAX        10
#define CPE_WEB_STA_LOCK_TIMEOUT_MS     100

#define CPE_WEB_FILE_PATH_MAX           (ESP_VFS_PATH_MAX + 128)
#define CPE_WEB_SCRATCH_BUFSIZE         10240

/* A handler must return quickly, so anything that blocks runs in its own
 * one-shot task. All of them are short lived and do the same kind of work, so
 * they share one stack size and one priority. */
#define CPE_WEB_HELPER_TASK_STACK       4096
#define CPE_WEB_HELPER_TASK_PRIO        5

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
#define CPE_WEB_HTTPD_501_STR           "501 Not Implemented"

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
    char mac[CPE_WEB_MAC_COLON_BUFSIZE];
    char ip[CPE_WEB_IPV4_BUFSIZE];
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

/* GET counterpart of the POST preamble: body rejection plus session
 * validation in one call. Used by the route dispatcher, so a GET endpoint can
 * no longer be added without its authentication being declared. */
typedef enum {
    CPE_WEB_GET_SESSION_NONE = 0,   /* public endpoint (static assets, login) */
    CPE_WEB_GET_SESSION_REQUIRED,   /* valid session */
} cpe_web_get_session_t;

typedef struct {
    cpe_web_get_session_t session;
    bool csrf;                      /* honoured for SESSION_REQUIRED only */
} cpe_web_get_opts_t;

/* --- route table ----------------------------------------------------------- */

/* Authentication required by one route. GET routes are enforced by the
 * dispatcher; POST routes get a uniform session gate there as well, while the
 * CSRF token, the body and the forced password change stay in the handler's
 * cpe_web_post_begin() call (it hands the session token to the handler). The
 * value below is the single place where the requirement is declared, so it
 * must always match the handler's own opts. */
typedef enum {
    CPE_WEB_AUTH_PUBLIC = 0,        /* no session at all */
    CPE_WEB_AUTH_SESSION,           /* valid session */
    CPE_WEB_AUTH_SESSION_CSRF,      /* session + CSRF token */
    CPE_WEB_AUTH_MUTABLE,           /* session + CSRF + password already changed */
} cpe_web_auth_t;

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *req);
    cpe_web_auth_t auth;
} cpe_web_route_t;

/* httpd handler slots: one per table entry plus the catch-all static handler. */
#define CPE_WEB_HTTPD_MAX_URI_HANDLERS  24

/* One slot per registered URI (registration repeats after every listener
 * re-creation, the table is reset beforehand). */
#define CPE_WEB_ROUTE_LOG_MAX           24

/* --- http_common.c ------------------------------------------------------- */

void cpe_web_secure_wipe(void *ptr, size_t len);

/* Reset the per-request log table before a (re)registration round. No-op
 * unless CONFIG_CPE_WEB_HTTP_LOG is enabled. */
void cpe_web_route_log_reset(void);

esp_err_t cpe_web_register_uris(httpd_handle_t server, const httpd_uri_t *uris,
                                size_t count);

/* --- http_response.c ----------------------------------------------------- */

/* Handler return value that also closes the connection and skips the
 * unread-body purge: the HTTP server treats any non-OK return as "close the
 * socket". Use it to distinguish "sent and closed" from a real send failure. */
#define CPE_WEB_CLOSE_SOCKET            ESP_FAIL

esp_err_t cpe_web_send_json_body(httpd_req_t *req, const char *status_line,
                                 const char *json, bool close_socket);
/* Answer with one of the declared errors: the HTTP status and whether the
 * connection is dropped both come from the table in http_response.c, so a call
 * site only names the error. An unknown code is logged and answered as 500. */
esp_err_t cpe_web_send_error(httpd_req_t *req, const char *code);
esp_err_t cpe_web_set_common_headers(httpd_req_t *req, bool no_store);

esp_err_t cpe_web_set_session_cookie(httpd_req_t *req, const char *session);
esp_err_t cpe_web_set_csrf_cookie(httpd_req_t *req, const char *csrf);
void cpe_web_clear_csrf_cookie(httpd_req_t *req);
esp_err_t cpe_web_clear_session_cookie(httpd_req_t *req);

/* --- http_request.c ------------------------------------------------------ */

bool cpe_web_request_origin_is_same_origin(httpd_req_t *req);
esp_err_t cpe_web_precheck_post(httpd_req_t *req, bool require_body);
cpe_web_body_result_t cpe_web_read_json_body(httpd_req_t *req, char *buf,
                                             size_t buf_len, size_t *out_len);
esp_err_t cpe_web_send_body_error(httpd_req_t *req, cpe_web_body_result_t result);
esp_err_t cpe_web_reject_unexpected_body(httpd_req_t *req);

/* --- http_session.c ------------------------------------------------------ */

esp_err_t cpe_web_require_session(httpd_req_t *req,
                                  char session[CPE_WEB_SESSION_BUF_LEN], bool need_csrf);
esp_err_t cpe_web_require_session_mutable(httpd_req_t *req,
                                          char session[CPE_WEB_SESSION_BUF_LEN]);

esp_err_t cpe_web_get_begin(httpd_req_t *req, const cpe_web_get_opts_t *opts,
                            char session[CPE_WEB_SESSION_BUF_LEN]);

esp_err_t cpe_web_post_begin(httpd_req_t *req, const cpe_web_post_opts_t *opts,
                             cpe_web_post_ctx_t *ctx);
void cpe_web_post_end(cpe_web_post_ctx_t *ctx);

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

/* --- station_names.c ----------------------------------------------------- */

void cpe_web_mac_to_hex(const uint8_t mac[6], char out[CPE_WEB_MAC_HEX_BUFSIZE]);
void cpe_web_mac_to_colon(const uint8_t mac[6], char out[CPE_WEB_MAC_COLON_BUFSIZE]);
bool cpe_web_mac_parse(const char *str, uint8_t mac[6]);
bool cpe_web_station_name_valid(const char *name);
esp_err_t cpe_web_station_name_load(const uint8_t mac[6], char *out, size_t out_len);
esp_err_t cpe_web_station_name_save(const uint8_t mac[6], const char *name);

/* --- station_registry.c -------------------------------------------------- */

esp_err_t cpe_web_station_registry_init(void);
void cpe_web_station_registry_deinit(void);
void cpe_web_station_registry_enable(bool enabled);
void cpe_web_station_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data);

size_t cpe_web_sta_snapshot(cpe_web_sta_snapshot_t *out, size_t max);
bool cpe_web_sta_node_exists(const uint8_t mac[6]);
bool cpe_web_sta_node_rename(const uint8_t mac[6], const char *name);

/* --- modem_http_config.c ------------------------------------------------- */

/* Stop the listening socket before the SoftAP is restarted (app_wifi_apply()).
 * The socket is bound to the AP netif and lwIP drops the binding when the
 * interface goes down, so closing it first gives the browser a clean close
 * instead of a half-open connection. Must not be called from an HTTP handler
 * (httpd_stop() waits for that very task to exit); call it from a helper task
 * after the response has been queued. */
void cpe_web_listener_suspend_for_ap_restart(void);

/* Re-create the listener when a scheduled SoftAP restart did not happen (apply
 * failed, or only the channel/bandwidth changed). No-op while the AP netif is
 * down or a re-creation is already queued. */
void cpe_web_listener_ensure_up(void);

/* --- routes (web_routes.c) ----------------------------------------------- */

/* Register every API route of the table below plus the catch-all static handler.
 * Must be called again after the listener is re-created. */
esp_err_t cpe_web_routes_register(httpd_handle_t server,
                                  cpe_web_rest_context_t *context);

/* --- route handlers ------------------------------------------------------ */

/* Handlers are non-static only so that web_routes.c can hold the single route
 * table; nothing outside this component sees them. Authentication is enforced
 * by the dispatcher and must not be repeated inside the GET handlers. */
esp_err_t api_login_post_handler(httpd_req_t *req);
esp_err_t api_logout_post_handler(httpd_req_t *req);
esp_err_t api_password_post_handler(httpd_req_t *req);
esp_err_t api_status_get_handler(httpd_req_t *req);
esp_err_t api_maintenance_get_handler(httpd_req_t *req);
esp_err_t api_maintenance_post_handler(httpd_req_t *req);
esp_err_t api_factory_reset_post_handler(httpd_req_t *req);
esp_err_t api_system_reboot_post_handler(httpd_req_t *req);
esp_err_t api_ota_status_get_handler(httpd_req_t *req);
esp_err_t api_ota_upload_post_handler(httpd_req_t *req);
esp_err_t api_sms_list_get_handler(httpd_req_t *req);
esp_err_t api_sms_send_post_handler(httpd_req_t *req);
esp_err_t api_sms_delete_post_handler(httpd_req_t *req);
esp_err_t wlan_general_get_handler(httpd_req_t *req);
esp_err_t wlan_general_post_handler(httpd_req_t *req);
esp_err_t wlan_advance_get_handler(httpd_req_t *req);
esp_err_t wlan_advance_post_handler(httpd_req_t *req);
esp_err_t system_station_get_handler(httpd_req_t *req);
esp_err_t system_station_delete_device_post_handler(httpd_req_t *req);
esp_err_t system_station_change_name_post_handler(httpd_req_t *req);

/* --- static assets (static_files.c) --------------------------------------- */

esp_err_t cpe_web_static_files_register(httpd_handle_t server,
                                        cpe_web_rest_context_t *context);
