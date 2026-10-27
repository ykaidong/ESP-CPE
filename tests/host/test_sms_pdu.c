/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for components/cpe_sms/cpe_sms_pdu.c. Vectors are SMS-DELIVER PDUs
 * (SMSC address included) with SCA absent and SCTS 25/10/25,10:00:00+32 unless
 * stated otherwise.
 */

#include <stdio.h>
#include <string.h>

#include "cpe_sms_parse.h"
#include "cpe_sms_pdu.h"
#include "test_harness.h"

#define PDU_UCS2_CN \
    "00040D91683108108300F0000852015201000023044E2D6587"
#define PDU_7BIT_HELLO \
    "00040D91683108108300F000005201520100002306C8329BFD0E01"
#define PDU_EXT_EURO \
    "00040D91683108108300F0000052015201000023029B32"
#define PDU_SURROGATE \
    "00040D91683108108300F000085201520100002304D83DDE00"
#define PDU_CONCAT8_SEG1 \
    "00440D91683108108300F00000520152010000230A0500032A020182C221"
#define PDU_CONCAT8_SEG2 \
    "00440D91683108108300F00000520152010000230A0500032A0202884523"
#define PDU_CONCAT16 \
    "00440D91683108108300F00008520152010000230B060804002A03014E2D6587"
#define PDU_ALPHA_SENDER \
    "000408D0C2B07B0D00005201520100002302C834"
#define PDU_NEG_TZ \
    "00040D91683108108300F000005201520100002B02C834"
#define PDU_8BIT \
    "00040D91683108108300F000045201520100002304DEADBEEF"
#define PDU_WITH_SCA \
    "07919730071111F0040D91683108108300F000005201520100002302C834"

/* Full 140-octet user data (a concatenation segment of 16-bit reference). */
#define PDU_FULL_UCS2_CONCAT \
    "00440D91683108108300F00008520152010000238D0608041234030100410041" \
    "0041004100410041004100410041004100410041004100410041004100410041" \
    "0041004100410041004100410041004100410041004100410041004100410041" \
    "0041004100410041004100410041004100410041004100410041004100410041" \
    "0041004100410041004100410041004100410041004100410041004100410041" \
    "005A"
/* Longest possible PDU: 12-octet SMSC, 20-digit originator, 140-octet body. */
#define PDU_MAX_SIZE \
    "0C912B2B2B2B2B2B2B2B2B2B2B04149121436587092143658709000852015201" \
    "0000238C00420042004200420042004200420042004200420042004200420042" \
    "0042004200420042004200420042004200420042004200420042004200420042" \
    "0042004200420042004200420042004200420042004200420042004200420042" \
    "0042004200420042004200420042004200420042004200420042004200420042" \
    "00420042004200420042004200420043"
/* 160 septets, no UDH, still 140 octets once packed. */
#define PDU_FULL_7BIT \
    "00040D91683108108300F0000052015201000023A0783C1E8FC7E3F1783C1E8F" \
    "C7E3F1783C1E8FC7E3F1783C1E8FC7E3F1783C1E8FC7E3F1783C1E8FC7E3F178" \
    "3C1E8FC7E3F1783C1E8FC7E3F1783C1E8FC7E3F1783C1E8FC7E3F1783C1E8FC7" \
    "E3F1783C1E8FC7E3F1783C1E8FC7E3F1783C1E8FC7E3F1783C1E8FC7E3F1783C" \
    "1E8FC7E3F1783C1E8FC7E3F1783C1E8FC7E3F1783C1E8FC7E3F1783C1E8FC7E3" \
    "F3"

static void decode_ok(const char *hex, cpe_sms_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    HT_CHECK_INT(cpe_sms_pdu_decode_deliver(hex, m), CPE_SMS_PDU_OK);
}

static void test_ucs2_body(void)
{
    cpe_sms_msg_t m;
    decode_ok(PDU_UCS2_CN, &m);
    HT_CHECK_STR(m.from, "+8613800138000");
    HT_CHECK_STR(m.time, "25/10/25,10:00:00+32");
    HT_CHECK_STR(m.body, "\xe4\xb8\xad\xe6\x96\x87");
    HT_CHECK_INT(m.uid, 0);
    HT_CHECK_INT(m.part_seq, 1);
    HT_CHECK_INT(m.part_total, 1);
    HT_CHECK(!m.truncated);

    /* Surrogate pairs become four UTF-8 bytes. */
    decode_ok(PDU_SURROGATE, &m);
    HT_CHECK_STR(m.body, "\xf0\x9f\x98\x80");
}

