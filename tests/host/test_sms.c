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

    /* A supplementary-plane code point costs two UCS2 units. */
    HT_CHECK(cpe_sms_text_measure("\xf0\x9f\x98\x80", &ascii, &units));
    HT_CHECK(!ascii);
    HT_CHECK_INT(units, 2);

    /* Malformed UTF-8 is rejected without reading past the terminator: a
     * truncated sequence and a bad continuation byte. */
    HT_CHECK(!cpe_sms_text_measure("\xe4\xb8", &ascii, &units));
    HT_CHECK(!cpe_sms_text_measure("\xf0\x9f\x98", &ascii, &units));
    HT_CHECK(!cpe_sms_text_measure("\xe4\x41", &ascii, &units));

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

/* The modem's storage has more slots than the list used to hold, so records
 * past the cap must flag the list as truncated instead of silently vanishing. */
static void test_parse_cmgl_limit(void)
{
    char raw[4096];
    size_t off = 0;

    off += (size_t)snprintf(raw + off, sizeof(raw) - off, "AT+CMGL=\"ALL\"\r\n\r\n");
    for (int i = 1; i <= CPE_SMS_MAX_MESSAGES + 1; i++) {
        off += (size_t)snprintf(raw + off, sizeof(raw) - off,
                                "+CMGL: %d,\"REC READ\",\"0031\",,\"25/10/25,10:00:00+32\"\r\n"
                                "0041\r\n",
                                i);
    }
    snprintf(raw + off, sizeof(raw) - off, "\r\nOK\r\n");

    cpe_sms_list_t list;
    HT_CHECK(cpe_sms_parse_cmgl(raw, &list));
    HT_CHECK_INT(list.count, CPE_SMS_MAX_MESSAGES);
    HT_CHECK(list.truncated);
    HT_CHECK_STR(list.items[0].body, "A");
    HT_CHECK_INT(list.items[CPE_SMS_MAX_MESSAGES - 1].index, CPE_SMS_MAX_MESSAGES);
}

/* The response reader may stop at the "\r" of the final line, so the raw text
 * can end with "OK\r" and no "\n". The last message must not absorb it. */
static void test_parse_cmgl_cr_final(void)
{
    const char *raw =
        "AT+CMGL=\"ALL\"\r\n"
        "\r\n"
        "+CMGL: 1,\"REC UNREAD\",\"0031\",,\"25/10/25,10:00:00+32\"\r\n"
        "0041\r\n"
        "OK\r";

    cpe_sms_list_t list;
    HT_CHECK(cpe_sms_parse_cmgl(raw, &list));
    HT_CHECK_INT(list.count, 1);
    HT_CHECK_STR(list.items[0].body, "A");
}

static void test_parse_cpms(void)
{
    int used = -1;
    int total = -1;

    HT_CHECK(cpe_sms_parse_cpms("+CPMS: \"ME\",6,23,\"ME\",6,23,\"ME\",6,23\r\nOK\r\n",
                                &used, &total));
    HT_CHECK_INT(used, 6);
    HT_CHECK_INT(total, 23);

    HT_CHECK(cpe_sms_parse_cpms("+CPMS: 3,10,3,10,3,10", &used, &total));
    HT_CHECK_INT(used, 3);
    HT_CHECK_INT(total, 10);

    HT_CHECK(!cpe_sms_parse_cpms("OK", &used, &total));
    HT_CHECK(!cpe_sms_parse_cpms(NULL, &used, &total));
}

static void test_parse_qcmgr_seg(void)
{
    int uid = -1;
    int seg = -1;
    int total = -1;

    /* The SCTS field contains a comma; the last three fields are still the
     * segmentation triple. */
    HT_CHECK(cpe_sms_parse_qcmgr_seg(
        "+QCMGR: \"REC READ\",\"002B0038\",,\"26/10/25,16:32:59+32\",50,1,3\r\n", &uid, &seg,
        &total));
    HT_CHECK_INT(uid, 50);
    HT_CHECK_INT(seg, 1);
    HT_CHECK_INT(total, 3);

    HT_CHECK(!cpe_sms_parse_qcmgr_seg("+CMGL: 1,\"REC READ\"", &uid, &seg, &total));
    HT_CHECK(!cpe_sms_parse_qcmgr_seg("+QCMGR: \"REC READ\",\"1\",,\"t\",1,4,3\r\n", &uid, &seg,
                                      &total)); /* seg > total */
    /* A single message has no segment triple; the SCTS comma must not be
     * mistaken for one. */
    HT_CHECK(!cpe_sms_parse_qcmgr_seg(
        "+QCMGR: \"REC READ\",\"+8613800138000\",,\"26/10/25,16:32:59+32\"\r\nbody\r\nOK\r\n",
        &uid, &seg, &total));

    /* The real response keeps going with the body line and OK: the tail check
     * must stop at the end of the header, not at the end of the response. */
    HT_CHECK(cpe_sms_parse_qcmgr_seg(
        "+QCMGR: \"REC READ\",\"002B0038\",,\"26/10/25,16:32:59+32\",50,1,3\r\n"
        "592B5929573080054E0772694E4B900665C5\r\n\r\nOK\r\n",
        &uid, &seg, &total));
    HT_CHECK_INT(uid, 50);
    HT_CHECK_INT(seg, 1);
    HT_CHECK_INT(total, 3);
}

