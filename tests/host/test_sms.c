/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for components/cpe_sms pure logic (UCS2, CMGL parsing, validation).
 */

#include <stdio.h>
#include <string.h>

#include "cpe_sms_parse.h"
#include "cpe_sms_ucs2.h"
#include "test_harness.h"

static void test_ucs2_encode(void)
{
    char dst[64];

    HT_CHECK_INT(cpe_sms_ucs2_encode("AB", dst, sizeof(dst)), 8);
    HT_CHECK_STR(dst, "00410042");

    HT_CHECK_INT(cpe_sms_ucs2_encode("\xe4\xb8\xad", dst, sizeof(dst)), 4); /* 中 */
    HT_CHECK_STR(dst, "4E2D");

    /* U+1F600 -> surrogate pair D83D DE00 */
    HT_CHECK_INT(cpe_sms_ucs2_encode("\xf0\x9f\x98\x80", dst, sizeof(dst)), 8);
    HT_CHECK_STR(dst, "D83DDE00");

    HT_CHECK_INT(cpe_sms_ucs2_encode("\xff", dst, sizeof(dst)), 0); /* bad UTF-8 */
}

static void test_ucs2_decode(void)
{
    char dst[64];

    HT_CHECK_INT(cpe_sms_ucs2_decode("4E2D6587", dst, sizeof(dst)), 6); /* 中文 */
    HT_CHECK_STR(dst, "\xe4\xb8\xad\xe6\x96\x87");

    HT_CHECK_INT(cpe_sms_ucs2_decode("00410042", dst, sizeof(dst)), 2);
    HT_CHECK_STR(dst, "AB");

    /* Not hex -> pass through. */
    HT_CHECK_INT(cpe_sms_ucs2_decode("hello", dst, sizeof(dst)), 5);
    HT_CHECK_STR(dst, "hello");

    /* Odd length -> pass through. */
    HT_CHECK_INT(cpe_sms_ucs2_decode("4E2", dst, sizeof(dst)), 3);
    HT_CHECK_STR(dst, "4E2");

    /* CRLF is printable enough to survive. */
    HT_CHECK_INT(cpe_sms_ucs2_decode("000D000A", dst, sizeof(dst)), 2);
    HT_CHECK_STR(dst, "\r\n");

    /* A control character falls back to the raw hex. */
    HT_CHECK_INT(cpe_sms_ucs2_decode("0001", dst, sizeof(dst)), 4);
    HT_CHECK_STR(dst, "0001");
}

static void test_number_valid(void)
{
    HT_CHECK(cpe_sms_number_valid("+8613800138000"));
    HT_CHECK(cpe_sms_number_valid("13800138000"));
    HT_CHECK(!cpe_sms_number_valid("12"));
    HT_CHECK(!cpe_sms_number_valid("+123456789012345678901")); /* 21 digits */
    HT_CHECK(!cpe_sms_number_valid("abc"));
    HT_CHECK(!cpe_sms_number_valid("+86a"));
    HT_CHECK(!cpe_sms_number_valid(NULL));
}

static void test_text_measure(void)
{
    bool ascii = false;
    int units = 0;

    HT_CHECK(cpe_sms_text_measure("hello", &ascii, &units));
    HT_CHECK(ascii);
    HT_CHECK_INT(units, 5);

    char long_ascii[162];
    memset(long_ascii, 'a', 161);
    long_ascii[161] = '\0';
    HT_CHECK(!cpe_sms_text_measure(long_ascii, &ascii, &units));

    HT_CHECK(cpe_sms_text_measure("\xe4\xb8\xad\xe6\x96\x87", &ascii, &units));
    HT_CHECK(!ascii);
    HT_CHECK_INT(units, 2);

    HT_CHECK(!cpe_sms_text_measure("", &ascii, &units));
    HT_CHECK(!cpe_sms_text_measure(NULL, &ascii, &units));
}

static void test_stat_unread(void)
{
    HT_CHECK(cpe_sms_stat_unread("REC UNREAD"));
    HT_CHECK(!cpe_sms_stat_unread("REC READ"));
    HT_CHECK(cpe_sms_stat_unread("unread"));
    HT_CHECK(!cpe_sms_stat_unread("STO UNSENT"));
    HT_CHECK(!cpe_sms_stat_unread(NULL));
}