static void test_7bit_body(void)
{
    cpe_sms_msg_t m;
    decode_ok(PDU_7BIT_HELLO, &m);
    HT_CHECK_STR(m.body, "Hello!");

    /* The escape octet maps to the extension table. */
    decode_ok(PDU_EXT_EURO, &m);
    HT_CHECK_STR(m.body, "\xe2\x82\xac");
}

static void test_sender_and_time(void)
{
    cpe_sms_msg_t m;
    decode_ok(PDU_ALPHA_SENDER, &m);
    HT_CHECK_STR(m.from, "Bank");
    HT_CHECK_STR(m.body, "Hi");

    decode_ok(PDU_NEG_TZ, &m);
    HT_CHECK_STR(m.time, "25/10/25,10:00:00-32");

    decode_ok(PDU_WITH_SCA, &m);
    HT_CHECK_STR(m.from, "+8613800138000");
    HT_CHECK_STR(m.body, "Hi");
}

static void test_concat_header(void)
{
    cpe_sms_msg_t m;
    decode_ok(PDU_CONCAT8_SEG1, &m);
    HT_CHECK_INT(m.uid, 42);
    HT_CHECK_INT(m.part_seq, 1);
    HT_CHECK_INT(m.part_total, 2);
    HT_CHECK_STR(m.body, "ABC");

    decode_ok(PDU_CONCAT8_SEG2, &m);
    HT_CHECK_INT(m.uid, 42);
    HT_CHECK_INT(m.part_seq, 2);
    HT_CHECK_INT(m.part_total, 2);
    HT_CHECK_STR(m.body, "DEF");

    /* 16-bit reference, UCS2 text, one of three. */
    decode_ok(PDU_CONCAT16, &m);
    HT_CHECK_INT(m.uid, 42);
    HT_CHECK_INT(m.part_seq, 1);
    HT_CHECK_INT(m.part_total, 3);
    HT_CHECK_STR(m.body, "\xe4\xb8\xad\xe6\x96\x87");
}

/* A segment of a long message fills its whole 140-octet user data field, so
 * its PDU is longer than any single-part message and still has to decode. */
static void test_full_length_pdus(void)
{
    static char expect[CPE_SMS_MAX_BODY];
    cpe_sms_msg_t m;

    decode_ok(PDU_FULL_UCS2_CONCAT, &m);
    memset(expect, 'A', 66);
    expect[66] = 'Z';
    expect[67] = '\0';
    HT_CHECK_STR(m.body, expect);
    HT_CHECK_INT(m.uid, 0x1234);
    HT_CHECK_INT(m.part_seq, 1);
    HT_CHECK_INT(m.part_total, 3);
    HT_CHECK(!m.truncated);

    decode_ok(PDU_MAX_SIZE, &m);
    HT_CHECK_STR(m.from, "+12345678901234567890");
    memset(expect, 'B', 69);
    expect[69] = 'C';
    expect[70] = '\0';
    HT_CHECK_STR(m.body, expect);

    decode_ok(PDU_FULL_7BIT, &m);
    memset(expect, 'x', 159);
    expect[159] = 'y';
    expect[160] = '\0';
    HT_CHECK_STR(m.body, expect);
}

static void test_merge_decoded_parts(void)
{
    cpe_sms_list_t list;
    memset(&list, 0, sizeof(list));

    /* Storage order is not segment order: the merge must sort by part_seq. */
    decode_ok(PDU_CONCAT8_SEG2, &list.items[list.count++]);
    decode_ok(PDU_CONCAT8_SEG1, &list.items[list.count++]);
    list.items[0].index = 7;
    list.items[1].index = 3;

    cpe_sms_merge_parts(&list);
    HT_CHECK_INT(list.count, 1);
    HT_CHECK_STR(list.items[0].body, "ABCDEF");
    HT_CHECK_INT(list.items[0].index, 3);
    HT_CHECK_INT(list.items[0].index_count, 2);
    HT_CHECK_INT(list.items[0].indexes[0], 3);
    HT_CHECK_INT(list.items[0].indexes[1], 7);
    HT_CHECK(!list.items[0].truncated);
}

