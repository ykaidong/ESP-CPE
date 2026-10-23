/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

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

typedef enum {
    CPE_RECOVERY_NONE = 0,  /*!< Healthy, no recovery in progress */
    CPE_RECOVERY_USB,       /*!< Waiting for / recovering the USB link */
    CPE_RECOVERY_IP,        /*!< Link up but no IP (DHCP/PDP recovery) */
} cpe_recovery_stage_t;

typedef struct {
    cpe_state_t state;
    char state_text[48];
    bool usb_connected;
    bool link_up;
    bool got_ip;
    char ip[20];                /*!< IPv4 needs 16 bytes; the slack keeps a
                                 *   longer string from being cut silently */
    bool has_public_ip;
    char public_ip[20];
    int8_t rssi;                /*!< 0 when unknown, negative dBm otherwise */
    bool rssi_valid;
    char modem_model[40];
    char modem_fw[40];
    char imei[20];              /*!< 15 digit IMEI, slack for IMEISV */
    char carrier[32];
    bool modem_registered;      /*!< Modem registered on the network (CEREG) */
    bool sim_valid;             /*!< SIM state was read over AT */
    bool sim_inserted;          /*!< A SIM card is present (AT+CPIN?) */
    char phone_number[24];      /*!< MSISDN read over AT+CNUM, empty when unknown */
    bool ps_attached;           /*!< PS domain attached (AT+CGATT?) */
    bool ps_valid;              /*!< PS attach state was read over AT */
    bool pdp_active;            /*!< Any PDP context active (AT+CGACT?) */
    bool pdp_valid;             /*!< PDP state was read over AT */
    int16_t rsrp;               /*!< LTE RSRP from AT+QCSQ, dBm */
    int8_t rsrq;                /*!< LTE RSRQ from AT+QCSQ, dB */
    int8_t sinr;                /*!< LTE SINR from AT+QCSQ, dB */
    bool qcsq_valid;            /*!< QCSQ metrics were read for LTE */
    int8_t modem_temp;          /*!< Module temperature from AT+QTEMP, degrees C */
    bool modem_temp_valid;
    uint32_t uptime_s;
    uint32_t recovery_attempts;
    cpe_recovery_stage_t recovery_stage;
    uint32_t boot_count;
    char last_error[96];
} cpe_status_t;

/** Initialize the status snapshot. Returns ESP_ERR_NO_MEM when the mutex
 *  cannot be created. */
esp_err_t cpe_status_init(void);

/** Update the state (and its human readable text, printf style). */
void cpe_status_set_state(cpe_state_t state, const char *fmt, ...);

void cpe_status_set_usb_connected(bool connected);
void cpe_status_set_link(bool up);
void cpe_status_set_ip(const char *ip);          /* NULL or "" clears it */
void cpe_status_set_public_ip(const char *ip);
void cpe_status_set_rssi(int rssi, bool valid);
/** Model, firmware revision and IMEI read over the modem AT port. */
void cpe_status_set_modem_info(const char *model, const char *fw, const char *imei);
void cpe_status_set_carrier(const char *carrier);
void cpe_status_set_modem_registered(bool registered);
/** SIM presence reported by AT+CPIN?; valid stays false until the modem answers. */
void cpe_status_set_sim(bool inserted, bool valid);
void cpe_status_set_phone_number(const char *number);   /* NULL or "" clears it */
/** PS domain attach state reported by AT+CGATT?. */
void cpe_status_set_ps(bool attached, bool valid);
/** At least one PDP context active, reported by AT+CGACT?. */
void cpe_status_set_pdp(bool active, bool valid);
/** LTE radio metrics from AT+QCSQ (RSRP in dBm, RSRQ/SINR in dB). */
void cpe_status_set_signal_quality(int rsrp, int rsrq, int sinr, bool valid);
/** Module temperature in degrees Celsius reported by AT+QTEMP. */
void cpe_status_set_modem_temp(int temp_c, bool valid);
void cpe_status_set_recovery_attempts(uint32_t attempts);
void cpe_status_set_recovery_stage(cpe_recovery_stage_t stage);
void cpe_status_set_boot_count(uint32_t boot_count);
void cpe_status_set_error(const char *fmt, ...);

void cpe_status_get(cpe_status_t *out);

#ifdef __cplusplus
}
#endif
