/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB host / CDC / ECM lifecycle and module reset for the 4G WAN.
 *
 * The USB Host library and the CDC driver are installed once (idempotent) and
 * are NOT torn down at runtime: that removes the whole
 * "uninstall -> install -> panic" failure chain found by the audit.
 */

#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "usb/usb_host.h"

#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_ecm.h"

#include "cpe_core.h"
#include "cpe_wan.h"
#include "wan_internal.h"

static const char *TAG = "cpe_wan";

#define ECM_NETIF_KEY       "ECM0"
#define ECM_NETIF_DESC      "usb-ecm"

/* Pause before the netif glue is freed, so an in-flight RX callback has left
 * the glue. */
#define ECM_GLUE_RELEASE_MS     20
/* Pause between stopping and restarting the DHCP client. */
#define ECM_DHCP_RESTART_GAP_MS 300
/* How long a teardown waits for the borrowers to release the handles. It is a
 * bound, not a lock: a borrower only runs a few non-blocking calls, and when it
 * does not come back in time the teardown keeps the handles and is retried by
 * the next wan_ecm_bring_up(). Waiting without a bound here would hang the link
 * manager, which is exactly what the mutex did (commit bcca642). */
#define ECM_RELEASE_TIMEOUT_MS  200

typedef struct {
    iot_eth_driver_t *driver;
    iot_eth_handle_t eth;
    iot_eth_netif_glue_handle_t glue;
    esp_netif_t *netif;
    bool ready;
    /* Lifetime. The link manager task owns bring-up and teardown, everyone else
     * only borrows the handles - and a borrower must never block, because it
     * can be an event callback. So teardown does not lock anyone out: it
     * publishes "closing", waits for the borrowers to leave (bounded, see
     * ECM_RELEASE_TIMEOUT_MS) and keeps the handles if they do not.
     * The driver's own RX path (USB/ECM thread into lwIP) is NOT covered by
     * this: it is not our code. It is handled by stopping the driver,
     * disconnecting the input path and the settle delay below. */
    /* Both are only touched through the __atomic_* builtins, the way the rest of
     * the project publishes a scalar across tasks. */
    int refs;
    bool closing;
} ecm_ctx_t;

static ecm_ctx_t s_ecm;
static bool s_cdc_installed;

/* ------------------------------------------------------------------------- */
/* USB host / CDC driver                                                     */
/* ------------------------------------------------------------------------- */

#if CONFIG_CPE_ENUM_FILTER_ENABLE
/* Configuration selector, not a filter: the VID/PID below only decide which
 * device gets the forced bConfigurationValue. Every device is still enumerated
 * (the callback always accepts), because rejecting one here would silently hide
 * a modem that comes up with a different VID/PID (for example in download mode)
 * and leave nothing but "no USB modem" to diagnose. */
static bool cpe_config_selector_cb(const usb_device_desc_t *dev_desc,
                                   uint8_t *bConfigurationValue)
{
    bool vid_ok = (CONFIG_CPE_ECM_VID == 0) || (dev_desc->idVendor == CONFIG_CPE_ECM_VID);
    bool pid_ok = (CONFIG_CPE_ECM_PID == 0) || (dev_desc->idProduct == CONFIG_CPE_ECM_PID);
    if (vid_ok && pid_ok) {
        *bConfigurationValue = CONFIG_CPE_ECM_CONFIG_VALUE;
        ESP_LOGI(TAG, "Configuration selector: %04X:%04X -> configuration %d",
                 dev_desc->idVendor, dev_desc->idProduct, CONFIG_CPE_ECM_CONFIG_VALUE);
    } else {
        ESP_LOGI(TAG, "Configuration selector: %04X:%04X is not the target device, "
                 "keeping its default configuration",
                 dev_desc->idVendor, dev_desc->idProduct);
    }
    return true;
}
#endif

esp_err_t wan_usb_cdc_install(void)
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
        .enum_filter_cb = cpe_config_selector_cb,
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

/* esp_netif_action_*() are the callbacks esp_netif itself runs in the TCP/IP
 * thread. Calling them straight from the link manager task races with that
 * thread over the netif state, so they go through esp_netif_tcpip_exec(),
 * which blocks until the callback has run in the TCP/IP context. */