static void test_empty_body(void)
{
    cpe_sms_msg_t m;
    decode_ok("00040D91683108108300F000005201520100002300", &m);
    HT_CHECK_STR(m.body, "");
    HT_CHECK_STR(m.from, "+8613800138000");

    decode_ok("00040D91683108108300F000085201520100002300", &m);
    HT_CHECK_STR(m.body, "");
}

static void test_malformed(void)
{
    cpe_sms_msg_t m;

    memset(&m, 0, sizeof(m));
    HT_CHECK_INT(cpe_sms_pdu_decode_deliver(NULL, &m), CPE_SMS_PDU_MALFORMED);

    memset(&m, 0, sizeof(m));
    HT_CHECK_INT(cpe_sms_pdu_decode_deliver("", &m), CPE_SMS_PDU_MALFORMED);

    memset(&m, 0, sizeof(m));
    HT_CHECK_INT(cpe_sms_pdu_decode_deliver("0004ZZ", &m), CPE_SMS_PDU_MALFORMED);

    memset(&m, 0, sizeof(m));
    HT_CHECK_INT(cpe_sms_pdu_decode_deliver("000", &m), CPE_SMS_PDU_MALFORMED);

    /* SMSC length announces octets that are not there. */
    memset(&m, 0, sizeof(m));
    HT_CHECK_INT(cpe_sms_pdu_decode_deliver("0799", &m), CPE_SMS_PDU_MALFORMED);

    /* UDL says six 7-bit octets, the PDU carries five. */
    memset(&m, 0, sizeof(m));
    HT_CHECK_INT(cpe_sms_pdu_decode_deliver(
                     "00040D91683108108300F000005201520100002306C8329BFD0E", &m),
                 CPE_SMS_PDU_MALFORMED);
    HT_CHECK_STR(m.body, "");

    /* UCS2 with an odd byte count cannot be decoded. */
    memset(&m, 0, sizeof(m));
    HT_CHECK_INT(cpe_sms_pdu_decode_deliver(
                     "00040D91683108108300F000085201520100002303616263", &m),
                 CPE_SMS_PDU_MALFORMED);
}

static void test_unsupported(void)
{
    cpe_sms_msg_t m;

    /* 8-bit user data is not SMS text; the caller keeps the raw hex. */
    memset(&m, 0, sizeof(m));
    HT_CHECK_INT(cpe_sms_pdu_decode_deliver(PDU_8BIT, &m), CPE_SMS_PDU_UNSUPPORTED);
    HT_CHECK_STR(m.body, "");

    /* Compressed user data (DCS bit 5) is not decoded either. */
    memset(&m, 0, sizeof(m));
    HT_CHECK_INT(cpe_sms_pdu_decode_deliver(
                     "00040D91683108108300F000285201520100002306C8329BFD0E01", &m),
                 CPE_SMS_PDU_UNSUPPORTED);

    /* Not an SMS-DELIVER (MTI 1 = SMS-SUBMIT). */
    memset(&m, 0, sizeof(m));
    HT_CHECK_INT(cpe_sms_pdu_decode_deliver(
                     "00010D91683108108300F000005201520100002306C8329BFD0E01", &m),
                 CPE_SMS_PDU_UNSUPPORTED);
}

static void test_parse_cmgl_pdu(void)
{
    const char *raw =
        "AT+CMGL=4\r\n"
        "\r\n"
        "+CMGL: 1,0,,25\r\n"
        PDU_UCS2_CN "\r\n"
        "\r\n"
        "+CMGL: 2,1,\"+8613800138000\",27\r\n"
        PDU_7BIT_HELLO "\r\n"
        "\r\n"
        "OK\r\n";

    cpe_sms_list_t list;
    HT_CHECK(cpe_sms_parse_cmgl_pdu(raw, &list));
    HT_CHECK_INT(list.count, 2);
    HT_CHECK(!list.truncated);
    HT_CHECK_INT(list.storage_used, -1);
    HT_CHECK_INT(list.storage_capacity, -1);

    HT_CHECK_INT(list.items[0].index, 1);
    HT_CHECK_INT(list.items[0].stat, CPE_SMS_STAT_UNREAD);
    HT_CHECK_STR(list.items[0].from, "+8613800138000");
    HT_CHECK_STR(list.items[0].time, "25/10/25,10:00:00+32");
    HT_CHECK_STR(list.items[0].body, "\xe4\xb8\xad\xe6\x96\x87");
    HT_CHECK(!list.items[0].truncated);

    HT_CHECK_INT(list.items[1].index, 2);
    HT_CHECK_INT(list.items[1].stat, CPE_SMS_STAT_READ);
    HT_CHECK_STR(list.items[1].body, "Hello!");
}

