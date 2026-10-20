/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-CPE application entry point.
 *
 * Design notes (see docs/IMPLEMENTATION_PLAN.md):
 *  - SoftAP + Web management start FIRST and are independent from the 4G link,
 *    so the device is always reachable for diagnostics/configuration.
 *  - The USB 4G link is managed by a background task with bounded recovery:
 *    L1 rebuild ECM driver -> L2 root port power cycle -> L3 controlled reboot
 *    (NVS boot counter, cleared once healthy). "Link up but no IP" (no SIM /
 *    PDP not active) never reboots: the device stays in fault mode with AP/Web.
 *  - USB Host + CDC driver are installed once by the CDC component (idempotent)
 *    and are NOT torn down at runtime anymore, which removes the whole
 *    "uninstall -> install -> panic" failure chain found by the audit.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "dhcpserver/dhcpserver.h"
#include "lwip/lwip_napt.h"
#include "usb/usb_host.h"

#include "cpe_core.h"
#include "cpe_ota.h"
#include "app_wifi.h"
#include "cpe_web.h"
#include "reset_button.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_ecm.h"

static const char *TAG = "cpe";

/* Heap watermark helper: free heap alone is not enough to spot a leak or
 * fragmentation, so also log the historical minimum and the largest internal
 * block. Called on link transitions and periodically when enabled. */
static void log_heap(const char *reason)
{
    ESP_LOGI(TAG, "heap (%s): free=%u min=%u largest_internal=%u",
             reason,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

#define ECM_NETIF_KEY       "ECM0"
#define ECM_NETIF_DESC      "usb-ecm"

/* Validate an image that was just installed by OTA.
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the new image boots in
 * PENDING_VERIFY: it has to be marked valid, otherwise the bootloader rolls
 * back on the next reset. AP + Web being up (which is true by the time this
 * task runs) is what counts as "healthy": the 4G link depends on the external
 * modem and cannot be a firmware health criterion. A crash/reboot loop stops
 * the image from ever reaching this point, which is exactly when the rollback
 * is wanted. */
static void ota_validate_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(CONFIG_CPE_OTA_VALIDATE_DELAY_MS));
    (void)cpe_ota_mark_running_valid();
    vTaskDelete(NULL);
}

typedef struct {
    iot_eth_driver_t *driver;
    iot_eth_handle_t eth;
    iot_eth_netif_glue_handle_t glue;
    esp_netif_t *netif;
    bool ready;
} ecm_ctx_t;

static ecm_ctx_t s_ecm;
static bool s_cdc_installed;

/* ------------------------------------------------------------------------- */
/* USB host / CDC driver                                                     */
/* ------------------------------------------------------------------------- */

#if CONFIG_CPE_ENUM_FILTER_ENABLE
static bool cpe_enum_filter_cb(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue)
{
    bool vid_ok = (CONFIG_CPE_ECM_VID == 0) || (dev_desc->idVendor == CONFIG_CPE_ECM_VID);
    bool pid_ok = (CONFIG_CPE_ECM_PID == 0) || (dev_desc->idProduct == CONFIG_CPE_ECM_PID);
    if (vid_ok && pid_ok) {
        *bConfigurationValue = CONFIG_CPE_ECM_CONFIG_VALUE;
        ESP_LOGI(TAG, "Enumeration filter: %04X:%04X -> configuration %d",
                 dev_desc->idVendor, dev_desc->idProduct, CONFIG_CPE_ECM_CONFIG_VALUE);
    }
    return true;
}
#endif

