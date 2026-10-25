/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Periodic maintenance for long-running CPE devices.
 *
 * The two periods are runtime configuration: they default to the Kconfig values
 * and are stored in their own NVS namespace ("cpe_maint"), so they can be
 * changed from the management console without rebuilding the firmware.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Upper bound accepted for both periods (30 days). 0 disables the action. */
#define CPE_MAINT_PERIOD_MAX_MIN    43200

/** Runtime maintenance policy (persisted in NVS). */
typedef struct {
    uint32_t ecm_period_min;      /*!< ECM interface rebuild period, 0 = disabled */
    uint32_t reboot_period_min;   /*!< Device reboot period, 0 = disabled */
    bool reset_modem;             /*!< Send AT+CFUN=1,1 before a scheduled reboot */
} cpe_maint_config_t;

/** Current policy plus the schedule the maintenance task is following. */
typedef struct {
    cpe_maint_config_t config;
    int64_t next_ecm_in_s;        /*!< < 0 = not scheduled (disabled/not started) */
    int64_t next_reboot_in_s;     /*!< < 0 = not scheduled (disabled/not started) */
    uint32_t sched_reboot_count;  /*!< Consecutive scheduled reboots without a steady period */
    bool reboot_frozen;           /*!< Scheduled reboot stopped: guard limit reached */
} cpe_maint_status_t;

/**
 * @brief Start the periodic maintenance task.
 *
 * One low-priority task (1 s tick) drives both timers:
 *
 *  - config.ecm_period_min: rebuild the ECM network interface. The SoftAP and
 *    the web console stay up, the modem is not reset, only the WAN interface is
 *    recreated (a few seconds without WAN). Executed by the link manager task.
 *  - config.reboot_period_min: reboot the ESP32 after the standard pre-restart
 *    cleanup (optional AT+CFUN=1,1, Wi-Fi deauth).
 *
 * Both actions are skipped while an OTA upload is in progress or an OTA image
 * is still pending verification. The scheduled reboot is bounded by a NVS
 * counter (CPE_MAINT_MAX_SCHED_REBOOTS) that is cleared once the device has
 * been steady (out of the boot phase) for CPE_HEALTHY_CLEAR_SECONDS, so a
 * firmware that cannot come back up stops rebooting. A missing SIM or modem
 * does not count as unhealthy.
 *
 * The task is created even when both periods are 0, so the console can enable
 * them at runtime.
 *
 * Must be called after cpe_wan_start(): the ECM rebuild is executed by the link
 * manager, and a scheduled reboot uses the shared pre-restart cleanup.
 *
 * @return ESP_OK when the task is running, an IDF error code otherwise.
 */
esp_err_t cpe_maintenance_start(void);

/**
 * @brief Read the current policy (thread safe).
 */
esp_err_t cpe_maint_config_get(cpe_maint_config_t *out);

/**
 * @brief Validate, persist and apply a new policy.
 *
 * The maintenance task picks the new periods up on its next tick and restarts
 * the countdown from that moment.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG for an out-of-range period, or the NVS
 *         error (nothing is changed in that case).
 */
esp_err_t cpe_maint_config_set(const cpe_maint_config_t *cfg);

/**
 * @brief Restore the Kconfig defaults and erase the stored policy.
 *
 * Used by the factory reset path. Never fails on a missing namespace.
 */
esp_err_t cpe_maint_config_reset(void);

/**
 * @brief Read the policy and the current schedule (thread safe).
 */
esp_err_t cpe_maint_status_get(cpe_maint_status_t *out);

#ifdef __cplusplus
}
#endif
