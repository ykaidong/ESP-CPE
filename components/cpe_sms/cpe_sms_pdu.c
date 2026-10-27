/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SMS-DELIVER PDU decoding: address, DCS, SCTS, UDH concatenation and the
 * three user-data alphabets we care about (7-bit, UCS2). 8-bit user data and
 * compressed text are reported as unsupported so the caller can keep the raw
 * hex instead of inventing text.
 */

#include <stdio.h>
#include <string.h>

#include "cpe_sms_pdu.h"

#define PDU_OCTET_MAX   (1 + 12 + 1 + 140) /* SCA length + SCA + TPDU */
#define PDU_REPLACEMENT 0xFFFD

/* GSM 7-bit default alphabet, 3GPP TS 23.038 6.2.1. 0x1B is the escape. */
static const uint16_t gsm7_table[128] = {
    0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
    0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,
    0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
    0x03A3, 0x0398, 0x039E, 0x0000, 0x00C6, 0x00E6, 0x00DF, 0x00C9,
    0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,
    0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0,
};

static uint16_t gsm7_extension(uint8_t v)
{
    switch (v) {
    case 0x0A: return 0x000C; /* form feed */
    case 0x14: return 0x005E;
    case 0x28: return 0x007B;
    case 0x29: return 0x007D;
    case 0x2F: return 0x005C;
    case 0x3C: return 0x005B;
    case 0x3D: return 0x007E;
    case 0x3E: return 0x005D;
    case 0x40: return 0x007C;
    case 0x65: return 0x20AC;
    default:   return PDU_REPLACEMENT;
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static bool hex_to_octets(const char *hex, uint8_t *out, size_t *len)
{
    size_t n = strlen(hex);
    if (n == 0 || (n & 1) != 0 || n / 2 > PDU_OCTET_MAX) {
        return false;
    }
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *len = n / 2;
    return true;
}

static bool append_cp(char *dst, size_t cap, size_t *len, uint32_t cp)
{
    uint8_t b[4] = { 0 };
    size_t n;
    if (cp < 0x80) {
        b[0] = (uint8_t)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (uint8_t)(0xC0 | (cp >> 6));
        b[1] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (uint8_t)(0xE0 | (cp >> 12));
        b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (uint8_t)(0xF0 | (cp >> 18));
        b[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*len + n + 1 > cap) {
        dst[*len] = '\0';
        return false;
    }
    memcpy(dst + *len, b, n);
    *len += n;
    dst[*len] = '\0';
    return true;
}

/* GSM 7-bit unpacking. Bit `bit_off` of `ud` is the first bit of the first
 * septet (after a UDH and its fill bits). Stops early when `dst` is full. */
static void decode_7bit(const uint8_t *ud, size_t bit_off, size_t septets,
                        char *dst, size_t cap, bool *truncated)
{
    size_t len = 0;
    bool escape = false;

    for (size_t i = 0; i < septets; i++) {
        size_t bit = bit_off + i * 7;
        uint8_t v = 0;
        for (size_t k = 0; k < 7; k++) {
            if ((ud[(bit + k) / 8] >> ((bit + k) % 8)) & 1) {
                v |= (uint8_t)(1u << k);
            }
        }
        uint16_t cp;
        if (escape) {
            escape = false;
            cp = gsm7_extension(v);
        } else if (v == 0x1B) {
            escape = true;
            continue;
        } else {
            cp = gsm7_table[v];
        }
        if (!append_cp(dst, cap, &len, cp)) {
            *truncated = true;
            return;
        }
    }
    if (escape && !append_cp(dst, cap, &len, PDU_REPLACEMENT)) {
        *truncated = true;
    }
}

static void decode_ucs2(const uint8_t *p, size_t bytes, char *dst, size_t cap,
                        bool *truncated)
{
    size_t len = 0;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint16_t u = (uint16_t)((p[i] << 8) | p[i + 1]);
        uint32_t cp = u;
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < bytes) {
            uint16_t lo = (uint16_t)((p[i + 2] << 8) | p[i + 3]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000u + ((uint32_t)(u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            } else {
                cp = PDU_REPLACEMENT;
            }
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            cp = PDU_REPLACEMENT;
        } else if (u == 0) {
            cp = PDU_REPLACEMENT;
        }
        if (!append_cp(dst, cap, &len, cp)) {
            *truncated = true;
            return;
        }
    }
}

static bool decode_number(uint8_t toa, const uint8_t *p, size_t octets,
                          size_t digits, char *dst, size_t cap)
{
    size_t w = 0;
    size_t got = 0;
    bool international = (toa & 0x70) == 0x10;

    if ((toa & 0x80) == 0 || digits == 0 || digits > 20) {
        return false;
    }
    if ((international ? 1 : 0) + digits + 1 > cap) {
        return false;
    }
    if (international) {
        dst[w++] = '+';
    }
    for (size_t i = 0; i < octets; i++) {
        for (int half = 0; half < 2; half++) {
            uint8_t n = half == 0 ? (uint8_t)(p[i] & 0x0F) : (uint8_t)(p[i] >> 4);
            if (got < digits) {
                if (n > 9) {
                    return false;
                }
                dst[w++] = (char)('0' + n);
                got++;
            } else if (n != 0x0F) {
                return false;
            }
        }
    }
    if (got != digits) {
        return false;
    }
    dst[w] = '\0';
    return true;
}

static bool decode_alpha(uint8_t toa, const uint8_t *p, size_t octets,
                         size_t length, char *dst, size_t cap, bool *truncated)
{
    size_t septets;

    if ((toa & 0x80) == 0) {
        return false;
    }
    septets = length * 4 / 7;
    if (septets == 0 || (septets * 7 + 7) / 8 > octets) {
        return false;
    }
    decode_7bit(p, 0, septets, dst, cap, truncated);
    return true;
}

/* Concatenation IE 0x00 (8-bit reference) or 0x08 (16-bit reference). */
static bool parse_udh(const uint8_t *udh, size_t len, cpe_sms_msg_t *out)
{
    size_t p = 1;

    while (p + 1 < len) {
        uint8_t iei = udh[p];
        uint8_t iedl = udh[p + 1];
        const uint8_t *ie = udh + p + 2;
        if (p + 2 + iedl > len) {
            return false;
        }
        if (iei == 0x00 && iedl >= 3) {
            uint8_t total = ie[1];
            uint8_t seq = ie[2];
            if (total >= 2 && seq >= 1 && seq <= total) {
                out->uid = ie[0];
                out->part_total = total;
                out->part_seq = seq;
                return true;
            }
        } else if (iei == 0x08 && iedl >= 4) {
            uint8_t total = ie[2];
            uint8_t seq = ie[3];
            if (total >= 2 && seq >= 1 && seq <= total) {
                out->uid = (ie[0] << 8) | ie[1];
                out->part_total = total;
                out->part_seq = seq;
                return true;
            }
        }
        p += 2 + iedl;
    }
    return false;
}

/* Semi-octet order is swapped: the low nibble holds the tens digit. */
static int bcd_digit(uint8_t octet, bool high, int *value)
{
    uint8_t n = high ? (uint8_t)(octet >> 4) : (uint8_t)(octet & 0x0F);
    if (n > 9) {
        return -1;
    }
    *value = n;
    return 0;
}

static bool format_scts(const uint8_t *s, char *dst, size_t cap)
{
    int d[6];

    for (int i = 0; i < 6; i++) {
        int tens;
        int units;
        if (bcd_digit(s[i], false, &tens) < 0 ||
            bcd_digit(s[i], true, &units) < 0) {
            return false;
        }
        d[i] = tens * 10 + units;
    }

    uint8_t raw = s[6];
    int tens = raw & 0x07;
    int units = (raw >> 4) & 0x0F;
    char sign = (raw & 0x08) != 0 ? '-' : '+';
    if (units > 9) {
        return false;
    }
    int written = snprintf(dst, cap, "%02d/%02d/%02d,%02d:%02d:%02d%c%02d",
                           d[0], d[1], d[2], d[3], d[4], d[5], sign,
                           tens * 10 + units);
    return written > 0 && (size_t)written < cap;
}

/* 0 = 7-bit, 1 = 8-bit, 2 = UCS2, negative = unsupported. */
static int dcs_alphabet(uint8_t dcs)
{
    if ((dcs & 0xC0) == 0x00) {
        if ((dcs & 0x20) != 0) {
            return -1; /* compressed */
        }
        return (dcs >> 2) & 0x03;
    }
    if ((dcs & 0xC0) == 0x40) {
        return (dcs >> 2) & 0x03;
    }
    if ((dcs & 0xF0) == 0xF0) {
        return (dcs & 0x04) != 0 ? 1 : 0;
    }
    if ((dcs & 0xC0) == 0xC0) {
        return 0; /* message waiting indication group */
    }
    return -1; /* reserved 1000xxxx */
}

cpe_sms_pdu_status_t cpe_sms_pdu_decode_deliver(const char *hex, cpe_sms_msg_t *out)
{
    uint8_t pdu[PDU_OCTET_MAX];
    size_t n = 0;
    size_t pos;

    if (hex == NULL || out == NULL) {
        return CPE_SMS_PDU_MALFORMED;
    }
    if (!hex_to_octets(hex, pdu, &n)) {
        return CPE_SMS_PDU_MALFORMED;
    }

    /* SMSC address, unused: the phone number that matters is TP-OA. */
    size_t sca = pdu[0];
    if (sca > 12 || 1 + sca >= n) {
        return CPE_SMS_PDU_MALFORMED;
    }
    pos = 1 + sca;

    uint8_t fo = pdu[pos++];
    if ((fo & 0x03) != 0x00) {
        return CPE_SMS_PDU_UNSUPPORTED; /* not SMS-DELIVER */
    }
    bool has_udh = (fo & 0x40) != 0;

    out->uid = 0;
    out->part_seq = 1;
    out->part_total = 1;

    if (pos >= n) {
        return CPE_SMS_PDU_MALFORMED;
    }
    size_t addr_len = pdu[pos++];
    size_t addr_octets = (addr_len + 1) / 2;
    if (addr_len == 0 || addr_len > 20 || pos + 1 + addr_octets > n) {
        return CPE_SMS_PDU_MALFORMED;
    }
    uint8_t toa = pdu[pos];
    bool truncated = false;
    if ((toa & 0x70) == 0x50) {
        if (!decode_alpha(toa, pdu + pos + 1, addr_octets, addr_len, out->from,
                          sizeof(out->from), &truncated)) {
            return CPE_SMS_PDU_MALFORMED;
        }
    } else {
        if (!decode_number(toa, pdu + pos + 1, addr_octets, addr_len,
                           out->from, sizeof(out->from))) {
            return CPE_SMS_PDU_MALFORMED;
        }
    }
    pos += 1 + addr_octets;

    if (pos + 2 + 7 + 1 > n) {
        return CPE_SMS_PDU_MALFORMED;
    }
    pos++; /* TP-PID */
    uint8_t dcs = pdu[pos++];
    int alphabet = dcs_alphabet(dcs);
    if (alphabet < 0 || alphabet == 3) {
        return CPE_SMS_PDU_UNSUPPORTED;
    }
    if (alphabet == 1) {
        return CPE_SMS_PDU_UNSUPPORTED; /* 8-bit data, caller keeps the hex */
    }
    if (!format_scts(pdu + pos, out->time, sizeof(out->time))) {
        return CPE_SMS_PDU_MALFORMED;
    }
    pos += 7;

    uint8_t udl = pdu[pos++];
    const uint8_t *ud = pdu + pos;
    size_t avail = n - pos;
    size_t bit_off = 0;
    size_t septets = 0;
    size_t text_octets = 0;

    if (alphabet == 0) {
        if (has_udh) {
            if (avail < 1) {
                return CPE_SMS_PDU_MALFORMED;
            }
            size_t hdr = (size_t)ud[0] + 1;
            size_t hdr_septets = (8 * hdr + 6) / 7;
            if (hdr > avail || udl < hdr_septets) {
                return CPE_SMS_PDU_MALFORMED;
            }
            septets = udl - hdr_septets;
            bit_off = hdr_septets * 7;
            text_octets = ((size_t)udl * 7 + 7) / 8;
            if (text_octets > avail) {
                return CPE_SMS_PDU_MALFORMED;
            }
            parse_udh(ud, hdr, out);
        } else {
            septets = udl;
            text_octets = ((size_t)udl * 7 + 7) / 8;
            if (text_octets > avail) {
                return CPE_SMS_PDU_MALFORMED;
            }
        }
        decode_7bit(ud, bit_off, septets, out->body, sizeof(out->body), &truncated);
    } else {
        size_t data_len;
        const uint8_t *text = ud;
        if (has_udh) {
            if (avail < 1) {
                return CPE_SMS_PDU_MALFORMED;
            }
            size_t hdr = (size_t)ud[0] + 1;
            if (hdr > avail || udl < hdr) {
                return CPE_SMS_PDU_MALFORMED;
            }
            data_len = udl - hdr;
            text = ud + hdr;
            parse_udh(ud, hdr, out);
        } else {
            data_len = udl;
        }
        if ((size_t)udl > avail || (data_len & 1) != 0) {
            return CPE_SMS_PDU_MALFORMED;
        }
        decode_ucs2(text, data_len, out->body, sizeof(out->body), &truncated);
    }

    out->truncated = truncated;
    return CPE_SMS_PDU_OK;
}
