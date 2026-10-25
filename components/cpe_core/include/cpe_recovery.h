/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
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

/**
 * @brief Number of consecutive scheduled reboots without a healthy period.
 *
 * Kept apart from the boot/recovery counter on purpose: a planned reboot is
 * maintenance, not a fault, so it must not consume the CPE_MAX_REBOOTS budget.
 * The value is cleared once the device has been online for
 * CPE_HEALTHY_CLEAR_SECONDS, which makes a firmware that cannot come back up
 * stop rebooting after CPE_MAINT_MAX_SCHED_REBOOTS attempts.
 *
 * @note Only the maintenance task reads/writes these keys, so the
 *       read-modify-write below never races with another task.
 */
esp_err_t cpe_maint_reboot_count_get(uint32_t *count);

/** @brief Increment and persist the scheduled-reboot counter. */
esp_err_t cpe_maint_reboot_count_increment(uint32_t *count);

/** @brief Reset the scheduled-reboot counter after a healthy period. */
esp_err_t cpe_maint_reboot_count_clear(void);

#ifdef __cplusplus
}
#endif
