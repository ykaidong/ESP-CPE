/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * WAN link events and the AP-side network services (NAPT, upstream DNS).
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"

#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip_addr.h"
#include "lwip/lwip_napt.h"

#include "app_wifi.h"
#include "cpe_core.h"
#include "iot_eth.h"

#include "cpe_wan.h"
#include "wan_internal.h"

static const char *TAG = "cpe";

static void wan_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IOT_ETH_EVENT) {
        /* Do NOT gate on s_ecm.ready: the ECM driver may report the link state
         * while the netif/glue is still being created. Dropping those events
         * would lose the link-up forever because the driver suppresses
         * duplicate reports (review P2-9). Only status is touched here. */
        if (id == IOT_ETH_EVENT_CONNECTED) {
            ESP_LOGI(TAG, "4G link up");
            cpe_status_set_usb_connected(true);
            cpe_status_set_link(true);
            cpe_status_set_error(NULL);
        } else if (id == IOT_ETH_EVENT_DISCONNECTED) {
            ESP_LOGW(TAG, "4G link down");
            cpe_status_set_link(false);
            cpe_status_set_usb_connected(false);
            cpe_status_set_ip(NULL);
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_ETH_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
            if (event->esp_netif != wan_ecm_netif()) {
                return;
            }
            char ip[16];
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "4G WAN IP: %s", ip);
            cpe_status_set_ip(ip);
            cpe_status_set_error(NULL);
        } else if (id == IP_EVENT_ETH_LOST_IP) {
            cpe_status_set_ip(NULL);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* CPE helpers: NAPT and DHCP DNS                                           */
/* ------------------------------------------------------------------------- */

static void wan_apply_upstream_dns(void)
{
    esp_netif_t *ap = app_wifi_get_ap_netif();
    esp_netif_t *wan = wan_ecm_netif();
    if (ap == NULL || wan == NULL) {
        return;
    }

    esp_netif_dns_info_t dns = {0};
    if (esp_netif_get_dns_info(wan, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
            dns.ip.u_addr.ip4.addr == 0) {
        ESP_LOGW(TAG, "No upstream DNS from the 4G modem");
        return;
    }

    if (esp_netif_dhcps_stop(ap) != ESP_OK) {
        return;
    }
    dhcps_offer_t offer_dns = OFFER_DNS;
    esp_err_t err = esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_dns, sizeof(offer_dns));
    if (err == ESP_OK) {
        err = esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    }
    if (esp_netif_dhcps_start(ap) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restart AP DHCP server");
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply upstream DNS to AP DHCP: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "AP clients now use upstream DNS " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }
}

static void wan_enable_napt(void)
{
    esp_netif_t *ap = app_wifi_get_ap_netif();
    if (ap == NULL) {
        return;
    }
    esp_netif_ip_info_t ap_ip;
    if (esp_netif_get_ip_info(ap, &ap_ip) != ESP_OK || ap_ip.ip.addr == 0) {
        return;
    }
    ip_napt_enable(ap_ip.ip.addr, 1);
    ESP_LOGI(TAG, "NAPT enabled on " IPSTR, IP2STR(&ap_ip.ip));
}

void wan_link_online(void)
{
    wan_enable_napt();
    wan_apply_upstream_dns();
}

void wan_link_offline(void)
{
    esp_netif_t *ap = app_wifi_get_ap_netif();
    if (ap != NULL) {
        esp_netif_ip_info_t ap_ip;
        if (esp_netif_get_ip_info(ap, &ap_ip) == ESP_OK && ap_ip.ip.addr != 0) {
            ip_napt_enable(ap_ip.ip.addr, 0);
        }
    }
}

/* Reconfiguring the SoftAP restarts it, and the Wi-Fi default handler removes
 * and re-adds the lwIP netif. netif_add() resets netif->napt to 0, and
 * ip_napt_forward() is a no-op unless the input netif has napt set, so every
 * LAN packet would leave the WAN untranslated: the console stays reachable but
 * all internet traffic (including ICMP and DNS) dies. This handler is
 * registered after app_wifi_start(), so the default handler has already put
 * the netif back on netif_list when it runs. */
static void on_ap_start_reapply_napt(void *arg, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    (void)event_data;

    cpe_status_t st;
    cpe_status_get(&st);
    if (!st.got_ip) {
        return;
    }
    ESP_LOGI(TAG, "SoftAP restarted; re-applying NAPT and upstream DNS");
    wan_link_online();
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t cpe_wan_events_register(void)
{
    esp_err_t err = esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID,
                                               wan_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IOT_ETH event handler: %s", esp_err_to_name(err));
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, wan_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH_GOT_IP handler: %s", esp_err_to_name(err));
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, wan_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH_LOST_IP handler: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t cpe_wan_ap_hook_register(void)
{
    esp_err_t err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START,
                                               on_ap_start_reapply_napt, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register AP start NAPT handler: %s",
                 esp_err_to_name(err));
    }
    return err;
}