static esp_err_t usb_cdc_install(void)
{
    if (s_cdc_installed) {
        return ESP_OK;
    }

    usbh_cdc_driver_config_t config = {
        .task_stack_size = CONFIG_CPE_USB_TASK_STACK_SIZE,
        .task_priority = CONFIG_CPE_USB_TASK_PRIORITY,
        .task_coreid = CONFIG_CPE_USB_TASK_CORE_ID,
        .skip_init_usb_host_driver = false,
#if CONFIG_CPE_ENUM_FILTER_ENABLE
        .enum_filter_cb = cpe_enum_filter_cb,
#endif
    };

    esp_err_t err = usbh_cdc_driver_install(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usbh_cdc_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    s_cdc_installed = true;
    ESP_LOGI(TAG, "USB CDC driver installed");
    return ESP_OK;
}

/* Release the USB bus by power-cycling the host root port.
 *
 * With an externally powered modem and no module reset line, the modem can keep
 * a stale USB device state after the ESP32 resets: the first enumeration after
 * boot fails with CHECK_SHORT_DEV_DESC and the ESP-IDF enumeration FSM never
 * retries. Powering the root port down deinitializes the HCD port and drops
 * VBUS, which the modem sees as a bus detach and usually answers by resetting
 * its USB device controller; powering the port back up then triggers a clean
 * enumeration.
 *
 * The USB Host library, the CDC driver and its client are deliberately kept
 * installed. Uninstalling/reinstalling them is not needed for a bus detach, and
 * in espressif/usb 1.6.0 usb_host_uninstall() publishes p_host_lib_obj = NULL
 * before the hub/HCD layers are removed, so a root-port interrupt landing in
 * that window calls proc_req_callback() with a NULL library object and panics
 * (LoadProhibited in proc_req_callback, observed 31 s after boot with a stale
 * modem). A power cycle keeps every object alive and avoids that race.
 */
static esp_err_t usb_root_port_power_cycle(uint32_t off_ms)
{
    if (!s_cdc_installed) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = usb_host_lib_set_root_port_power(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB root port power off failed (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "USB root port powered off for %" PRIu32 " ms", off_ms);
    vTaskDelay(pdMS_TO_TICKS(off_ms));

    /* ESP_ERR_INVALID_STATE only means the port is already powered on. */
    err = usb_host_lib_set_root_port_power(true);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "USB root port power on failed (%s)", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* ECM network interface                                                     */
/* ------------------------------------------------------------------------- */

static void ecm_teardown(void)
{
    if (!s_ecm.ready && s_ecm.eth == NULL) {
        return;
    }
    s_ecm.ready = false;
    ESP_LOGI(TAG, "Tearing down ECM driver");

    if (s_ecm.eth != NULL) {
        /* STOP is posted asynchronously by iot_eth_stop(); stop the lwIP netif
         * synchronously as well, otherwise its DHCP timers can still access a
         * netif that is about to be destroyed (review P2-4). */
        iot_eth_stop(s_ecm.eth);
        if (s_ecm.netif != NULL) {
            esp_netif_action_stop(s_ecm.netif, NULL, 0, NULL);
        }
        /* Disconnect the RX path BEFORE the glue/netif are freed; iot_eth does
         * not clear stack_input_info in del_netif_glue (review P1-1). */
        iot_eth_update_input_path(s_ecm.eth, NULL, NULL);
    }
    if (s_ecm.glue != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
        iot_eth_del_netif_glue(s_ecm.glue);
        s_ecm.glue = NULL;
    }
    if (s_ecm.netif != NULL) {
        esp_netif_destroy(s_ecm.netif);
        s_ecm.netif = NULL;
    }
    if (s_ecm.eth != NULL) {
        /* Driver deinit closes the CDC port first (patched fork, audit P0-1).
         * If it fails, keep the handle so a later retry can finish the
         * teardown instead of leaking a zombie instance (review P2-5). */
        esp_err_t err = iot_eth_uninstall(s_ecm.eth);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "iot_eth_uninstall failed (%s), will retry teardown", esp_err_to_name(err));
            cpe_status_set_link(false);
            cpe_status_set_usb_connected(false);
            cpe_status_set_ip(NULL);
            return;
        }
        s_ecm.eth = NULL;
    }
    s_ecm.driver = NULL;

    /* The link state belongs to this interface: clear it explicitly so the
     * link manager does not keep acting on stale "link up / got IP" values
     * while the driver is being rebuilt. */
    cpe_status_set_link(false);
    cpe_status_set_usb_connected(false);
    cpe_status_set_ip(NULL);
}

static esp_err_t ecm_bring_up(void)
{
    if (s_ecm.ready) {
        return ESP_OK;
    }
    /* A previous teardown may have left an uninstall retry pending: finish it
     * before creating a second instance. */
    if (s_ecm.eth != NULL) {
        ecm_teardown();
        if (s_ecm.eth != NULL) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    esp_err_t err = iot_eth_new_usb_ecm(&(iot_usbh_ecm_config_t) {
        .match_id_list = ESP_USB_DEVICE_MATCH_ID_ANY,
    }, &s_ecm.driver);
    if (err != ESP_OK || s_ecm.driver == NULL) {
        ESP_LOGE(TAG, "Failed to create ECM driver: %s", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_FAIL;
    }

    iot_eth_config_t eth_config = {
        .driver = s_ecm.driver,
        .stack_input = NULL,
    };
    err = iot_eth_install(&eth_config, &s_ecm.eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install ECM driver: %s", esp_err_to_name(err));
        /* The driver object is owned by iot_eth after a successful install; on
         * this rare error path it cannot be destroyed (limits API). Do not
         * hammer this path (recovery is rate limited by the caller). */
        s_ecm.driver = NULL;
        return err;
    }

    esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
    inherent.if_key = ECM_NETIF_KEY;
    inherent.if_desc = ECM_NETIF_DESC;
    esp_netif_config_t netif_config = {
        .base = &inherent,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_ecm.netif = esp_netif_new(&netif_config);
    if (s_ecm.netif == NULL) {
        ESP_LOGE(TAG, "Failed to create ECM netif");
        ecm_teardown();
        return ESP_ERR_NO_MEM;
    }

    s_ecm.glue = iot_eth_new_netif_glue(s_ecm.eth);
    if (s_ecm.glue == NULL) {
        ESP_LOGE(TAG, "Failed to create netif glue");
        ecm_teardown();
        return ESP_ERR_NO_MEM;
    }

    err = esp_netif_attach(s_ecm.netif, s_ecm.glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach netif glue: %s", esp_err_to_name(err));
        ecm_teardown();
        return err;
    }

    err = iot_eth_start(s_ecm.eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start ECM driver: %s", esp_err_to_name(err));
        ecm_teardown();
        return err;
    }

    s_ecm.ready = true;

    /* Make the ECM netif the default route as soon as it exists, instead of
     * waiting for the first got-IP event: DHCP, DNS and any traffic the upper
     * layers start from the link-up event then use the correct interface. */
    esp_netif_set_default_netif(s_ecm.netif);

    /* The ECM driver may have already reported LINK_UP before the glue was
     * attached (review P2-9). Re-sync the netif with the current status so the
     * DHCP client and NAPT path are actually started. */
    cpe_status_t st;
    cpe_status_get(&st);
    if (st.link_up && s_ecm.netif != NULL) {
        ESP_LOGI(TAG, "ECM link was already up, re-syncing the netif");
        esp_netif_action_connected(s_ecm.netif, NULL, 0, NULL);
    }

    ESP_LOGI(TAG, "ECM driver ready, waiting for the USB modem");
    return ESP_OK;
}

/* Restart the DHCP client on the ECM netif without touching the USB/ECM
 * session. Tearing the ECM driver down resets the modem USB interface and its
 * DHCP server only gets CPE_IP_TIMEOUT_MS to answer after every rebuild. When
 * the modem is still re-attaching to the network (a USB reset makes it drop
 * and re-establish PDP), that window is far too short and the link manager can
 * trap itself in a rebuild loop. Keep the interface up and retry DHCP first. */
static void dhcp_client_restart(void)
{
    if (s_ecm.netif == NULL) {
        return;
    }
    esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
    esp_err_t err = esp_netif_dhcpc_get_status(s_ecm.netif, &status);
    ESP_LOGW(TAG, "Restarting the DHCP client (status %d, %s)",
             (int)status, esp_err_to_name(err));
    err = esp_netif_dhcpc_stop(s_ecm.netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "esp_netif_dhcpc_stop failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    err = esp_netif_dhcpc_start(s_ecm.netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "esp_netif_dhcpc_start failed: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------------- */
/* Events                                                                   */
/* ------------------------------------------------------------------------- */

static void cpe_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IOT_ETH_EVENT) {
        /* Do NOT gate on s_ecm.ready: the ECM driver may report the link state
         * while main is still creating the netif/glue. Dropping those events
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
            if (event->esp_netif != s_ecm.netif) {
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

static void cpe_apply_upstream_dns(void)
{
    esp_netif_t *ap = app_wifi_get_ap_netif();
    if (ap == NULL || s_ecm.netif == NULL) {
        return;
    }

    esp_netif_dns_info_t dns = {0};
    if (esp_netif_get_dns_info(s_ecm.netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
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

static void cpe_enable_napt(void)
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

static void cpe_link_online(void)
{
    cpe_enable_napt();
    cpe_apply_upstream_dns();
}

static void cpe_link_offline(void)
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
    cpe_link_online();
}

/* ------------------------------------------------------------------------- */
/* 4G module reset / USB enumeration recovery                                */
/* ------------------------------------------------------------------------- */

#if CONFIG_CPE_MODULE_RESET_GPIO >= 0
static void module_reset_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_CPE_MODULE_RESET_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure module reset GPIO%d", CONFIG_CPE_MODULE_RESET_GPIO);
        return;
    }
    /* Keep the module out of reset when idle. */
    gpio_set_level(CONFIG_CPE_MODULE_RESET_GPIO, !CONFIG_CPE_MODULE_RESET_ACTIVE_LEVEL);
}

static void module_reset_pulse(void)
{
    ESP_LOGW(TAG, "Pulsing the 4G module reset (GPIO%d)", CONFIG_CPE_MODULE_RESET_GPIO);
    gpio_set_level(CONFIG_CPE_MODULE_RESET_GPIO, CONFIG_CPE_MODULE_RESET_ACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_CPE_MODULE_RESET_HOLD_MS));
    gpio_set_level(CONFIG_CPE_MODULE_RESET_GPIO, !CONFIG_CPE_MODULE_RESET_ACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_CPE_MODULE_BOOT_DELAY_MS));
}
#endif

/* Bring the module USB stack back to a clean state. Prefers the module reset
 * GPIO (definitive for an externally powered module); without it the host root
 * port is power-cycled, which detaches the modem from the bus. Returns true when
 * a reset was actually performed. */
static bool usb_modem_reset(void)
{
#if CONFIG_CPE_MODULE_RESET_GPIO >= 0
    module_reset_pulse();
    return true;
#else
    ESP_LOGW(TAG, "Power-cycling the USB root port to force re-enumeration");
    return usb_root_port_power_cycle(CONFIG_CPE_USB_PORT_POWER_OFF_MS) == ESP_OK;
#endif
}

/* ------------------------------------------------------------------------- */
/* Link manager                                                              */
/* ------------------------------------------------------------------------- */

/* True when the USB Host Library has at least one *enumerated* device.
 *
 * Do not use usb_host_lib_info().num_devices here: after a failed enumeration
 * (e.g. CHECK_SHORT_DEV_DESC FAILED when the ESP32 resets while an externally
 * powered modem keeps its USB state) the device object can still be counted,
 * which made the recovery logic believe "the modem is on the bus" and skip the
 * root-port power cycle. The address list only contains devices that finished
 * enumeration. */
static bool usb_device_present(void)
{
    uint8_t dev_addr_list[8];
    int num_dev = 0;
    if (usb_host_device_addr_list_fill(sizeof(dev_addr_list), dev_addr_list, &num_dev) != ESP_OK) {
        return false;
    }
    return num_dev > 0;
}

/* Sleep in 500 ms slices, returning early if the 4G link comes up (e.g. the
 * user plugs the modem back in while the link manager is backing off). */
static void backoff_sleep_ms(uint32_t ms)
{
    uint32_t waited = 0;
    while (waited < ms) {
        cpe_status_t st;
        cpe_status_get(&st);
        if (st.link_up) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }
}

typedef enum {
    LM_PHASE_BOOT = 0,      /* USB host / ECM driver bring-up */
    LM_PHASE_USB_WAIT,      /* no link yet */
    LM_PHASE_LINK_WAIT,     /* link is up, no IP yet */
    LM_PHASE_ONLINE,        /* link up and IP acquired */
} lm_phase_t;

/* LM_TICK_ACTION means a recovery action already consumed the delay (the old
 * loop's "continue"), so the task must not add its usual 500 ms tick. */
typedef enum {
    LM_TICK_IDLE = 0,
    LM_TICK_ACTION,
} lm_tick_result_t;

typedef struct {
    lm_phase_t phase;
    uint32_t recover_attempts;
    uint32_t enum_reset_attempts;
    uint32_t dhcp_restarts;
    int64_t wait_start_us;
    int64_t online_start_us;
    int64_t next_heap_log_us;
    bool was_online;
    bool last_link_up;
    bool last_got_ip;
} lm_ctx_t;

static const char *lm_phase_name(lm_phase_t phase)
{
    switch (phase) {
    case LM_PHASE_BOOT:
        return "boot";
    case LM_PHASE_USB_WAIT:
        return "usb_wait";
    case LM_PHASE_LINK_WAIT:
        return "link_wait";
    case LM_PHASE_ONLINE:
        return "online";
    default:
        return "unknown";
    }
}

static void lm_enter_phase(lm_ctx_t *ctx, lm_phase_t phase)
{
    if (ctx->phase != phase) {
        ESP_LOGD(TAG, "Link manager phase: %s -> %s",
                 lm_phase_name(ctx->phase), lm_phase_name(phase));
        ctx->phase = phase;
    }
}

/* USB host + CDC driver (installed once), then the ECM driver and netif. */
static bool lm_ensure_ready(lm_ctx_t *ctx)
{
    if (!s_cdc_installed) {
        if (usb_cdc_install() != ESP_OK) {
            cpe_status_set_state(CPE_STATE_FAULT, "usb host init failed, retrying");
            cpe_status_set_error("usb host init failed");
            lm_enter_phase(ctx, LM_PHASE_BOOT);
            vTaskDelay(pdMS_TO_TICKS(5000));
            return false;
        }
    }

    if (!s_ecm.ready) {
        if (ecm_bring_up() != ESP_OK) {
            cpe_status_set_state(CPE_STATE_FAULT, "ecm driver init failed, retrying");
            cpe_status_set_error("ecm driver init failed");
            lm_enter_phase(ctx, LM_PHASE_BOOT);
            vTaskDelay(pdMS_TO_TICKS(5000));
            return false;
        }
    }
    return true;
}

static void lm_service(lm_ctx_t *ctx, const cpe_status_t *st, int64_t now)
{
    if (st->link_up != ctx->last_link_up) {
        ctx->wait_start_us = now;
        ctx->last_link_up = st->link_up;
        log_heap(st->link_up ? "link up" : "link down");
    }
    if (st->got_ip != ctx->last_got_ip) {
        ctx->wait_start_us = now;
        ctx->last_got_ip = st->got_ip;
        if (st->got_ip) {
            ctx->dhcp_restarts = 0;
            cpe_link_online();
        } else {
            cpe_link_offline();
        }
        log_heap(st->got_ip ? "got ip" : "lost ip");
    }

#if CONFIG_CPE_HEAP_LOG_PERIOD_S > 0
    if (ctx->next_heap_log_us == 0) {
        ctx->next_heap_log_us = now + (int64_t)CONFIG_CPE_HEAP_LOG_PERIOD_S * 1000000LL;
    } else if (now >= ctx->next_heap_log_us) {
        log_heap("periodic");
        ctx->next_heap_log_us = now + (int64_t)CONFIG_CPE_HEAP_LOG_PERIOD_S * 1000000LL;
    }
#endif
}

static lm_tick_result_t lm_online(lm_ctx_t *ctx, const cpe_status_t *st, int64_t now)
{
    lm_enter_phase(ctx, LM_PHASE_ONLINE);

    /* Healthy. Clear the persistent boot/recovery counter once stable. */
    if (!ctx->was_online) {
        ctx->was_online = true;
        ctx->recover_attempts = 0;
        ctx->enum_reset_attempts = 0;
        ctx->online_start_us = now;
        cpe_status_set_recovery_attempts(0);
        cpe_status_set_state(CPE_STATE_ONLINE, "online");
        ESP_LOGI(TAG, "CPE online (WAN IP %s)", st->ip);
    }
    if (ctx->online_start_us != 0 &&
            (now - ctx->online_start_us) > (int64_t)CONFIG_CPE_HEALTHY_CLEAR_SECONDS * 1000000LL) {
        if (cpe_boot_count_get() != 0) {
            cpe_boot_count_clear();
            /* Keep the reported status in sync: otherwise the web UI keeps
             * showing the boot count captured at start-up. */
            cpe_status_set_boot_count(0);
            ESP_LOGI(TAG, "Healthy for %d s, boot counter cleared", CONFIG_CPE_HEALTHY_CLEAR_SECONDS);
        }
        ctx->online_start_us = 0;
    }
    return LM_TICK_IDLE;
}

/* Link is up but the modem has not obtained an IP: usually no SIM / PDP not
 * activated. Keep the ECM/USB session and retry only the DHCP client (a
 * teardown resets the modem USB interface, which is exactly what to avoid). */
static lm_tick_result_t lm_recover_dhcp(lm_ctx_t *ctx, int64_t now)
{
    ctx->dhcp_restarts++;
    cpe_status_set_error("waiting for the modem DHCP server");
    cpe_status_set_state(CPE_STATE_LINK_WAIT, "waiting for IP (DHCP retry)");
    dhcp_client_restart();
    ctx->wait_start_us = now;
    return LM_TICK_ACTION;
}

static lm_tick_result_t lm_rebuild_ecm(lm_ctx_t *ctx)
{
    ctx->dhcp_restarts = 0;
    cpe_status_set_error("link up but no IP (SIM/PDP problem?)");
    cpe_status_set_state(CPE_STATE_FAULT, "no IP from modem (SIM/PDP?), retrying");
    ESP_LOGW(TAG, "DHCP client retries exhausted, rebuilding the ECM driver");
    ecm_teardown();
    ctx->wait_start_us = 0;
    lm_enter_phase(ctx, LM_PHASE_BOOT);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_CPE_RECOVERY_RETRY_MS));
    return LM_TICK_ACTION;
}

