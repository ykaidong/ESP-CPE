/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Persistence of the forwarding configuration and the duplicate ring. Only
 * cpe_sms_mqtt.c uses these helpers; the web API goes through the public
 * functions so a save always triggers a reconnect.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "cpe_sms_mqtt.h"

/** Fill `cfg` with the factory defaults. */
void cpe_sms_mqtt_store_defaults(cpe_sms_mqtt_config_t *cfg);

/** Read the stored configuration. Returns ESP_ERR_NVS_NOT_FOUND when nothing
 *  has been saved yet (*cfg is then left as the caller set it). A stored
 *  configuration that no longer validates is returned with `enabled` cleared. */
esp_err_t cpe_sms_mqtt_store_load(cpe_sms_mqtt_config_t *cfg);

esp_err_t cpe_sms_mqtt_store_save(const cpe_sms_mqtt_config_t *cfg);

void cpe_sms_mqtt_store_ring_load(cpe_sms_mqtt_ring_t *ring);
esp_err_t cpe_sms_mqtt_store_ring_save(const cpe_sms_mqtt_ring_t *ring);

/** Read the installed CA certificate into a new heap buffer, or NULL when none
 *  is installed (or the stored copy no longer parses as PEM). The caller owns
 *  the buffer and frees it. */
char *cpe_sms_mqtt_store_cert_load(void);
esp_err_t cpe_sms_mqtt_store_cert_save(const char *pem);
esp_err_t cpe_sms_mqtt_store_cert_clear(void);

/** Erase the whole namespace. Used by the factory reset hook. */
void cpe_sms_mqtt_store_erase_all(void);