static void test_parse_cmgl_pdu_undecodable(void)
{
    /* A malformed PDU and an 8-bit message are both kept with their raw hex so
     * they can still be listed and deleted. The response ends at "OK\r". */
    const char *raw =
        "AT+CMGL=4\r\n"
        "+CMGL: 5,1,,5\r\n"
        "00DEADBEEF\r\n"
        "+CMGL: 6,0,,25\r\n"
        PDU_8BIT "\r\n"
        "OK\r";

    cpe_sms_list_t list;
    HT_CHECK(cpe_sms_parse_cmgl_pdu(raw, &list));
    HT_CHECK_INT(list.count, 2);
    HT_CHECK_INT(list.items[0].index, 5);
    HT_CHECK_INT(list.items[0].stat, CPE_SMS_STAT_READ);
    HT_CHECK_STR(list.items[0].from, "");
    HT_CHECK_STR(list.items[0].body, "00DEADBEEF");
    HT_CHECK(list.items[0].truncated);

    HT_CHECK_INT(list.items[1].index, 6);
    HT_CHECK_INT(list.items[1].stat, CPE_SMS_STAT_UNREAD);
    HT_CHECK_STR(list.items[1].body, PDU_8BIT);
    HT_CHECK(list.items[1].truncated);
}

static void test_parse_cmgl_pdu_merge(void)
{
    const char *raw =
        "AT+CMGL=4\r\n"
        "+CMGL: 1,0,,30\r\n"
        PDU_CONCAT8_SEG2 "\r\n"
        "+CMGL: 2,0,,30\r\n"
        PDU_CONCAT8_SEG1 "\r\n"
        "OK\r\n";

    cpe_sms_list_t list;
    HT_CHECK(cpe_sms_parse_cmgl_pdu(raw, &list));
    HT_CHECK_INT(list.count, 2);
    cpe_sms_merge_parts(&list);
    HT_CHECK_INT(list.count, 1);
    HT_CHECK_STR(list.items[0].body, "ABCDEF");
    HT_CHECK_INT(list.items[0].index_count, 2);
}

static void test_parse_cmgl_pdu_limit(void)
{
    static char raw[8192];
    size_t off = (size_t)snprintf(raw, sizeof(raw), "AT+CMGL=4\r\n");
    for (int i = 1; i <= CPE_SMS_MAX_MESSAGES + 1; i++) {
        off += (size_t)snprintf(raw + off, sizeof(raw) - off,
                                "+CMGL: %d,1,,27\r\n" PDU_7BIT_HELLO "\r\n", i);
    }
    snprintf(raw + off, sizeof(raw) - off, "OK\r\n");

    cpe_sms_list_t list;
    HT_CHECK(cpe_sms_parse_cmgl_pdu(raw, &list));
    HT_CHECK_INT(list.count, CPE_SMS_MAX_MESSAGES);
    HT_CHECK(list.truncated);
    HT_CHECK_INT(list.items[CPE_SMS_MAX_MESSAGES - 1].index, CPE_SMS_MAX_MESSAGES);
}

static void test_parse_cmgl_pdu_no_terminal(void)
{
    cpe_sms_list_t list;
    HT_CHECK(cpe_sms_parse_cmgl_pdu("+CMGL: 3,1,,27\r\n" PDU_7BIT_HELLO, &list));
    HT_CHECK_INT(list.count, 1);
    HT_CHECK_INT(list.items[0].index, 3);
    HT_CHECK_STR(list.items[0].body, "Hello!");

    HT_CHECK(!cpe_sms_parse_cmgl_pdu(NULL, &list));
}

void test_sms_pdu(void)
{
    test_ucs2_body();
    test_7bit_body();
    test_sender_and_time();
    test_concat_header();
    test_full_length_pdus();
    test_merge_decoded_parts();
    test_empty_body();
    test_malformed();
    test_unsupported();
    test_parse_cmgl_pdu();
    test_parse_cmgl_pdu_undecodable();
    test_parse_cmgl_pdu_merge();
    test_parse_cmgl_pdu_limit();
    test_parse_cmgl_pdu_no_terminal();
}
