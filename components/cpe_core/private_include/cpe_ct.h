/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Constant-time comparison helpers shared inside cpe_core.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compare two byte buffers without early exit.
 *
 * @param a    First buffer (must not be NULL).
 * @param b    Second buffer (must not be NULL).
 * @param len  Number of bytes to compare.
 * @return true if all bytes match, false otherwise or on a NULL argument.
 */
bool cpe_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);

/**
 * @brief Compare two NUL-terminated strings without early exit.
 *
 * Returns false for NULL, empty or length-mismatched inputs. Length is
 * public information here (it is determined by strlen), only the content
 * comparison is constant time.
 */
bool cpe_ct_equal_str(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
