/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Low-level helpers shared by every module of the management web server.
 */

#include "esp_log.h"

#include "http_internal.h"

#if CONFIG_CPE_WEB_HTTP_LOG
#include <stdio.h>

#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#endif

static const char *TAG = "cpe_web";

/* Overwrite sensitive buffers so credentials do not linger on the stack. */
void cpe_web_secure_wipe(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) {
        *p++ = 0;
    }
}

#if CONFIG_CPE_WEB_HTTP_LOG

/* One slot per registered URI. Registration runs once per httpd instance and
 * again when the listener is re-created after a SoftAP restart, so the table is
 * reset by cpe_web_route_log_reset() before each registration round. */
#define CPE_WEB_ROUTE_LOG_MAX 24

typedef struct {
    httpd_uri_t uri;                 /* copy actually registered (handler wrapped) */
    esp_err_t (*handler)(httpd_req_t *);
    void *user_ctx;
} cpe_web_route_log_t;

static cpe_web_route_log_t s_route_log[CPE_WEB_ROUTE_LOG_MAX];
static size_t s_route_log_count;

static const char *http_method_name(httpd_method_t method)
{
    switch (method) {
    case HTTP_GET:
        return "GET";
    case HTTP_POST:
        return "POST";
    case HTTP_PUT:
        return "PUT";
    case HTTP_DELETE:
        return "DELETE";
    case HTTP_HEAD:
        return "HEAD";
    case HTTP_OPTIONS:
        return "OPTIONS";
    case HTTP_PATCH:
        return "PATCH";
    default:
        return "?";
    }
}

static void format_client_ip(httpd_req_t *req, char *out, size_t out_len)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    if (fd >= 0 && getpeername(fd, (struct sockaddr *)&addr, &addr_len) == 0) {
        esp_ip4_addr_t ip = { .addr = addr.sin_addr.s_addr };
        esp_ip4addr_ntoa(&ip, out, out_len);
    } else {
        snprintf(out, out_len, "unknown");
    }
}

/* Wrapper around a registered handler: logs the request, its client and how
 * long the handler took. The original user_ctx is restored for the real
 * handler (the static file handler needs it). */
static esp_err_t http_log_handler(httpd_req_t *req)
{
    cpe_web_route_log_t *route = (cpe_web_route_log_t *)req->user_ctx;
    if (route == NULL || route->handler == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char client[16];
    format_client_ip(req, client, sizeof(client));
    const char *method = http_method_name(route->uri.method);
    int64_t start_us = esp_timer_get_time();

    ESP_LOGI(TAG, "HTTP --> %s %s from %s", method, req->uri, client);

    /* The real handler must see the context it was registered with. */
    req->user_ctx = route->user_ctx;
    esp_err_t err = route->handler(req);
    req->user_ctx = route;

    ESP_LOGI(TAG, "HTTP <-- %s %s %s (%lld ms)", method, req->uri,
             err == ESP_OK ? "ok" : esp_err_to_name(err),
             (long long)((esp_timer_get_time() - start_us) / 1000));
    return err;
}

#endif /* CONFIG_CPE_WEB_HTTP_LOG */

void cpe_web_route_log_reset(void)
{
#if CONFIG_CPE_WEB_HTTP_LOG
    s_route_log_count = 0;
#endif
}

esp_err_t cpe_web_register_uris(httpd_handle_t server, const httpd_uri_t *uris,
                                size_t count)
{
    for (size_t i = 0; i < count; i++) {
#if CONFIG_CPE_WEB_HTTP_LOG
        if (s_route_log_count >= CPE_WEB_ROUTE_LOG_MAX) {
            ESP_LOGE(TAG, "HTTP request log table is full (%d entries)",
                     CPE_WEB_ROUTE_LOG_MAX);
            return ESP_ERR_NO_MEM;
        }
        cpe_web_route_log_t *route = &s_route_log[s_route_log_count++];
        route->handler = uris[i].handler;
        route->user_ctx = uris[i].user_ctx;
        route->uri = uris[i];
        route->uri.handler = http_log_handler;
        route->uri.user_ctx = route;
        esp_err_t err = httpd_register_uri_handler(server, &route->uri);
#else
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
#endif
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register handler %s: %s",
                     uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
