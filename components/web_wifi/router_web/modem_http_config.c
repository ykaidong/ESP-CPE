/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Embedded web management server for the ESP-CPE product.
 *
 * This file only owns the server lifecycle. The protocol helpers, JSON layer,
 * station registry and API endpoints live in their own modules; the security
 * invariants they implement are documented at their definitions:
 *
 *  - session cookie (CPE_SID, HttpOnly + SameSite=Strict) + X-CSRF-Token,
 *    same-origin checks and body limits: http_common.c
 *  - JSON escaping / escaping-aware parsing and the depth limit: json_util.c
 *  - station list mutex and nickname validation: station_registry.c
 *  - endpoint-specific validation and rollback: api_*.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <net/if.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"

#include "app_wifi.h"
#include "cpe_auth.h"
#include "http_internal.h"
#include "modem_http_config.h"

static const char *TAG = "cpe_web";

/* Set once modem_http_start() completed; guards event handlers that may race
 * with modem_http_stop(). */
static bool s_started;
static bool s_events_registered;
static bool s_listener_restart_pending;

static httpd_handle_t s_server;
static cpe_web_rest_context_t *s_rest_context;

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
        ESP_LOGI(TAG, "SPIFFS mounted: total %u, used %u",
                 (unsigned)total, (unsigned)used);
    } else {
        ESP_LOGW(TAG, "Failed to read SPIFFS info: %s", esp_err_to_name(ret));
    }
    return ESP_OK;
}

static void free_rest_context(void)
{
    free(s_rest_context);
    s_rest_context = NULL;
}

static esp_err_t register_api_handlers(httpd_handle_t server)
{
    esp_err_t err = cpe_web_api_auth_register(server);
    if (err == ESP_OK) {
        err = cpe_web_api_status_register(server);
    }
    if (err == ESP_OK) {
        err = cpe_web_api_system_register(server);
    }
    if (err == ESP_OK) {
        err = cpe_web_api_ota_register(server);
    }
    if (err == ESP_OK) {
        err = cpe_web_api_wlan_register(server);
    }
    if (err == ESP_OK) {
        err = cpe_web_api_station_register(server);
    }
    if (err == ESP_OK) {
        err = cpe_web_static_files_register(server, s_rest_context);
    }
    return err;
}

/* Create the HTTP server and bind its listening socket to the SoftAP netif.
 * Called at startup and again after a SoftAP restart (see below). */
static esp_err_t start_http_server(void)
{
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
        return ESP_ERR_INVALID_STATE;
    }
    config.if_name = &ap_ifr;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    err = register_api_handlers(s_server);
    if (err != ESP_OK) {
        (void)httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Web server listening on SoftAP interface \"%s\" port %d",
             ap_ifr.ifr_name, config.server_port);
    return ESP_OK;
}

/* Reconfiguring the SoftAP (SSID, password, channel, bandwidth, hidden SSID,
 * max clients) restarts the Wi-Fi AP. The listening socket is bound to that
 * netif, and lwIP drops the binding when the interface goes down: afterwards
 * the device answers ICMP but TCP connects to port 80 are refused. httpd only
 * logs the accept error and never re-creates the socket, so the listener has
 * to be rebuilt once the AP is back. */
static void http_listener_restart_task(void *arg)
{
    /* Wait for the restarted AP to bring its netif and DHCP server up. */
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (s_started && s_server != NULL) {
        httpd_handle_t old = s_server;
        s_server = NULL;
        esp_err_t err = httpd_stop(old);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "httpd_stop during listener restart failed: %s",
                     esp_err_to_name(err));
        }
        /* Retry in case the netif is still coming up. Keeping s_started set
         * lets a later modem_http_stop() unregister the handlers and release
         * the station registry even if every attempt fails. */
        for (int attempt = 0; attempt < 3; attempt++) {
            err = start_http_server();
            if (err == ESP_OK) {
                break;
            }
            ESP_LOGE(TAG, "Failed to re-create the HTTP listener (attempt %d): %s",
                     attempt + 1, esp_err_to_name(err));
            if (attempt < 2) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
    s_listener_restart_pending = false;
    vTaskDelete(NULL);
}

static void on_wifi_ap_start(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    if (!s_started || s_listener_restart_pending) {
        return;
    }
    s_listener_restart_pending = true;
    if (xTaskCreate(http_listener_restart_task, "cpe_http_relisten",
                    4096, NULL, 5, NULL) != pdPASS) {
        s_listener_restart_pending = false;
        ESP_LOGE(TAG, "No memory to schedule HTTP listener re-creation");
    }
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
    err = cpe_web_station_registry_init();
    if (err != ESP_OK) {
        goto fail;
    }

    /* Mark before the first registration so the error path disables the
     * handler even if only one of the two registrations succeeded
     * (unregistering a handler that was never added is harmless). */
    cpe_web_station_registry_enable(true);
    s_events_registered = true;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     cpe_web_station_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register Wi-Fi event handler: %s",
                 esp_err_to_name(err));
        goto fail;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                     cpe_web_station_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s",
                 esp_err_to_name(err));
        goto fail;
    }
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START,
                                     on_wifi_ap_start, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register AP start handler: %s",
                 esp_err_to_name(err));
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

    err = start_http_server();
    if (err != ESP_OK) {
        goto fail;
    }

    s_started = true;
    ESP_LOGI(TAG, "Management web server started");
    return ESP_OK;

fail:
    if (s_server != NULL) {
        (void)httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_events_registered) {
        cpe_web_station_registry_enable(false);
        (void)esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_START,
                                           on_wifi_ap_start);
        (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                           cpe_web_station_event_handler);
        (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                           cpe_web_station_event_handler);
        s_events_registered = false;
    }
    free_rest_context();
    cpe_web_station_registry_deinit();
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
        cpe_web_station_registry_enable(false);
        (void)esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_START,
                                           on_wifi_ap_start);
        (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                           cpe_web_station_event_handler);
        (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                           cpe_web_station_event_handler);
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
    cpe_web_station_registry_deinit();

    ESP_LOGI(TAG, "Management web server stopped");
    return ESP_OK;
}