static lm_tick_result_t lm_wait_ip(lm_ctx_t *ctx, int64_t now)
{
    lm_enter_phase(ctx, LM_PHASE_LINK_WAIT);
    if (ctx->wait_start_us == 0) {
        ctx->wait_start_us = now;
    }
    if ((now - ctx->wait_start_us) <= (int64_t)CONFIG_CPE_IP_TIMEOUT_MS * 1000LL) {
        cpe_status_set_state(CPE_STATE_LINK_WAIT, "waiting for IP");
        return LM_TICK_IDLE;
    }

    ctx->recover_attempts++;
    cpe_status_set_recovery_attempts(ctx->recover_attempts);
    ESP_LOGW(TAG, "Link up but no IP after %d ms (attempt %"PRIu32")",
             CONFIG_CPE_IP_TIMEOUT_MS, ctx->recover_attempts);

    if (CONFIG_CPE_IP_DHCP_RESTARTS > 0 &&
            ctx->dhcp_restarts < CONFIG_CPE_IP_DHCP_RESTARTS) {
        return lm_recover_dhcp(ctx, now);
    }
    return lm_rebuild_ecm(ctx);
}

/* Nothing enumerated: the modem is absent OR its USB stack is stale (typical
 * when the ESP32 resets while an externally powered module keeps its power ->
 * CHECK_SHORT_DEV_DESC FAILED). ECM rebuilds cannot fix this, so try the reset
 * mechanisms right away. */
