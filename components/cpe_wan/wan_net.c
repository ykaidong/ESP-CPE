/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * WAN link events and the AP-side network services (NAPT, upstream DNS).
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

static const char *TAG = "cpe_net";

static void wan_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IOT_ETH_EVENT) {
        /* Do NOT gate on s_ecm.ready: the ECM driver may report the link state
         * while the netif/glue is still being created. Dropping those events
         * would lose the link-up forever because the driver suppresses
         * duplicate reports (review P2-9). Only status is touched here.
         * The payload is the handle that raised the event (&eth): ignore
         * instances that are not the current ECM driver. */
        iot_eth_handle_t *sender = (iot_eth_handle_t *)data;
        if (sender == NULL || *sender != wan_ecm_eth_handle()) {
            return;
        }
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
            /* An IPv4 string is 15 characters plus the terminator: the field
             * has slack, and a truncated address is never published. */
            char ip[20];
            int n = snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
            if (n < 0 || (size_t)n >= sizeof(ip)) {
                ESP_LOGW(TAG, "WAN IP string does not fit, not publishing it");
            } else {
                ESP_LOGI(TAG, "4G WAN IP: %s", ip);
                cpe_status_set_ip(ip);
            }
            cpe_status_set_error(NULL);
        } else if (id == IP_EVENT_ETH_LOST_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
            if (event->esp_netif != wan_ecm_netif()) {
                return;
            }
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
    if (ap == NULL) {
        return;
    }
    /* The ECM netif belongs to the link manager task: borrow it, so a teardown
     * cannot destroy it while its DNS address is read here. wan_apply_upstream_dns()
     * runs on ap_reapply_task (and on the link manager task), never on the event
     * loop, but the borrow is non-blocking either way. */
    if (!wan_ecm_borrow()) {
        return;
    }
    esp_netif_t *wan = wan_ecm_netif();
    esp_netif_dns_info_t dns = {0};
    bool have_dns = (wan != NULL &&
                     esp_netif_get_dns_info(wan, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
                     dns.ip.u_addr.ip4.addr != 0);
    wan_ecm_release();

    if (!have_dns) {
        ESP_LOGW(TAG, "No upstream DNS from the 4G modem");
        return;
    }

    /* The server is NEVER stopped here: dhcps_stop() frees the whole lease
     * table and dhcps_start() resets the allocation cursor to the first
     * address, so a station that is already using an address keeps it while the
     * server hands the very same address out to the next client: two stations
     * end up with one IP (and the station registry reports "Duplicate IP").
     *
     * That also means OFFER_DNS cannot be enabled here: OP_SET is refused with
     * ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED while the server runs. app_wifi
     * enables it once before the server starts; below only the DNS *address* is
     * refreshed, which does work on a running server. */
    /* OP_GET reports a boolean (1 = the OFFER_DNS bit is set), not the
     * dhcps_offer_t mask, so compare against 0. */
    uint8_t offer_dns = 0;
    esp_err_t err = esp_netif_dhcps_option(ap, ESP_NETIF_OP_GET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_dns, sizeof(offer_dns));
    if (err != ESP_OK || offer_dns == 0) {
        ESP_LOGW(TAG, "AP DHCP server does not offer DNS (option %u, %s)",
                 (unsigned)offer_dns, esp_err_to_name(err));
    }

    /* Nothing to do when the value is already the one the server offers: this
     * runs on every 4G reconnect and on every SoftAP restart. */
    esp_netif_dns_info_t cur = {0};
    if (esp_netif_get_dns_info(ap, ESP_NETIF_DNS_MAIN, &cur) == ESP_OK &&
            cur.ip.u_addr.ip4.addr == dns.ip.u_addr.ip4.addr) {
        return;
    }

    err = esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply upstream DNS to AP DHCP: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "AP clients now use upstream DNS " IPSTR, IP2STR(&dns.ip.u_addr.ip4));

    /* A client only refreshes its options at T1 (half of the 3600 s lease), so
     * without this it would keep the old DNS for about 30 minutes. Forcing a
     * re-association makes it re-request its lease: the lease table survived,
     * so every station keeps its address and only the options change. */
    wifi_sta_list_t sta_list = { 0 };
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num > 0) {
        ESP_LOGI(TAG, "Deauthenticating %u station(s) so they pick up the new DNS",
                 (unsigned)sta_list.num);
        app_wifi_deauth_all_stations();
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

/* Asked for when the SoftAP restart handler could not create its task: the work
 * blocks on the TCP/IP thread and deauthenticates stations, so it must not run
 * in the event loop. The link manager owns the ECM handles and executes it. */
static bool s_napt_reapply_req;

void wan_request_napt_reapply(void)
{
    __atomic_store_n(&s_napt_reapply_req, true, __ATOMIC_RELEASE);
}

bool wan_napt_reapply_take(void)
{
    return __atomic_exchange_n(&s_napt_reapply_req, false, __ATOMIC_ACQ_REL);
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

/* Re-applying NAPT/DNS blocks on the TCP/IP thread (esp_netif_get_ip_info(),
 * esp_netif_get/set_dns_info()) and deauthenticates every station, so it must
 * not run in the event loop task: everything else posted to the default event
 * loop (Wi-Fi, IP, link manager notifications) would wait behind it. */
#define AP_REAPPLY_TASK_STACK        4096
#define AP_REAPPLY_TASK_PRIO         5

static void ap_reapply_task(void *arg)
{
    (void)arg;
    wan_link_online();
    vTaskDelete(NULL);
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
    if (xTaskCreate(ap_reapply_task, "cpe_ap_reapply", AP_REAPPLY_TASK_STACK, NULL,
                    AP_REAPPLY_TASK_PRIO, NULL) != pdPASS) {
        /* Not from here: this is the event loop task, and the work blocks on the
         * TCP/IP thread. The link manager owns the ECM handles and does it on
         * its next tick. */
        ESP_LOGW(TAG, "No memory for the NAPT task, asking the link manager to re-apply");
        wan_request_napt_reapply();
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t cpe_wan_events_register(void)
{
    esp_err_t first_err = ESP_OK;
    esp_err_t err = esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID,
                                               wan_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IOT_ETH event handler: %s", esp_err_to_name(err));
        first_err = err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, wan_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH_GOT_IP handler: %s", esp_err_to_name(err));
        if (first_err == ESP_OK) {
            first_err = err;
        }
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, wan_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH_LOST_IP handler: %s", esp_err_to_name(err));
        if (first_err == ESP_OK) {
            first_err = err;
        }
    }
    return first_err;
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
