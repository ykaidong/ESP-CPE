/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Interfaces shared by the cpe_wan sources. Private to the component.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "iot_eth.h"

/* --- usb_wan.c ----------------------------------------------------------- */

esp_err_t wan_usb_cdc_install(void);
esp_err_t wan_boot_reset_module(void);
esp_err_t wan_ecm_bring_up(void);
/* Returns ESP_OK when the driver is fully gone. A non-OK return means the
 * driver could not be uninstalled yet: the handle is kept on purpose so the
 * next wan_ecm_bring_up() finishes the teardown. Callers must not assume the
 * interface is down when it fails. */
esp_err_t wan_ecm_teardown(void);
void wan_ecm_dhcp_restart(void);
bool wan_usb_modem_reset(void);
bool wan_usb_device_present(void);
esp_netif_t *wan_ecm_netif(void);
iot_eth_handle_t wan_ecm_eth_handle(void);
/* Borrow the handles before dereferencing them from another task: see
 * wan_ecm_borrow() in usb_wan.c for the rules. Never blocks, always paired
 * with wan_ecm_release(). */
bool wan_ecm_borrow(void);
void wan_ecm_release(void);

/* --- wan_net.c ----------------------------------------------------------- */

void wan_link_online(void);
void wan_link_offline(void);
/* Deferred "re-apply NAPT and upstream DNS". The SoftAP restart handler asks
 * for it when it could not create a task, because that work blocks on the
 * TCP/IP thread and must therefore not run in the event loop. The link manager
 * owns the ECM handles and picks the request up on its next tick. */
void wan_request_napt_reapply(void);
bool wan_napt_reapply_take(void);

/* --- link_manager.c ------------------------------------------------------ */

esp_err_t wan_link_manager_start(void);
