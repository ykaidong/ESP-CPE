/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded wait for cpe_at_modem_reset(): covers one 2 s AT command plus the
 * task wake-up and schedule jitter. Configurable via
 * CONFIG_CPE_AT_MODEM_RESET_TIMEOUT_MS; the fallback keeps the call sites
 * compiling when AT telemetry (and therefore the option) is disabled. */
#ifdef CONFIG_CPE_AT_MODEM_RESET_TIMEOUT_MS
#define CPE_AT_MODEM_RESET_TIMEOUT_MS  CONFIG_CPE_AT_MODEM_RESET_TIMEOUT_MS
#else
#define CPE_AT_MODEM_RESET_TIMEOUT_MS  3000
#endif

/**
 * @brief Start the AT telemetry task.
 *
 * Opens a second CDC port on the modem AT interface and periodically publishes
 * signal strength, carrier, registration state, model/firmware and IMEI into
 * cpe_status. The modem may not be enumerated yet; the task waits for it.
 *
 * @return ESP_OK when the task is running, an error otherwise.
 */
esp_err_t cpe_at_start(void);

/**
 * @brief Ask the AT task to reset the modem (AT+CFUN=1,1) before a restart.
 *
 * Safe to call from any task; the command is executed on the AT task, which
 * owns the AT CDC port. Best effort: returns immediately when no AT port is
 * ready, so a restart is never delayed by an absent or unresponsive modem.
 * Callers must proceed with their restart regardless of the result.
 *
 * @param timeout_ms Maximum time to wait for the AT task to finish trying.
 * @return ESP_OK               The modem answered OK.
 *         ESP_FAIL             The command was sent but no OK was received.
 *         ESP_ERR_INVALID_STATE Not started, no AT port ready, or another
 *                               reset request is already in flight.
 *         ESP_ERR_TIMEOUT      The AT task did not finish in time.
 *         ESP_ERR_NOT_SUPPORTED CONFIG_CPE_AT_ENABLE is disabled.
 */
esp_err_t cpe_at_modem_reset(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
