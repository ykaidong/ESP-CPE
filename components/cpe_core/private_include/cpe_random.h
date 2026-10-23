/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * One random string generator for the whole component (audit C4). The session
 * and CSRF tokens and the generated SoftAP password used to have their own
 * copy, with different alphabets and different truncation rules.
 */

#pragma once

#include <stddef.h>

/**
 * @brief Fill buf with uniformly chosen characters from alphabet.
 *
 * @param buf       Destination.
 * @param size      Capacity of buf: size - 1 characters are generated and the
 *                  terminator always lands inside the buffer (same rule as
 *                  snprintf). Pass a value below the real size to generate a
 *                  short string in a larger field: nothing beyond @p size is
 *                  touched, and buf[size - 1] is the terminator.
 * @param alphabet  Characters to choose from.
 */
void cpe_random_string(char *buf, size_t size, const char *alphabet);
