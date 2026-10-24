/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SoftAP start-up / runtime reconfiguration for the ESP-CPE product.
 *
 * Security notes:
 *  - No credential is compiled in: SSID and WPA2 password come from
 *    cpe_config (per-device random credential generated on first boot).
 *  - The AP is always WPA2-PSK, an empty password can never downgrade the AP
 *    to WIFI_AUTH_OPEN.
 *  - No password is ever printed. Only the SSID (public by definition) is
 *    logged.
 *  - All IDF calls are checked, never wrapped in ESP_ERROR_CHECK, so an
 *    invalid stored value results in a returned error instead of abort().
 *  - DNS and NAPT are deliberately not configured here: components/cpe_wan
 *    applies the upstream DNS and enables NAPT once the 4G link is online.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/ip_addr.h"

#include "cpe_config.h"
#include "app_wifi.h"

static const char *TAG = "app_wifi";

#define APP_WIFI_BW_20              WIFI_BW20
#define APP_WIFI_BW_40              WIFI_BW40

/* One hour instead of the IDF default of two: a shorter lease frees a stale
 * address sooner, which matters because the DHCP server deletes the oldest
 * lease without notification once its table is full. */
#define APP_WIFI_DHCP_LEASE_S       3600

/* The lease table must be able to hold every station the SoftAP accepts,
 * otherwise the DHCP server silently recycles the oldest address while that
 * station is still using it (two clients end up with the same IP). */
#if CONFIG_LWIP_DHCPS_MAX_STATION_NUM < CPE_MAX_STA_MAX
#error "CONFIG_LWIP_DHCPS_MAX_STATION_NUM must be >= CPE_MAX_STA_MAX"
#endif

/* Bring-up is a fixed sequence, so the state is one value instead of three
 * flags that could disagree: init -> event handler -> started. app_wifi_stop()
 * unwinds it in reverse, and every step is skipped when it was never reached. */
typedef enum {
    APP_WIFI_STOPPED = 0,
    APP_WIFI_INITED,    /* esp_wifi_init() done */
    APP_WIFI_HANDLERS,  /* Wi-Fi event handler registered */
    APP_WIFI_STARTED,   /* the SoftAP is up */
} app_wifi_state_t;

static app_wifi_state_t s_state = APP_WIFI_STOPPED;
static esp_netif_t *s_ap_netif;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static wifi_bandwidth_t to_bandwidth(uint8_t bandwidth)
{
    return (bandwidth == CPE_BANDWIDTH_40) ? APP_WIFI_BW_40 : APP_WIFI_BW_20;
}

/**
 * @brief Validate a cpe_config_t AP section and translate it into a
 *        wifi_ap_config_t. Returns ESP_ERR_INVALID_ARG on any bad value.
 */
