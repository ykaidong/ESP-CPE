/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host stub: heap queries only feed log messages, no tested logic depends on
 * the value.
 */

#pragma once

#include <stddef.h>

size_t esp_get_free_heap_size(void);