static esp_err_t netif_action_stop(void *ctx)
{
    esp_netif_action_stop(ctx, NULL, 0, NULL);
    return ESP_OK;
}

static esp_err_t netif_action_connected(void *ctx)
{
    esp_netif_action_connected(ctx, NULL, 0, NULL);
    return ESP_OK;
}

/* Borrow the ECM handles from another task. Never blocks, so it is safe from an
 * event callback: it fails while the interface is not up or a teardown is
 * running. Every successful call must be paired with wan_ecm_release(); until
 * then wan_ecm_netif()/wan_ecm_eth_handle() may be dereferenced.
 * Comparing them without borrowing stays allowed - that is all the event
 * handler does - because a comparison never touches the object. */
bool wan_ecm_borrow(void)
{
    if (!s_ecm.ready || __atomic_load_n(&s_ecm.closing, __ATOMIC_ACQUIRE)) {
        return false;
    }
    __atomic_fetch_add(&s_ecm.refs, 1, __ATOMIC_ACQ_REL);
    /* A teardown may have started in between: give the reference back. */
    if (__atomic_load_n(&s_ecm.closing, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_sub(&s_ecm.refs, 1, __ATOMIC_ACQ_REL);
        return false;
    }
    return true;
}

void wan_ecm_release(void)
{
    __atomic_fetch_sub(&s_ecm.refs, 1, __ATOMIC_ACQ_REL);
}

/* Bounded, see ECM_RELEASE_TIMEOUT_MS: a borrower only runs a few non-blocking
 * calls, and if it does not come back the teardown is postponed rather than
 * hanging the link manager here. */
static esp_err_t ecm_wait_for_borrowers(void)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)ECM_RELEASE_TIMEOUT_MS * 1000LL;
    while (__atomic_load_n(&s_ecm.refs, __ATOMIC_ACQUIRE) > 0) {
        if (esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

esp_err_t wan_ecm_teardown(void)
{
    if (!s_ecm.ready && s_ecm.eth == NULL) {
        return ESP_OK;
    }
    s_ecm.ready = false;
    /* Stop admitting borrowers first: nothing new may take a reference while
     * these handles are on their way out. */
    __atomic_store_n(&s_ecm.closing, true, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "Tearing down ECM driver");

    if (ecm_wait_for_borrowers() != ESP_OK) {
        ESP_LOGW(TAG, "ECM handles still in use, keeping them and retrying the teardown");
        return ESP_ERR_TIMEOUT;
    }

    if (s_ecm.eth != NULL) {
        /* STOP is posted asynchronously by iot_eth_stop(); stop the lwIP netif
         * synchronously as well, otherwise its DHCP timers can still access a
         * netif that is about to be destroyed (review P2-4). */
        iot_eth_stop(s_ecm.eth);
        if (s_ecm.netif != NULL) {
            (void)esp_netif_tcpip_exec(netif_action_stop, s_ecm.netif);
        }
        /* Disconnect the RX path BEFORE the glue/netif are freed; iot_eth does
         * not clear stack_input_info in del_netif_glue (review P1-1). */
        iot_eth_update_input_path(s_ecm.eth, NULL, NULL);
    }
    if (s_ecm.glue != NULL) {
        vTaskDelay(pdMS_TO_TICKS(ECM_GLUE_RELEASE_MS));
        iot_eth_del_netif_glue(s_ecm.glue);
        s_ecm.glue = NULL;
    }
    if (s_ecm.netif != NULL) {
        /* Destroying the netif drops the lwIP default route, but esp_netif
         * re-elects one immediately: esp_netif_destroy() ends with
         * esp_netif_update_default_netif(ESP_NETIF_STOPPED), and since the
         * destroyed netif is unlisted by then, the override is dropped and the
         * remaining netif that is up (here: the SoftAP) becomes default. No
         * explicit fallback is needed here; wan_ecm_bring_up() sets the ECM
         * netif as default again as soon as it exists. */
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
            return err;
        }
        s_ecm.eth = NULL;
    }
    s_ecm.driver = NULL;
    /* The handles are gone, so nothing can borrow them any more: allow the next
     * bring-up to admit borrowers again. */
    __atomic_store_n(&s_ecm.closing, false, __ATOMIC_RELEASE);

    /* The link state belongs to this interface: clear it explicitly so the
     * link manager does not keep acting on stale "link up / got IP" values
     * while the driver is being rebuilt. */
    cpe_status_set_link(false);
    cpe_status_set_usb_connected(false);
    cpe_status_set_ip(NULL);
    return ESP_OK;
}

/* Cleanup after a failed bring-up: an incomplete teardown is not an error of
 * its own, the next bring-up finishes it, but it must not pass unnoticed. */
static void ecm_cleanup(void)
{
    if (wan_ecm_teardown() != ESP_OK) {
        ESP_LOGW(TAG, "ECM teardown incomplete, the next bring-up retries it");
    }
}

esp_err_t wan_ecm_bring_up(void)
{
    if (s_ecm.ready) {
        return ESP_OK;
    }
    /* A previous teardown may have left an uninstall retry pending: finish it
     * before creating a second instance. */
    if (s_ecm.eth != NULL) {
        if (wan_ecm_teardown() != ESP_OK) {
            ESP_LOGW(TAG, "Previous ECM teardown still incomplete, retrying it now");
        }
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
        ecm_cleanup();
        return ESP_ERR_NO_MEM;
    }

    s_ecm.glue = iot_eth_new_netif_glue(s_ecm.eth);
    if (s_ecm.glue == NULL) {
        ESP_LOGE(TAG, "Failed to create netif glue");
        ecm_cleanup();
        return ESP_ERR_NO_MEM;
    }

    err = esp_netif_attach(s_ecm.netif, s_ecm.glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach netif glue: %s", esp_err_to_name(err));
        ecm_cleanup();
        return err;
    }

    err = iot_eth_start(s_ecm.eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start ECM driver: %s", esp_err_to_name(err));
        ecm_cleanup();
        return err;
    }

    __atomic_store_n(&s_ecm.refs, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_ecm.closing, false, __ATOMIC_RELEASE);
    s_ecm.ready = true;

    /* Make the ECM netif the default route as soon as it exists, instead of
     * waiting for the first got-IP event: DHCP, DNS and any traffic the upper
     * layers start from the link-up event then use the correct interface. */
    err = esp_netif_set_default_netif(s_ecm.netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set ECM netif as default: %s", esp_err_to_name(err));
    }

    /* The ECM driver may have already reported LINK_UP before the glue was
     * attached (review P2-9). Re-sync the netif with the current status so the
     * DHCP client and NAPT path are actually started. */
    cpe_status_t st;
    cpe_status_get(&st);
    if (st.link_up && s_ecm.netif != NULL) {
        ESP_LOGI(TAG, "ECM link was already up, re-syncing the netif");
        (void)esp_netif_tcpip_exec(netif_action_connected, s_ecm.netif);
    }

    ESP_LOGI(TAG, "ECM driver ready, waiting for the USB modem");
    return ESP_OK;
}

esp_netif_t *wan_ecm_netif(void)
{
    return s_ecm.netif;
}

iot_eth_handle_t wan_ecm_eth_handle(void)
{
    return s_ecm.eth;
}

/* Restart the DHCP client on the ECM netif without touching the USB/ECM
 * session. Tearing the ECM driver down resets the modem USB interface and its
 * DHCP server only gets CPE_IP_TIMEOUT_MS to answer after every rebuild. When
 * the modem is still re-attaching to the network (a USB reset makes it drop
 * and re-establish PDP), that window is far too short and the link manager can
 * trap itself in a rebuild loop. Keep the interface up and retry DHCP first. */
void wan_ecm_dhcp_restart(void)
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
    vTaskDelay(pdMS_TO_TICKS(ECM_DHCP_RESTART_GAP_MS));
    err = esp_netif_dhcpc_start(s_ecm.netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "esp_netif_dhcpc_start failed: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------------- */
/* 4G module reset / USB enumeration recovery                                */
/* ------------------------------------------------------------------------- */

#if CONFIG_CPE_MODULE_RESET_GPIO >= 0
static bool s_module_reset_ready;

static esp_err_t module_reset_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_CPE_MODULE_RESET_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure module reset GPIO%d: %s",
                 CONFIG_CPE_MODULE_RESET_GPIO, esp_err_to_name(err));
        return err;
    }
    /* Keep the module out of reset when idle. */
    err = gpio_set_level(CONFIG_CPE_MODULE_RESET_GPIO, !CONFIG_CPE_MODULE_RESET_ACTIVE_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set module reset GPIO%d idle level: %s",
                 CONFIG_CPE_MODULE_RESET_GPIO, esp_err_to_name(err));
        return err;
    }
    s_module_reset_ready = true;
    return ESP_OK;
}

