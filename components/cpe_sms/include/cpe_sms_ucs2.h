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

/* Decode a UCS2 hex string into UTF-8. When the input is not plausibly UCS2
 * (too short, odd length, or a non-hex byte) it is copied through unchanged.
 * A decode whose result would contain a control character other than \t \r \n
 * also falls back to the raw input. Returns the number of bytes written
 * (excluding the terminating NUL). */
size_t cpe_sms_ucs2_decode(const char *src, char *dst, size_t dst_cap);