static esp_err_t build_ap_config(const cpe_config_t *cfg, wifi_config_t *out)
{
    if (cfg == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Only the AP part: the device name and the admin user have nothing to do
     * with the radio, they must not stop the SoftAP from coming up. */
    if (cpe_config_validate_ap(cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Refusing invalid AP configuration");
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    size_t ssid_len = strlen(cfg->ap_ssid);
    /* ssid[] is not NUL terminated when the SSID is exactly 32 bytes, so copy
     * explicitly and set ssid_len (do not rely on strlcpy here). */
    memcpy(out->ap.ssid, cfg->ap_ssid, ssid_len);
    out->ap.ssid_len = (uint8_t)ssid_len;
    memcpy(out->ap.password, cfg->ap_password, strlen(cfg->ap_password));
    /* Never allow an open AP: cpe_config guarantees 8..63 printable bytes. */
    out->ap.authmode = WIFI_AUTH_WPA2_PSK;
    out->ap.channel = cfg->ap_channel;
    out->ap.ssid_hidden = cfg->ap_hidden ? 1 : 0;
    out->ap.max_connection = cfg->max_sta;
    return ESP_OK;
}

/**
 * @brief Apply an optional non-default AP IPv4 address (CONFIG_SERVER_IP).
 *
 * The default 192.168.4.1 is already configured by esp_netif, so the DHCP
 * server is untouched in the common case. Errors are returned instead of being
 * turned into aborts.
 */
static esp_err_t ap_apply_static_ip(esp_netif_t *ap_netif)
{
    if (strcmp(CONFIG_SERVER_IP, "192.168.4.1") == 0) {
        return ESP_OK;
    }

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = ipaddr_addr(CONFIG_SERVER_IP);
    if (ip.ip.addr == IPADDR_NONE || ip.ip.addr == 0) {
        ESP_LOGE(TAG, "CONFIG_SERVER_IP \"%s\" is not a valid IPv4 address", CONFIG_SERVER_IP);
        return ESP_ERR_INVALID_ARG;
    }
    ip.gw.addr = ip.ip.addr;
    ip.netmask.addr = ipaddr_addr("255.255.255.0");

    esp_err_t err = esp_netif_dhcps_stop(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "Failed to stop AP DHCP server: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_netif_set_ip_info(ap_netif, &ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AP IP info: %s", esp_err_to_name(err));
        (void)esp_netif_dhcps_start(ap_netif);
        return err;
    }
    err = esp_netif_dhcps_start(ap_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restart AP DHCP server: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "AP IP address set to %s", CONFIG_SERVER_IP);
    return ESP_OK;
}

/**
 * @brief Apply the DHCP lease time used for the SoftAP clients.
 *
 * The lease time can only be changed while the server is stopped, and it must
 * be applied after ap_apply_static_ip(): restarting the server keeps the option
 * but not a previous change made on another instance.
 */
/* DHCP options that can only be written while the server is stopped:
 * esp_netif_dhcps_option(ESP_NETIF_OP_SET, ...) refuses every change once the
 * server runs (ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED). This is called before
 * esp_wifi_start(), so no station is connected and there is no lease table to
 * preserve - unlike a runtime stop/start, which frees the lease table and makes
 * the server hand out addresses that are still in use.
 *
 * OFFER_DNS is enabled once here; the WAN side only refreshes the DNS *address*
 * at runtime with esp_netif_set_dns_info(), which does work on a running
 * server. */
static esp_err_t ap_configure_dhcp_server(esp_netif_t *ap_netif)
{
    dhcps_time_t lease_time = APP_WIFI_DHCP_LEASE_S;

    esp_err_t err = esp_netif_dhcps_stop(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "Failed to stop AP DHCP server: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                 ESP_NETIF_IP_ADDRESS_LEASE_TIME,
                                 &lease_time, sizeof(lease_time));
    if (err != ESP_OK) {
        /* Non-fatal: the server keeps the IDF default lease time. */
        ESP_LOGW(TAG, "Failed to set the DHCP lease time: %s", esp_err_to_name(err));
    }

    dhcps_offer_t offer_dns = OFFER_DNS;
    esp_err_t dns_err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                              ESP_NETIF_DOMAIN_NAME_SERVER,
                                              &offer_dns, sizeof(offer_dns));
    if (dns_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable the DHCP DNS option: %s", esp_err_to_name(dns_err));
    }

    esp_err_t start_err = esp_netif_dhcps_start(ap_netif);
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restart AP DHCP server: %s", esp_err_to_name(start_err));
        return start_err;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "AP DHCP lease time set to %d s", APP_WIFI_DHCP_LEASE_S);
    }
    return err;
}

/* ------------------------------------------------------------------------- */
/* Wi-Fi events                                                              */
/* ------------------------------------------------------------------------- */

static void app_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d", MAC2STR(event->mac), event->aid);
            break;
        }
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

/*
 * Give the SoftAP a fresh BSSID after an abrupt reset.
 *
 * A reset that never runs our restart path (reset button, panic, watchdog,
 * brownout, quick power cycle) cannot deauthenticate the stations first. If the
 * AP then comes back with the same BSSID a moment later, a station (notably
 * Windows) may keep its old association and stale keys: beacons from the same
 * BSSID keep the link "alive", the AP drops every frame, and the web console
 * stays unreachable until the user reconnects manually. A new locally
 * administered BSSID makes the old association unverifiable, so the station
 * detects the loss and reconnects by itself.
 *
 * Controlled restarts already call app_wifi_deauth_all_stations() and keep the
 * factory BSSID.
 */