static esp_err_t module_reset_pulse(void)
{
    ESP_LOGW(TAG, "Pulsing the 4G module reset (GPIO%d)", CONFIG_CPE_MODULE_RESET_GPIO);
    esp_err_t err = gpio_set_level(CONFIG_CPE_MODULE_RESET_GPIO,
                                   CONFIG_CPE_MODULE_RESET_ACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_CPE_MODULE_RESET_HOLD_MS));
    esp_err_t release_err = gpio_set_level(CONFIG_CPE_MODULE_RESET_GPIO,
                                           !CONFIG_CPE_MODULE_RESET_ACTIVE_LEVEL);
    if (err == ESP_OK) {
        err = release_err;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Module reset GPIO%d pulse failed: %s",
                 CONFIG_CPE_MODULE_RESET_GPIO, esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(CONFIG_CPE_MODULE_BOOT_DELAY_MS));
    return err;
}
#endif

/* An ESP32 reset does not power-cycle an externally powered 4G module; its USB
 * stack can stay in a stale state and enumeration fails with
 * "CHECK_SHORT_DEV_DESC FAILED". Pulse the module reset once at boot so the USB
 * host always enumerates from a clean state. A missing/failed reset line is not
 * fatal: the link manager can still power-cycle the root port later. */
esp_err_t wan_boot_reset_module(void)
{
#if CONFIG_CPE_MODULE_RESET_GPIO >= 0
    if (module_reset_init() != ESP_OK) {
        ESP_LOGW(TAG, "Module reset GPIO unavailable, relying on the USB root port");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Resetting the 4G module so USB enumeration starts clean");
    (void)module_reset_pulse();
#endif
    return ESP_OK;
}

/* Bring the module USB stack back to a clean state. Prefers the module reset
 * GPIO (definitive for an externally powered module); without it (or when the
 * GPIO path fails) the host root port is power-cycled, which detaches the modem
 * from the bus. Returns true when a reset was actually performed. */
bool wan_usb_modem_reset(void)
{
#if CONFIG_CPE_MODULE_RESET_GPIO >= 0
    if (s_module_reset_ready || module_reset_init() == ESP_OK) {
        if (module_reset_pulse() == ESP_OK) {
            return true;
        }
        ESP_LOGW(TAG, "Module reset pulse failed, falling back to the USB root port");
    }
#endif
    ESP_LOGW(TAG, "Power-cycling the USB root port to force re-enumeration");
    return usb_root_port_power_cycle(CONFIG_CPE_USB_PORT_POWER_OFF_MS) == ESP_OK;
}

/* True when the USB Host Library has at least one *enumerated* device.
 *
 * Do not use usb_host_lib_info().num_devices here: after a failed enumeration
 * (e.g. CHECK_SHORT_DEV_DESC FAILED when the ESP32 resets while an externally
 * powered modem keeps its USB state) the device object can still be counted,
 * which made the recovery logic believe "the modem is on the bus" and skip the
 * root-port power cycle. The address list only contains devices that finished
 * enumeration. */
bool wan_usb_device_present(void)
{
    uint8_t dev_addr_list[8];
    int num_dev = 0;
    if (usb_host_device_addr_list_fill(sizeof(dev_addr_list), dev_addr_list, &num_dev) != ESP_OK) {
        return false;
    }
    return num_dev > 0;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t cpe_wan_start(void)
{
    esp_err_t err = wan_boot_reset_module();
    if (err != ESP_OK) {
        return err;
    }

    /* Install the USB host/CDC driver here instead of leaving it to the first
     * link manager tick: app_main starts the AT telemetry right after this, and
     * its USB device event registration fails until the driver exists. Not
     * fatal on error - lm_ensure_ready() retries every 5 s. */
    if (wan_usb_cdc_install() != ESP_OK) {
        ESP_LOGW(TAG, "USB CDC driver not installed yet, the link manager will retry");
    }

    return wan_link_manager_start();
}
