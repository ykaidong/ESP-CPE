/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the read-only AT telemetry task.
 *
 * Opens a second CDC port on the modem AT interface and periodically publishes
 * signal strength, carrier, registration state, model/firmware and IMEI into
 * cpe_status. The modem may not be enumerated yet; the task waits for it.
 *
 * @return ESP_OK when the task is running, an error otherwise.
 */
esp_err_t cpe_at_start(void);

#ifdef __cplusplus
}
#endif
