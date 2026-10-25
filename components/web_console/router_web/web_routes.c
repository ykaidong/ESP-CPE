/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Single route table of the management web server and the
 * authentication gate that goes with it.
 */

#include <stddef.h>

#include "esp_log.h"

#include "cpe_config.h"
#include "http_internal.h"

static const char *TAG = "cpe_web";

/* One entry per URI+method. Keep this table sorted by module; the counts are
 * checked against the httpd handler slots and the request log at compile time,
 * so a new route can no longer silently exceed either limit. */
static const cpe_web_route_t s_routes[] = {
    /* Authentication */
    { .uri = "/api/login", .method = HTTP_POST,
      .handler = api_login_post_handler, .auth = CPE_WEB_AUTH_PUBLIC },
    { .uri = "/api/logout", .method = HTTP_POST,
      .handler = api_logout_post_handler, .auth = CPE_WEB_AUTH_SESSION },
    { .uri = "/api/password", .method = HTTP_POST,
      .handler = api_password_post_handler, .auth = CPE_WEB_AUTH_SESSION_CSRF },

    /* Status */
    { .uri = "/api/status", .method = HTTP_GET,
      .handler = api_status_get_handler, .auth = CPE_WEB_AUTH_SESSION },

    /* System */
    { .uri = "/api/system/maintenance", .method = HTTP_GET,
      .handler = api_maintenance_get_handler, .auth = CPE_WEB_AUTH_SESSION },
    { .uri = "/api/system/maintenance", .method = HTTP_POST,
      .handler = api_maintenance_post_handler, .auth = CPE_WEB_AUTH_MUTABLE },
    { .uri = "/api/system/reboot", .method = HTTP_POST,
      .handler = api_system_reboot_post_handler, .auth = CPE_WEB_AUTH_MUTABLE },
    { .uri = "/api/factory_reset", .method = HTTP_POST,
      .handler = api_factory_reset_post_handler, .auth = CPE_WEB_AUTH_SESSION_CSRF },

    /* OTA */
    { .uri = "/api/ota/status", .method = HTTP_GET,
      .handler = api_ota_status_get_handler, .auth = CPE_WEB_AUTH_SESSION },
    { .uri = "/api/ota/upload", .method = HTTP_POST,
      .handler = api_ota_upload_post_handler, .auth = CPE_WEB_AUTH_MUTABLE },

    /* SMS */
    { .uri = "/api/sms/list", .method = HTTP_GET,
      .handler = api_sms_list_get_handler, .auth = CPE_WEB_AUTH_SESSION },
    { .uri = "/api/sms/send", .method = HTTP_POST,
      .handler = api_sms_send_post_handler, .auth = CPE_WEB_AUTH_MUTABLE },
    { .uri = "/api/sms/delete", .method = HTTP_POST,
      .handler = api_sms_delete_post_handler, .auth = CPE_WEB_AUTH_MUTABLE },

    /* SoftAP configuration */
    { .uri = "/wlan_general", .method = HTTP_GET,
      .handler = wlan_general_get_handler, .auth = CPE_WEB_AUTH_SESSION },
    { .uri = "/wlan_general", .method = HTTP_POST,
      .handler = wlan_general_post_handler, .auth = CPE_WEB_AUTH_MUTABLE },
    { .uri = "/wlan_advance", .method = HTTP_GET,
      .handler = wlan_advance_get_handler, .auth = CPE_WEB_AUTH_SESSION },
    { .uri = "/wlan_advance", .method = HTTP_POST,
      .handler = wlan_advance_post_handler, .auth = CPE_WEB_AUTH_MUTABLE },

    /* Connected stations */
    { .uri = "/system/station_state", .method = HTTP_GET,
      .handler = system_station_get_handler, .auth = CPE_WEB_AUTH_SESSION },
    { .uri = "/system/station_state/delete_device", .method = HTTP_POST,
      .handler = system_station_delete_device_post_handler,
      .auth = CPE_WEB_AUTH_MUTABLE },
    { .uri = "/system/station_state/change_name", .method = HTTP_POST,
      .handler = system_station_change_name_post_handler,
      .auth = CPE_WEB_AUTH_MUTABLE },
};

#define CPE_WEB_ROUTE_COUNT (sizeof(s_routes) / sizeof(s_routes[0]))

/* The catch-all static handler takes one more slot in the httpd table. */
_Static_assert(CPE_WEB_ROUTE_COUNT + 1 <= CPE_WEB_HTTPD_MAX_URI_HANDLERS,
               "Route table does not fit into CPE_WEB_HTTPD_MAX_URI_HANDLERS");
_Static_assert(CPE_WEB_ROUTE_COUNT + 1 <= CPE_WEB_ROUTE_LOG_MAX,
               "Route table does not fit into CPE_WEB_ROUTE_LOG_MAX");

/* Uniform session gate in front of every non-public route. GET routes are
 * fully authenticated here (body rejection included); POST routes still run
 * cpe_web_post_begin() in the handler, because that is what reads the body and
 * hands the session token to the handler. Duplicating the session check keeps
 * a new endpoint from being reachable when its handler forgets it. */
static esp_err_t route_gate(httpd_req_t *req, const cpe_web_route_t *route)
{
    if (route->auth == CPE_WEB_AUTH_PUBLIC) {
        return ESP_OK;
    }

    if (route->method == HTTP_GET) {
        static const cpe_web_get_opts_t session_opts = {
            .session = CPE_WEB_GET_SESSION_REQUIRED,
            .csrf = false,
        };
        return cpe_web_get_begin(req, &session_opts, NULL);
    }

    char session[CPE_WEB_SESSION_BUF_LEN] = { 0 };
    esp_err_t err = cpe_web_require_session(req, session, false);
    if (err != ESP_OK) {
        return err;
    }
    cpe_web_secure_wipe(session, sizeof(session));

    if (route->auth == CPE_WEB_AUTH_MUTABLE && cpe_config_admin_must_change()) {
        return cpe_web_send_error(req, "password_change_required");
    }
    return ESP_OK;
}

/* Registered as the handler for every entry; req->user_ctx carries the table
 * entry (cpe_web_register_uris() restores it when request logging is on). */
static esp_err_t route_dispatch(httpd_req_t *req)
{
    const cpe_web_route_t *route = (const cpe_web_route_t *)req->user_ctx;
    if (route == NULL || route->handler == NULL) {
        ESP_LOGE(TAG, "No route entry for %s", req->uri);
        return cpe_web_send_error(req, "internal_error");
    }

    esp_err_t err = route_gate(req, route);
    if (err != ESP_OK) {
        return err;
    }
    return route->handler(req);
}

esp_err_t cpe_web_routes_register(httpd_handle_t server,
                                  cpe_web_rest_context_t *context)
{
    for (size_t i = 0; i < CPE_WEB_ROUTE_COUNT; i++) {
        httpd_uri_t uri = {
            .uri = s_routes[i].uri,
            .method = s_routes[i].method,
            .handler = route_dispatch,
            .user_ctx = (void *)&s_routes[i],
        };
        esp_err_t err = cpe_web_register_uris(server, &uri, 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s", s_routes[i].uri);
            return err;
        }
    }

    /* The catch-all must be registered last: it is the lowest priority match. */
    return cpe_web_static_files_register(server, context);
}
