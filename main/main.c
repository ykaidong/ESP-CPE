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
 *  - The 4G WAN (USB host/CDC, ECM lifecycle, AP-side NAPT/DNS, recovery state
 *    machine) is owned by components/cpe_wan. This file only orchestrates the
 *    boot order.
 *  - The WAN link manager applies bounded recovery: L1 rebuild ECM driver ->
 *    L2 root port power cycle -> L3 controlled reboot (NVS boot counter,
 *    cleared once healthy). "Link up but no IP" (no SIM / PDP not active) never
 *    reboots: the device stays in fault mode with AP/Web.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "cpe_core.h"
#include "cpe_ota.h"
#include "app_wifi.h"
#include "cpe_web.h"
#include "cpe_wan.h"
#include "reset_button.h"

static const char *TAG = "cpe";

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

    /* Register the WAN link events before any driver can report them. */
    (void)cpe_wan_events_register();

    /* SoftAP + Web first: the device is always reachable, even without 4G. */
    err = app_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP start failed: %s", esp_err_to_name(err));
    }
    /* Must come after app_wifi_start(): the default Wi-Fi handler that re-adds
     * the AP netif has to run before this one re-enables NAPT on it. */
    (void)cpe_wan_ap_hook_register();
    err = modem_http_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Web server start failed: %s", esp_err_to_name(err));
    }

    err = cpe_wan_start();
    if (err != ESP_OK) {
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

    cpe_wan_log_heap("boot");
}