static bool lm_try_reenumerate(lm_ctx_t *ctx)
{
    if (ctx->enum_reset_attempts >= CONFIG_CPE_USB_ENUM_RESET_ATTEMPTS) {
        return false;
    }
    if (!usb_modem_reset()) {
        return false;
    }
    ctx->enum_reset_attempts++;
    cpe_status_set_state(CPE_STATE_BOOT, "re-enumerating the USB modem");
    ctx->wait_start_us = 0;
    lm_enter_phase(ctx, LM_PHASE_BOOT);
    return true;
}

static lm_tick_result_t lm_wait_for_modem(lm_ctx_t *ctx)
{
    ESP_LOGW(TAG, "No USB modem detected, staying in fault mode (AP/Web available)");
    cpe_status_set_state(CPE_STATE_FAULT, "no USB modem, AP/Web still available");
    backoff_sleep_ms(CONFIG_CPE_RECOVERY_RETRY_MS);
    /* Retry the whole reset cycle later (a modem may be plugged in at any
     * time). */
    ctx->enum_reset_attempts = 0;
    ctx->wait_start_us = 0;
    return LM_TICK_ACTION;
}

/* The modem is on the bus but the ECM link never came up: controlled reboot is
 * the last resort, bounded by the persistent boot counter. */
static void lm_controlled_restart(uint32_t boot_count)
{
    ESP_LOGE(TAG, "ECM link stuck, controlled reboot %"PRIu32"/%d",
             boot_count, CONFIG_CPE_MAX_REBOOTS);
    cpe_status_set_state(CPE_STATE_FAULT, "restarting to recover ECM link");
    vTaskDelay(pdMS_TO_TICKS(1000));
    app_wifi_deauth_all_stations();
    esp_restart();
}

