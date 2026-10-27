/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

/* Encode a UTF-8 string into UCS2 hex (4 uppercase hex digits per code unit,
 * surrogate pairs for supplementary code points). Returns the number of hex
 * characters written (excluding the terminating NUL), or 0 on a bad UTF-8 byte
 * or a full buffer. dst must hold at least 4 * strlen(src) + 1 bytes. */
size_t cpe_sms_ucs2_encode(const char *src, char *dst, size_t dst_cap);
