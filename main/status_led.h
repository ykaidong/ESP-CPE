/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * WS2812 status LED.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configure the RMT channel and switch the LED off. Call once at boot. */
esp_err_t status_led_init(void);

/** Start the status task. Safe to call when the LED is disabled in Kconfig. */
esp_err_t status_led_start(void);

/**
 * @brief Switch the LED to the factory-reset pattern (blue blink).
 *
 * Latching: once set, the network state no longer drives the LED until the
 * device reboots (the reset path reboots anyway).
 */
void status_led_factory_reset(void);

#ifdef __cplusplus
}
#endif
