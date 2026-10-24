/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host stub: the tested sources only need the two entry points. The host
 * implementation is deterministic, which is what a test wants.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

uint32_t esp_random(void);
void esp_fill_random(void *buf, size_t len);
