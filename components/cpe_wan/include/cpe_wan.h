/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public interface of the 4G WAN component.
 *
 * The component owns the USB host/CDC/ECM lifecycle, the WAN-side network
 * services (NAPT and upstream DNS for the SoftAP clients) and the link manager
 * with its bounded recovery policy. main only orchestrates the boot order.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the 4G link event handlers (IOT_ETH and IP events).
 *
 * Must be called after the default event loop has been created and before the
 * ECM driver is brought up (the link manager starts later). Registering early
 * is deliberate: the driver may report LINK_UP before the ECM netif exists, and
 * those events must not be lost.
 *
 * Individual registration failures are logged; the function returns the last
 * error but the caller normally continues (matching the previous boot path).
 */
esp_err_t cpe_wan_events_register(void);

/**
 * @brief Register the SoftAP-restart hook that re-applies NAPT and DNS.
 *
 * Must be called after app_wifi_start(): the Wi-Fi default handler that re-adds
 * the AP netif has to run first. The hook re-enables NAPT only when an IP had
 * already been acquired.
 */
esp_err_t cpe_wan_ap_hook_register(void);

/**
 * @brief Pulse the optional module reset GPIO and start the link manager task.
 *
 * The link manager installs the USB host/CDC driver once, creates the ECM
 * driver/netif and recovers the WAN with bounded escalation (ECM rebuild ->
 * root port power cycle -> controlled reboot). It never tears down the USB
 * Host stack.
 *
 * @return ESP_OK when the task was created, an IDF error code otherwise; on
 *         failure the device stays AP/Web-only (the caller decides whether to
 *         keep running).
 */
esp_err_t cpe_wan_start(void);

/**
 * @brief Log free heap, minimum free heap and the largest internal block.
 *
 * Diagnostic helper used by the link manager on link transitions and by main
 * once at the end of boot.
 */
void cpe_wan_log_heap(const char *reason);

/**
 * @brief Request a scheduled rebuild of the ECM network interface.
 *
 * Safe to call from any task: it only raises a flag. The link manager performs
 * the teardown/bring-up on its own context on the next tick, because the ECM
 * handles are not protected by a lock and must never be touched by two tasks.
 * A scheduled rebuild is maintenance, not a fault: the recovery budget is not
 * consumed and no modem reset is issued (the USB session is kept).
 *
 * @note The request is dropped when link manager is not running.
 */
void cpe_wan_request_ecm_rebuild(void);

/**
 * @brief Reboot the device after the standard pre-restart cleanup.
 *
 * Optionally resets the modem (best effort) so it re-enumerates cleanly after
 * the ESP boots, then deauthenticates every SoftAP station. Never returns.
 *
 * @param reset_modem_first  Send AT+CFUN=1,1 before deauthenticating/restarting.
 */
void cpe_wan_reboot(bool reset_modem_first);

#ifdef __cplusplus
}
#endif
