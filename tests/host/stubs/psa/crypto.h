/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host stub: only psa_crypto_init() is reached by the tested sources.
 */

#pragma once

#include <stdint.h>

typedef int32_t psa_status_t;

#define PSA_SUCCESS ((psa_status_t)0)

psa_status_t psa_crypto_init(void);
