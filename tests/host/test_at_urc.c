/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the AT line classifier and URC prefix matcher.
 */

#include <stddef.h>

#include "cpe_at_urc_match.h"
#include "test_harness.h"

static const char *const s_cmti[] = { "+CMTI:" };
static const char *const s_two[] = { "+CMTI:", "+QIND:" };
static const char *const s_cme[] = { "+CME" };
static const char *const s_empty[] = { "" };

static void test_line_kind(void)
{
    HT_CHECK_INT(cpe_at_urc_line_kind("OK", 2), CPE_AT_LINE_FINAL_OK);
    HT_CHECK_INT(cpe_at_urc_line_kind("ERROR", 5), CPE_AT_LINE_FINAL_ERROR);
    HT_CHECK_INT(cpe_at_urc_line_kind("+CME ERROR: 3", 13), CPE_AT_LINE_FINAL_ERROR);
    HT_CHECK_INT(cpe_at_urc_line_kind("+CMS ERROR: 500", 15), CPE_AT_LINE_FINAL_ERROR);
    HT_CHECK_INT(cpe_at_urc_line_kind("OKAY", 4), CPE_AT_LINE_OTHER);
    HT_CHECK_INT(cpe_at_urc_line_kind("+CMTI: \"ME\",1", 13), CPE_AT_LINE_OTHER);
    HT_CHECK_INT(cpe_at_urc_line_kind("", 0), CPE_AT_LINE_OTHER);
    HT_CHECK_INT(cpe_at_urc_line_kind(NULL, 0), CPE_AT_LINE_OTHER);
}

static void test_line_matches(void)
{
    HT_CHECK(cpe_at_urc_line_matches("+CMTI: \"ME\",5", 13, s_cmti, 1));
    HT_CHECK(cpe_at_urc_line_matches("+CMTI:", 6, s_cmti, 1));
    HT_CHECK(!cpe_at_urc_line_matches("+CMTI", 5, s_cmti, 1));
    HT_CHECK(!cpe_at_urc_line_matches("+CMTIX: 1", 9, s_cmti, 1));
    HT_CHECK(!cpe_at_urc_line_matches("OK", 2, s_cmti, 1));
    /* 结果行永远不是 URC，即使前缀能匹配到它。 */
    HT_CHECK(!cpe_at_urc_line_matches("+CME ERROR: 1", 13, s_cme, 1));
    HT_CHECK(cpe_at_urc_line_matches("+QIND: \"x\"", 10, s_two, 2));
    HT_CHECK(!cpe_at_urc_line_matches("+QIND: \"x\"", 10, NULL, 0));
    HT_CHECK(!cpe_at_urc_line_matches("+QIND: \"x\"", 10, s_two, 0));
    HT_CHECK(!cpe_at_urc_line_matches("+X", 2, s_empty, 1));
    HT_CHECK(!cpe_at_urc_line_matches(NULL, 0, s_cmti, 1));
}

static void test_partial_matches(void)
{
    HT_CHECK(cpe_at_urc_partial_matches("+", 1, s_cmti, 1));
    HT_CHECK(cpe_at_urc_partial_matches("+CM", 3, s_cmti, 1));
    HT_CHECK(cpe_at_urc_partial_matches("+CMTI", 5, s_cmti, 1));
    HT_CHECK(cpe_at_urc_partial_matches("+CMTI:", 6, s_cmti, 1));
    HT_CHECK(cpe_at_urc_partial_matches("+CMTI: \"ME\"", 10, s_cmti, 1));
    HT_CHECK(!cpe_at_urc_partial_matches("+CMTX", 5, s_cmti, 1));
    HT_CHECK(!cpe_at_urc_partial_matches("OK", 2, s_cmti, 1));
    HT_CHECK(!cpe_at_urc_partial_matches("", 0, s_cmti, 1));
    HT_CHECK(!cpe_at_urc_partial_matches(NULL, 0, s_cmti, 1));
    HT_CHECK(!cpe_at_urc_partial_matches("+", 1, s_empty, 1));
}

void test_at_urc(void)
{
    test_line_kind();
    test_line_matches();
    test_partial_matches();
}