static void test_parse_cmgl(void)
{
    const char *raw =
        "AT+CMGL=\"ALL\"\r\n"
        "\r\n"
        "+CMGL: 1,\"REC UNREAD\",\"+8613800138000\",,\"25/10/25,10:00:00+32\"\r\n"
        "4E2D6587\r\n"
        "\r\n"
        "+CMGL: 2,\"REC READ\",\"00410042\",,\"25/10/25,11:00:00+32\"\r\n"
        "00480065006C006C006F\r\n"
        "\r\n"
        "OK\r\n";

    cpe_sms_list_t list;
    HT_CHECK(cpe_sms_parse_cmgl(raw, &list));
    HT_CHECK_INT(list.count, 2);
    HT_CHECK(!list.truncated);

    HT_CHECK_INT(list.items[0].index, 1);
    HT_CHECK_STR(list.items[0].stat, "REC UNREAD");
    HT_CHECK(cpe_sms_stat_unread(list.items[0].stat));
    HT_CHECK_STR(list.items[0].from, "+8613800138000");
    HT_CHECK_STR(list.items[0].time, "25/10/25,10:00:00+32");
    HT_CHECK_STR(list.items[0].body, "\xe4\xb8\xad\xe6\x96\x87");

    HT_CHECK_INT(list.items[1].index, 2);
    HT_CHECK_STR(list.items[1].from, "AB");
    HT_CHECK_STR(list.items[1].body, "Hello");
}

/* A full-length message: the UCS2 hex for a 160-char body and a numeric sender
 * are far longer than the decoded text, so this guards against truncating the
 * raw hex before decoding (which leaked raw hex into the UI). */
static void test_parse_cmgl_long(void)
{
    char body_hex[CPE_SMS_MAX_BODY_HEX];
    for (int i = 0; i < 160; i++) {
        body_hex[i * 4 + 0] = '0';
        body_hex[i * 4 + 1] = '0';
        body_hex[i * 4 + 2] = '4';
        body_hex[i * 4 + 3] = '1';
    }
    body_hex[640] = '\0';

    char raw[4096];
    snprintf(raw, sizeof(raw),
             "AT+CMGL=\"ALL\"\r\n\r\n"
             "+CMGL: 1,\"REC UNREAD\",\"00310033003800300030003100330038003000300030\",,\"25/10/25,10:00:00+32\"\r\n"
             "%s\r\n\r\nOK\r\n", body_hex);

    cpe_sms_list_t list;
    HT_CHECK(cpe_sms_parse_cmgl(raw, &list));
    HT_CHECK_INT(list.count, 1);
    HT_CHECK_STR(list.items[0].from, "13800138000");
    HT_CHECK_INT(strlen(list.items[0].body), 160);
    for (int i = 0; i < 160; i++) {
        HT_CHECK(list.items[0].body[i] == 'A');
    }
}

/* A message whose UCS2 hex body is emitted across several lines (embedded
 * newline) must be reassembled into one entry, not dropped or split. */
static void test_parse_cmgl_multiline(void)
{
    const char *raw =
        "AT+CMGL=\"ALL\"\r\n"
        "\r\n"
        "+CMGL: 1,\"REC UNREAD\",\"0031\",,\"25/10/25,10:00:00+32\"\r\n"
        "00410041\r\n"
        "00420042\r\n"
        "+CMGL: 2,\"REC READ\",\"0032\",,\"25/10/25,11:00:00+32\"\r\n"
        "0043\r\n"
        "\r\n"
        "OK\r\n";

    cpe_sms_list_t list;
    HT_CHECK(cpe_sms_parse_cmgl(raw, &list));
    HT_CHECK_INT(list.count, 2);
    HT_CHECK_STR(list.items[0].from, "1");
    HT_CHECK_STR(list.items[0].body, "AABB");
    HT_CHECK_STR(list.items[1].from, "2");
    HT_CHECK_STR(list.items[1].body, "C");
}

void test_sms(void)
{
    test_ucs2_encode();
    test_ucs2_decode();
    test_number_valid();
    test_text_measure();
    test_stat_unread();
    test_parse_cmgl();
    test_parse_cmgl_long();
    test_parse_cmgl_multiline();
}
