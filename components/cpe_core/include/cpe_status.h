/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CPE_STATE_BOOT = 0,     /*!< Bootstrapping */
    CPE_STATE_USB_WAIT,     /*!< No ECM link yet (module missing or not linked) */
    CPE_STATE_LINK_WAIT,    /*!< Link up, waiting for the DHCP address */
    CPE_STATE_ONLINE,       /*!< Link up and IP acquired */
    CPE_STATE_FAULT,        /*!< Recoverable failure, AP/Web still available */
} cpe_state_t;

typedef struct {
    cpe_state_t state;
    char state_text[48];
    bool usb_connected;
    bool link_up;
    bool got_ip;
    char ip[16];
    bool has_public_ip;
    char public_ip[16];
    int8_t rssi;                /*!< 0 when unknown, negative dBm otherwise */
    bool rssi_valid;
    char modem_model[32];
    char modem_fw[32];
    uint32_t uptime_s;
    uint32_t recovery_attempts;
    uint32_t boot_count;
    char last_error[96];
} cpe_status_t;

void cpe_status_init(void);

/** Update the state (and its human readable text, printf style). */
void cpe_status_set_state(cpe_state_t state, const char *fmt, ...);

void cpe_status_set_usb_connected(bool connected);
void cpe_status_set_link(bool up);
void cpe_status_set_ip(const char *ip);          /* NULL or "" clears it */
void cpe_status_set_public_ip(const char *ip);
void cpe_status_set_rssi(int rssi, bool valid);
void cpe_status_set_recovery_attempts(uint32_t attempts);
void cpe_status_set_boot_count(uint32_t boot_count);
void cpe_status_set_error(const char *fmt, ...);

void cpe_status_get(cpe_status_t *out);

#ifdef __cplusplus
}
#endif
