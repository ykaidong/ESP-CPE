/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal stand-in for esp_err.h so the pure logic below can be compiled and run
 * on the host. Only what the tested sources use is declared here.
 *
 * The numeric values deliberately differ from the real ESP-IDF ones: tests
 * compare against these same macros, so the values are irrelevant, and picking
 * different ones makes it obvious that this is a host stub and not the IDF.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                (-1)
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105

const char *esp_err_to_name(esp_err_t err);