static lm_tick_result_t lm_escalate_usb_fault(lm_ctx_t *ctx)
{
    if (ctx->recover_attempts >= CONFIG_CPE_MAX_RECOVERY_ATTEMPTS) {
        ctx->recover_attempts = 0;
        cpe_status_set_recovery_attempts(0);

        uint32_t boot_count = cpe_boot_count_get();
        if (CONFIG_CPE_MAX_REBOOTS > 0 && boot_count < CONFIG_CPE_MAX_REBOOTS) {
            lm_controlled_restart(boot_count);
        }
        ESP_LOGE(TAG, "ECM link stuck, keeping AP/Web alive (fault mode)");
        cpe_status_set_state(CPE_STATE_FAULT, "ECM link stuck, AP/Web still available");
        backoff_sleep_ms(CONFIG_CPE_RECOVERY_RETRY_MS);
    } else {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return LM_TICK_ACTION;
}

/* No USB link at all: module missing, enumeration failed or the module
 * dropped off the bus. */
static lm_tick_result_t lm_wait_usb(lm_ctx_t *ctx, int64_t now)
{
    lm_enter_phase(ctx, LM_PHASE_USB_WAIT);
    ctx->dhcp_restarts = 0;
    if (ctx->wait_start_us == 0) {
        ctx->wait_start_us = now;
    }
    if ((now - ctx->wait_start_us) <= (int64_t)CONFIG_CPE_LINK_TIMEOUT_MS * 1000LL) {
        cpe_status_set_state(CPE_STATE_USB_WAIT, "waiting for USB modem");
        return LM_TICK_IDLE;
    }

    ctx->recover_attempts++;
    cpe_status_set_recovery_attempts(ctx->recover_attempts);
    cpe_status_set_error("no USB link from 4G modem");
    ESP_LOGW(TAG, "No USB link after %d ms (attempt %"PRIu32")",
             CONFIG_CPE_LINK_TIMEOUT_MS, ctx->recover_attempts);
    cpe_status_set_state(CPE_STATE_FAULT, "waiting for USB modem");
    ecm_teardown();
    ctx->wait_start_us = 0;

    if (!usb_device_present()) {
        if (lm_try_reenumerate(ctx)) {
            return LM_TICK_ACTION;
        }
        return lm_wait_for_modem(ctx);
    }
    return lm_escalate_usb_fault(ctx);
}

static lm_tick_result_t lm_tick(lm_ctx_t *ctx)
{
    if (!lm_ensure_ready(ctx)) {
        return LM_TICK_ACTION;
    }

    cpe_status_t st;
    cpe_status_get(&st);
    int64_t now = esp_timer_get_time();

    lm_service(ctx, &st, now);

    if (st.link_up && st.got_ip) {
        return lm_online(ctx, &st, now);
    }

    ctx->was_online = false;
    if (st.link_up) {
        return lm_wait_ip(ctx, now);
    }
    return lm_wait_usb(ctx, now);
}

static void link_manager_task(void *arg)
{
    (void)arg;
    lm_ctx_t ctx = { .phase = LM_PHASE_BOOT };

    cpe_status_set_state(CPE_STATE_BOOT, "starting usb modem support");

    for (;;) {
        if (lm_tick(&ctx) == LM_TICK_IDLE) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Application entry                                                          */
/* ------------------------------------------------------------------------- */

void app_main(void)
{
    /* Start the reset-button boot window: a double-click within the first
     * seconds after boot prints the credentials (physical-presence trigger). */
    (void)reset_button_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: nvs_flash_init failed (%s); staying idle", esp_err_to_name(err));
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    err = esp_netif_init();
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: network stack init failed (%s); staying idle", esp_err_to_name(err));
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    err = cpe_core_init();
    if (err != ESP_OK) {
        /* A configuration/NVS failure must not turn into an endless panic
         * reboot loop on a remote CPE: erase NVS once and retry. */
        ESP_LOGE(TAG, "cpe_core_init failed (%s), erasing NVS and retrying once", esp_err_to_name(err));
        if (nvs_flash_erase() == ESP_OK && nvs_flash_init() == ESP_OK) {
            err = cpe_core_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: cannot initialize configuration; staying idle (no reboot loop)");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    err = esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID, cpe_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IOT_ETH event handler: %s", esp_err_to_name(err));
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, cpe_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH_GOT_IP handler: %s", esp_err_to_name(err));
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, cpe_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH_LOST_IP handler: %s", esp_err_to_name(err));
    }

    /* SoftAP + Web first: the device is always reachable, even without 4G. */
    err = app_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP start failed: %s", esp_err_to_name(err));
    }
    /* Must come after app_wifi_start(): the default Wi-Fi handler that re-adds
     * the AP netif has to run before this one re-enables NAPT on it. */
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START,
                                     on_ap_start_reapply_napt, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register AP start NAPT handler: %s",
                 esp_err_to_name(err));
    }
    err = modem_http_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Web server start failed: %s", esp_err_to_name(err));
    }

