/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
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

/* --- usb_wan.c ----------------------------------------------------------- */

esp_err_t wan_usb_cdc_install(void);
esp_err_t wan_boot_reset_module(void);
esp_err_t wan_ecm_bring_up(void);
void wan_ecm_teardown(void);
void wan_ecm_dhcp_restart(void);
bool wan_usb_modem_reset(void);
bool wan_usb_device_present(void);
esp_netif_t *wan_ecm_netif(void);

/* --- wan_net.c ----------------------------------------------------------- */

void wan_link_online(void);
void wan_link_offline(void);

/* --- link_manager.c ------------------------------------------------------ */

esp_err_t wan_link_manager_start(void);
void wan_log_heap(const char *reason);
