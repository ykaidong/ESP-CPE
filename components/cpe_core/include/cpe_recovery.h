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
 *
 * @note get() may be called from any task. increment()/clear() perform a NVS
 *       read-modify-write and must not run concurrently; they are used from
 *       the boot path and the factory-reset path only.
 */
/**
 * @brief Read the counter (0 when it has never been written).
 *
 * @param count  Receives the value; 0 when NVS is unreadable.
 * @return ESP_OK, or the NVS error.
 */
esp_err_t cpe_boot_count_get(uint32_t *count);

/**
 * @brief Increment and persist the counter.
 *
 * @param count  Receives the new value even if persisting it failed.
 * @return ESP_OK, or the NVS error (the stored value is then unchanged).
 */
esp_err_t cpe_boot_count_increment(uint32_t *count);

esp_err_t cpe_boot_count_clear(void);

#ifdef __cplusplus
}
#endif
