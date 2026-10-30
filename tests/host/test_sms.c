/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for components/cpe_sms pure logic (UCS2 encoding, CPMS/CMTI parsing,
 * validation, concatenation).
 */

#include <stdio.h>
#include <stdlib.h>
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

static void test_parse_cmti(void)
{
    int index = -1;

    HT_CHECK(cpe_sms_parse_cmti("+CMTI: \"ME\",5", &index));
    HT_CHECK_INT(index, 5);

    index = -1;
    HT_CHECK(cpe_sms_parse_cmti("+CMTI: \"SM\",23", &index));
    HT_CHECK_INT(index, 23);

    index = -1;
    HT_CHECK(cpe_sms_parse_cmti("+CMTI: \"ME\", 7\r\n", &index));
    HT_CHECK_INT(index, 7);

    HT_CHECK(!cpe_sms_parse_cmti("+CMTI: \"ME\"", &index));
    HT_CHECK(!cpe_sms_parse_cmti("+CMT: \"+8613800138000\",,\"25/10/25\"", &index));
    HT_CHECK(!cpe_sms_parse_cmti("+CMTI: \"ME\",abc", &index));
    HT_CHECK(!cpe_sms_parse_cmti("+CMTI: \"ME\",-1", &index));
    HT_CHECK(!cpe_sms_parse_cmti(NULL, &index));
    HT_CHECK(!cpe_sms_parse_cmti("+CMTI: \"ME\",1", NULL));
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
    m->stat = CPE_SMS_STAT_READ;
    snprintf(m->from, sizeof(m->from), "%s", from);
    m->body = malloc(strlen(body) + 1);
    if (m->body != NULL) {
        strcpy(m->body, body);
    }
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
    list.items[1].stat = CPE_SMS_STAT_UNREAD;
    cpe_sms_merge_parts(&list);
    HT_CHECK_INT(list.count, 1);
    HT_CHECK_INT(list.items[0].stat, CPE_SMS_STAT_UNREAD);

    /* Concatenation stops on a UTF-8 boundary and flags truncation. */
    memset(&list, 0, sizeof(list));
    static char big[CPE_SMS_MAX_BODY];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    add_part(&list, 0, "+861", 80, 1, 2, big);
    add_part(&list, 1, "+861", 80, 2, 2, big);
    cpe_sms_merge_parts(&list);
    HT_CHECK_INT(list.count, 1);
    HT_CHECK_INT(strlen(list.items[0].body), CPE_SMS_MAX_BODY - 1);
    HT_CHECK(list.items[0].truncated);

    /* The second segment sits at the lower storage index, so merging moves the
     * base entry down; the vacated slot must not keep its body pointer, or
     * clearing the list frees it twice. */
    memset(&list, 0, sizeof(list));
    add_part(&list, 0, "+861", 90, 2, 2, "B");
    add_part(&list, 1, "+861", 90, 1, 2, "A");
    cpe_sms_merge_parts(&list);
    HT_CHECK_INT(list.count, 1);
    HT_CHECK_STR(list.items[0].body, "AB");
    HT_CHECK(list.items[1].body == NULL);
    cpe_sms_list_clear(&list);
    HT_CHECK_INT(list.count, 0);
    HT_CHECK(list.items[0].body == NULL);
}

void test_sms(void)
{
    test_ucs2_encode();
    test_number_valid();
    test_text_measure();
    test_parse_cpms();
    test_merge_parts();
    test_parse_cmti();
}