#if CONFIG_CPE_MODULE_RESET_GPIO >= 0
    /* An ESP32 reset does not power-cycle an externally powered 4G module; its
     * USB stack can stay in a stale state and enumeration fails with
     * "CHECK_SHORT_DEV_DESC FAILED". Pulse the module reset once here so the
     * USB host always enumerates from a clean state. */
    module_reset_init();
    ESP_LOGI(TAG, "Resetting the 4G module so USB enumeration starts clean");
    module_reset_pulse();
#endif

    BaseType_t created = xTaskCreate(link_manager_task, "cpe_link", 4096, NULL, 6, NULL);
    if (created != pdPASS) {
        /* Without the link manager the CPE stays in AP-only mode instead of
         * aborting into a reboot loop. */
        ESP_LOGE(TAG, "Failed to create link manager task, 4G WAN disabled");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    /* Long-press the same button to restore factory settings. */
    err = reset_button_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reset button monitor not started (%s)", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "CPE started, management console on http://192.168.4.1");

    /* Firmware/OTA bookkeeping: log the running image, then validate it once the
     * boot is considered healthy (see ota_validate_task). */
    cpe_ota_info_t ota_info;
    if (cpe_ota_get_info(&ota_info) == ESP_OK) {
        ESP_LOGI(TAG, "Firmware version %s on %s%s%s",
                 ota_info.running_version, ota_info.running_partition,
                 ota_info.next_partition[0] != '\0' ? ", next boot " : "",
                 ota_info.next_partition);
        if (ota_info.pending_verify) {
            ESP_LOGW(TAG, "Image is pending verification, validating in %d ms",
                     CONFIG_CPE_OTA_VALIDATE_DELAY_MS);
        }
    }
    if (xTaskCreate(ota_validate_task, "cpe_ota", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Failed to create the OTA validation task");
    }

    log_heap("boot");
}
