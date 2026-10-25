/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cpe_random.h"

#include <stdint.h>
#include <string.h>

#include "esp_random.h"

void cpe_random_string(char *buf, size_t size, const char *alphabet)
{
    if (buf == NULL || alphabet == NULL || alphabet[0] == '\0' || size == 0) {
        return;
    }
    const size_t len = size - 1;
    const uint32_t alphabet_len = (uint32_t)strlen(alphabet);
    /* 2^32 is not a multiple of most alphabet sizes: reject the low values so
     * that the modulo below stays unbiased. */
    const uint32_t threshold = (0u - alphabet_len) % alphabet_len;
    for (size_t i = 0; i < len; i++) {
        uint32_t r;
        do {
            r = esp_random();
        } while (r < threshold);
        buf[i] = alphabet[r % alphabet_len];
    }
    buf[len] = '\0';
}