static void use_fresh_bssid_after_abrupt_reset(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_SW || reason == ESP_RST_DEEPSLEEP) {
        return;
    }

    uint8_t mac[6];
    uint32_t rnd = esp_random();
    mac[0] = 0x02; /* locally administered, unicast */
    mac[1] = (uint8_t)(rnd >> 24);
    mac[2] = (uint8_t)(rnd >> 16);
    mac[3] = (uint8_t)(rnd >> 8);
    mac[4] = (uint8_t)rnd;
    mac[5] = (uint8_t)(esp_random() >> 24);

    esp_err_t err = esp_wifi_set_mac(WIFI_IF_AP, mac);
    if (err != ESP_OK) {
        /* Non-fatal: keep the factory BSSID and start the AP anyway. */
        ESP_LOGW(TAG, "Failed to use a fresh AP BSSID (%s), keeping the factory MAC",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Abrupt reset detected, using BSSID " MACSTR " for this boot",
             MAC2STR(mac));
}

void app_wifi_stop(void)
{
    if (s_state == APP_WIFI_STOPPED && s_ap_netif == NULL) {
        return;
    }
    if (s_state == APP_WIFI_STARTED) {
        (void)esp_wifi_stop();
    }
    if (s_state >= APP_WIFI_HANDLERS) {
        (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, app_wifi_event_handler);
    }
    if (s_state >= APP_WIFI_INITED) {
        (void)esp_wifi_deinit();
    }
    if (s_ap_netif != NULL) {
        /* Created by esp_netif_create_default_wifi_ap(): use the matching
         * destructor so the default handler and DHCP state go with it. */
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    s_state = APP_WIFI_STOPPED;
    ESP_LOGI(TAG, "SoftAP torn down");
}

esp_err_t app_wifi_start(void)
{
    if (s_state == APP_WIFI_STARTED) {
        return ESP_OK;
    }

    cpe_config_t cfg;
    esp_err_t err = cpe_config_get(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read CPE configuration: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t ap_cfg;
    err = build_ap_config(&cfg, &ap_cfg);
    if (err != ESP_OK) {
        return err;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create AP netif");
        return ESP_ERR_NO_MEM;
    }

    err = ap_apply_static_ip(s_ap_netif);
    if (err != ESP_OK) {
        goto fail;
    }

    /* Bounded lease + a table big enough for max_sta: see the #error above. */
    err = ap_configure_dhcp_server(s_ap_netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Continuing with the default DHCP lease time");
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        goto fail;
    }
    s_state = APP_WIFI_INITED;

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, app_wifi_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register Wi-Fi event handler: %s", esp_err_to_name(err));
        goto fail;
    }
    s_state = APP_WIFI_HANDLERS;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* Must happen before esp_wifi_start(): esp_wifi_set_mac() only works while
     * the interface is disabled. */
    use_fresh_bssid_after_abrupt_reset();

    /* cpe_config is the single source of truth: do not let the Wi-Fi driver
     * keep a second plaintext copy of the credentials in NVS. */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage failed: %s", esp_err_to_name(err));
        goto fail;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply AP configuration: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* Bandwidth is a per-interface setting, not part of wifi_ap_config_t. */
    esp_err_t bw_err = esp_wifi_set_bandwidth(WIFI_IF_AP, to_bandwidth(cfg.ap_bandwidth));
    if (bw_err != ESP_OK) {
        /* Non fatal: the AP keeps running with its previous bandwidth. */
        ESP_LOGW(TAG, "Failed to set AP bandwidth: %s", esp_err_to_name(bw_err));
    }

    s_state = APP_WIFI_STARTED;
    ESP_LOGI(TAG, "SoftAP started: SSID \"%s\", channel %u, %u MHz, WPA2-PSK, hidden=%d, max_sta=%u",
             cfg.ap_ssid, (unsigned)cfg.ap_channel, (unsigned)cfg.ap_bandwidth,
             cfg.ap_hidden ? 1 : 0, (unsigned)cfg.max_sta);
    return ESP_OK;

fail:
    /* Unwind whatever was reached: stop() skips the steps that were not. */
    app_wifi_stop();
    return err;
}

esp_err_t app_wifi_apply(const cpe_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != APP_WIFI_STARTED) {
        ESP_LOGE(TAG, "Cannot apply AP configuration before app_wifi_start()");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t new_cfg;
    esp_err_t err = build_ap_config(cfg, &new_cfg);
    if (err != ESP_OK) {
        return err;
    }

    /* Keep the currently effective configuration so a failed apply can be
     * rolled back (the caller persists the new configuration only after this
     * function returned ESP_OK). */
    cpe_config_t prev;
    err = cpe_config_get(&prev);
    if (err != ESP_OK) {
        return err;
    }
    wifi_config_t prev_cfg;
    if (build_ap_config(&prev, &prev_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Current AP configuration is invalid, cannot roll back safely");
        return ESP_ERR_INVALID_STATE;
    }
    wifi_bandwidth_t prev_bw = to_bandwidth(prev.ap_bandwidth);
    wifi_bandwidth_t new_bw = to_bandwidth(cfg->ap_bandwidth);

    /* Changing any of these fields makes the driver restart the SoftAP with the
     * same BSSID. A station that keeps its association (Windows in particular)
     * then transmits with stale keys while the AP drops every frame, so the
     * console looks unreachable until the user reconnects manually. Send a
     * deauth first: it makes the stations really let go and rescan instead of
     * assuming the link is still alive. Channel/bandwidth changes are excluded
     * on purpose (the AP follows them without a restart). */
    bool radio_restart = (strcmp(prev.ap_ssid, cfg->ap_ssid) != 0) ||
                         (strcmp(prev.ap_password, cfg->ap_password) != 0) ||
                         (prev.ap_hidden != cfg->ap_hidden) ||
                         (prev.max_sta != cfg->max_sta);
    if (radio_restart) {
        ESP_LOGI(TAG, "SoftAP will restart, deauthenticating the stations first");
        app_wifi_deauth_all_stations();
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &new_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply new AP configuration: %s", esp_err_to_name(err));
        return err;
    }

    /* A running AP needs esp_wifi_set_channel() for the channel change to take
     * effect; the channel field of wifi_ap_config_t is only applied on start. */
    if (cfg->ap_channel != prev.ap_channel) {
        esp_err_t ch_err = esp_wifi_set_channel(cfg->ap_channel, WIFI_SECOND_CHAN_NONE);
        if (ch_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to apply new AP channel: %s", esp_err_to_name(ch_err));
            if (esp_wifi_set_config(WIFI_IF_AP, &prev_cfg) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restore previous AP configuration");
            }
            return ch_err;
        }
    }

    if (new_bw != prev_bw) {
        esp_err_t bw_err = esp_wifi_set_bandwidth(WIFI_IF_AP, new_bw);
        if (bw_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to apply new AP bandwidth: %s", esp_err_to_name(bw_err));
            /* Roll back both the bandwidth and the AP configuration. */
            (void)esp_wifi_set_bandwidth(WIFI_IF_AP, prev_bw);
            esp_err_t restore_err = esp_wifi_set_config(WIFI_IF_AP, &prev_cfg);
            if (restore_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restore previous AP configuration: %s",
                         esp_err_to_name(restore_err));
            }
            if (cfg->ap_channel != prev.ap_channel) {
                (void)esp_wifi_set_channel(prev.ap_channel, WIFI_SECOND_CHAN_NONE);
            }
            return bw_err;
        }
    }

    ESP_LOGI(TAG, "SoftAP configuration applied: SSID \"%s\", channel %u, %u MHz, hidden=%d, max_sta=%u",
             cfg->ap_ssid, (unsigned)cfg->ap_channel, (unsigned)cfg->ap_bandwidth,
             cfg->ap_hidden ? 1 : 0, (unsigned)cfg->max_sta);
    return ESP_OK;
}

esp_netif_t *app_wifi_get_ap_netif(void)
{
    return s_ap_netif;
}

void app_wifi_deauth_all_stations(void)
{
    esp_err_t err = esp_wifi_deauth_sta(0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to deauthenticate stations before restart: %s",
                 esp_err_to_name(err));
        return;
    }
    /* Let the deauth frame(s) reach the air before the caller resets the radio.
     * The frame is handed to the radio synchronously, so a short settle delay is
     * enough. */
    vTaskDelay(pdMS_TO_TICKS(100));
}

esp_err_t app_wifi_sta_disconnect(const uint8_t mac[6])
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t aid = 0;
    esp_err_t err = esp_wifi_ap_get_sta_aid(mac, &aid);
    if (err != ESP_OK) {
        /* ESP_ERR_NOT_FOUND: the station left on its own in the meantime. */
        ESP_LOGW(TAG, "Station " MACSTR " is not associated: %s",
                 MAC2STR(mac), esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_deauth_sta(aid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deauthenticate " MACSTR ": %s",
                 MAC2STR(mac), esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Station " MACSTR " disconnected on request", MAC2STR(mac));
    return ESP_OK;
}
