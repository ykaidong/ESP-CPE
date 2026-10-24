/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host stub: the types and constants the tested sources use. The host runs every
 * suite on one thread, so the lock is a no-op.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef int BaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE              ((BaseType_t)1)
#define pdFALSE             ((BaseType_t)0)
#define portMAX_DELAY       ((TickType_t)0xFFFFFFFFU)
#define portTICK_PERIOD_MS  (1U)
