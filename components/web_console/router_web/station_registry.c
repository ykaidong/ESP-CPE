/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * In-memory station list fed by Wi-Fi/IP events (audit P1-17).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/queue.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip_addr.h"

#include "app_wifi.h"
#include "http_internal.h"

static const char *TAG = "cpe_web";

typedef struct sta_node {
    SLIST_ENTRY(sta_node) field;
    uint8_t mac[6];
    char name[CPE_WEB_STA_NAME_MAX_LEN + 1];
    esp_ip4_addr_t ip;
    int64_t start_time_us;  /* esp_timer_get_time() when the station connected */
} sta_node_t;

static SLIST_HEAD(sta_list_head, sta_node) s_sta_list = SLIST_HEAD_INITIALIZER(s_sta_list);
static SemaphoreHandle_t s_sta_mutex;
static volatile bool s_events_enabled;

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
    node->start_time_us = esp_timer_get_time();
    if (cpe_web_station_name_load(mac, node->name, sizeof(node->name)) != ESP_OK) {
        cpe_web_mac_to_hex(mac, node->name);
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

    if (xSemaphoreTake(s_sta_mutex, pdMS_TO_TICKS(CPE_WEB_STA_LOCK_TIMEOUT_MS)) != pdTRUE) {
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
    if (xSemaphoreTake(s_sta_mutex, pdMS_TO_TICKS(CPE_WEB_STA_LOCK_TIMEOUT_MS)) != pdTRUE) {
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
    if (xSemaphoreTake(s_sta_mutex, pdMS_TO_TICKS(CPE_WEB_STA_LOCK_TIMEOUT_MS)) != pdTRUE) {
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

bool cpe_web_sta_node_exists(const uint8_t mac[6])
{
    bool found = false;
    if (s_sta_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_sta_mutex, pdMS_TO_TICKS(CPE_WEB_STA_LOCK_TIMEOUT_MS)) != pdTRUE) {
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

bool cpe_web_sta_node_rename(const uint8_t mac[6], const char *name)
{
    bool found = false;
    if (s_sta_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_sta_mutex, pdMS_TO_TICKS(CPE_WEB_STA_LOCK_TIMEOUT_MS)) != pdTRUE) {
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

size_t cpe_web_sta_snapshot(cpe_web_sta_snapshot_t *out, size_t max)
{
    size_t count = 0;
    if (s_sta_mutex == NULL) {
        return 0;
    }
    if (xSemaphoreTake(s_sta_mutex, pdMS_TO_TICKS(CPE_WEB_STA_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return 0;
    }
    sta_node_t *node;
    SLIST_FOREACH(node, &s_sta_list, field) {
        if (count >= max) {
            break;
        }
        snprintf(out[count].name, sizeof(out[count].name), "%s", node->name);
        cpe_web_mac_to_colon(node->mac, out[count].mac);
        esp_ip4_addr_t addr = node->ip;
        if (addr.addr != 0) {
            esp_ip4addr_ntoa(&addr, out[count].ip, sizeof(out[count].ip));
        } else {
            snprintf(out[count].ip, sizeof(out[count].ip), "0.0.0.0");
        }
        out[count].start_time_us = node->start_time_us;
        count++;
    }
    xSemaphoreGive(s_sta_mutex);
    return count;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle and Wi-Fi events                                                */
/* ------------------------------------------------------------------------- */

esp_err_t cpe_web_station_registry_init(void)
{
    if (s_sta_mutex != NULL) {
        return ESP_OK;
    }
    s_sta_mutex = xSemaphoreCreateMutex();
    if (s_sta_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create station list mutex");
        return ESP_ERR_NO_MEM;
    }
    SLIST_INIT(&s_sta_list);
    return ESP_OK;
}

void cpe_web_station_registry_deinit(void)
{
    s_events_enabled = false;
    sta_free_all();
    if (s_sta_mutex != NULL) {
        vSemaphoreDelete(s_sta_mutex);
        s_sta_mutex = NULL;
    }
}

void cpe_web_station_registry_enable(bool enabled)
{
    s_events_enabled = enabled;
}

void cpe_web_station_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    (void)arg;

    if (!s_events_enabled) {
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