static void add_part(cpe_sms_list_t *list, int index, const char *from,
                     int uid, int seq, int total, const char *body)
{
    cpe_sms_msg_t *m = &list->items[list->count++];
    memset(m, 0, sizeof(*m));
    m->index = index;
    m->indexes[0] = index;
    m->index_count = 1;
    m->uid = uid;
    m->part_seq = (uint8_t)seq;
    m->part_total = (uint8_t)total;
    snprintf(m->stat, sizeof(m->stat), "%s", "REC READ");
    snprintf(m->from, sizeof(m->from), "%s", from);
    snprintf(m->body, sizeof(m->body), "%s", body);
}

static void test_merge_parts(void)
{
    cpe_sms_list_t list;

    /* Out-of-order segments merge by seq, not by storage index. */
    memset(&list, 0, sizeof(list));
    add_part(&list, 0, "+8618609412300", 50, 1, 3, "A");
    add_part(&list, 1, "+8618609412300", 50, 3, 3, "C");
    add_part(&list, 2, "+8618609412300", 50, 2, 3, "B");
    cpe_sms_merge_parts(&list);
    HT_CHECK_INT(list.count, 1);
    HT_CHECK_INT(list.items[0].index, 0);
    HT_CHECK_INT(list.items[0].index_count, 3);
    HT_CHECK_INT(list.items[0].indexes[0], 0);
    HT_CHECK_INT(list.items[0].indexes[1], 2);
    HT_CHECK_INT(list.items[0].indexes[2], 1);
    HT_CHECK_STR(list.items[0].body, "ABC");
    HT_CHECK(!list.items[0].truncated);

    /* A missing segment still merges what is there and flags the gap. */
    memset(&list, 0, sizeof(list));
    add_part(&list, 4, "+861", 51, 1, 3, "A");
    add_part(&list, 5, "+861", 51, 3, 3, "C");
    cpe_sms_merge_parts(&list);
    HT_CHECK_INT(list.count, 1);
    HT_CHECK_STR(list.items[0].body, "AC");
    HT_CHECK(list.items[0].truncated);

    /* Different references, senders or totals never merge together. */
    memset(&list, 0, sizeof(list));
    add_part(&list, 0, "+861", 60, 1, 2, "A");
    add_part(&list, 1, "+862", 60, 2, 2, "B");
    add_part(&list, 2, "+861", 61, 2, 2, "C");
    cpe_sms_merge_parts(&list);
    HT_CHECK_INT(list.count, 3);

    /* Unread on any part marks the merged message unread. */
    memset(&list, 0, sizeof(list));
    add_part(&list, 0, "+861", 70, 1, 2, "A");
    add_part(&list, 1, "+861", 70, 2, 2, "B");
    snprintf(list.items[1].stat, sizeof(list.items[1].stat), "REC UNREAD");
    cpe_sms_merge_parts(&list);
    HT_CHECK_INT(list.count, 1);
    HT_CHECK(cpe_sms_stat_unread(list.items[0].stat));

    /* Concatenation stops on a UTF-8 boundary and flags truncation. */
    memset(&list, 0, sizeof(list));
    char big[500];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    add_part(&list, 0, "+861", 80, 1, 2, big);
    add_part(&list, 1, "+861", 80, 2, 2, big);
    cpe_sms_merge_parts(&list);
    HT_CHECK_INT(list.count, 1);
    HT_CHECK_INT(strlen(list.items[0].body), CPE_SMS_MAX_BODY - 1);
    HT_CHECK(list.items[0].truncated);
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
    test_parse_cmgl_limit();
    test_parse_cmgl_cr_final();
    test_parse_cpms();
    test_parse_qcmgr_seg();
    test_merge_parts();
}
