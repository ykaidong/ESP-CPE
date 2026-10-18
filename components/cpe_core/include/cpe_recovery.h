/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bounded boot/recovery counter persisted in NVS.
 *
 * The counter is incremented before a controlled restart and cleared once the
 * device has been healthy for a while, so a crash/panic loop cannot restart
 * forever. It also survives panics (unlike plain RTC counters).
 */
uint32_t cpe_boot_count_get(void);
uint32_t cpe_boot_count_increment(void);
esp_err_t cpe_boot_count_clear(void);

#ifdef __cplusplus
}
#endif
