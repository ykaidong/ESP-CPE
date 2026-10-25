/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cpe_config.h"
#include "cpe_auth.h"
#include "cpe_status.h"
#include "cpe_recovery.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the product core: configuration, auth, status, boot count.
 *
 * Must be called after nvs_flash_init() and after the default event loop has
 * been created (credentials/status only need NVS, but this keeps boot order in
 * one place).
 */
esp_err_t cpe_core_init(void);

#ifdef __cplusplus
}
#endif
