/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runtime side of SMS forwarding: keeps an MQTTS connection to the broker the
 * user configured, pushes messages that arrive after start-up and executes
 * downlink send commands when the user allows remote sending. Configuration
 * shapes and the wire format live in cpe_sms_mqtt_proto.h so the host tests
 * can cover them.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "cpe_sms_mqtt_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CPE_SMS_MQTT_DISABLED = 0,  /* user switched forwarding off */
    CPE_SMS_MQTT_IDLE,          /* enabled, waiting for the WAN link */
    CPE_SMS_MQTT_CONNECTING,    /* TCP/TLS/MQTT handshake in progress */
    CPE_SMS_MQTT_CONNECTED,     /* subscribed and forwarding */
    CPE_SMS_MQTT_ERROR,         /* last attempt failed; retrying */
} cpe_sms_mqtt_state_t;

#define CPE_SMS_MQTT_ERROR_MAX       32

typedef struct {
    cpe_sms_mqtt_state_t state;
    bool wan_online;
    bool time_synced;
    bool cert_installed; /* a custom CA replaces the built-in public bundle */
    char last_error[CPE_SMS_MQTT_ERROR_MAX + 1];
    uint8_t queued;      /* downlink commands waiting for the worker */
    uint8_t pending;     /* uplinks published but not acknowledged yet */
    uint32_t free_heap;
} cpe_sms_mqtt_status_t;

/** Start the forwarding task. A no-op when the feature is configured out or
 *  init runs twice. */
esp_err_t cpe_sms_mqtt_init(void);

/** Copy the configuration in use (persisted values, or defaults). */
esp_err_t cpe_sms_mqtt_get_config(cpe_sms_mqtt_config_t *out);

/** Validate (cpe_sms_mqtt_config_check) and persist `cfg`, then reconnect with
 *  it. Returns ESP_ERR_INVALID_ARG when validation fails; use
 *  cpe_sms_mqtt_check_config() first to obtain the error code. */
esp_err_t cpe_sms_mqtt_set_config(const cpe_sms_mqtt_config_t *cfg);

/** Validate without saving. On failure *err (when non-NULL) points at a static
 *  code, see cpe_sms_mqtt_config_check(). */
esp_err_t cpe_sms_mqtt_check_config(const cpe_sms_mqtt_config_t *cfg,
                                    const char **err);

/** Install a PEM CA certificate that replaces the built-in public bundle for
 *  broker verification, and reconnect. Returns ESP_ERR_INVALID_ARG when the
 *  document is not a usable certificate (see cpe_sms_mqtt_pem_valid()). */
esp_err_t cpe_sms_mqtt_set_cert(const char *pem);

/** Remove the custom CA and reconnect against the built-in public bundle. */
esp_err_t cpe_sms_mqtt_clear_cert(void);

void cpe_sms_mqtt_get_status(cpe_sms_mqtt_status_t *out);

#ifdef __cplusplus
}
#endif
