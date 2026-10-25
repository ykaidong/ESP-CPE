/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for components/web_console/router_web/station_names.c. The MAC parser
 * decides which station an API call applies to and the whitelist decides what a
 * user supplied name may contain, so both guard untrusted input.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "http_internal.h"
#include "test_harness.h"

/* ------------------------------------------------------------------------- */
/* MAC parsing                                                               */
/* ------------------------------------------------------------------------- */

static bool parse_ok(const char *str, uint8_t want[6])
{
    uint8_t mac[6] = { 0 };
    if (!cpe_web_mac_parse(str, mac)) {
        return false;
    }
    HT_CHECK_MEM(mac, want, sizeof(mac));
    return true;
}

static void test_mac_parse(void)
{
    uint8_t want[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };

    /* Both accepted forms, upper and lower case. */
    HT_CHECK(parse_ok("aa:bb:cc:dd:ee:ff", want));
    HT_CHECK(parse_ok("AA:BB:CC:DD:EE:FF", want));
    HT_CHECK(parse_ok("aAbBcCdDeEfF", want));
    HT_CHECK(parse_ok("AABBCCDDEEFF", want));

    uint8_t zeros[6] = { 0 };
    HT_CHECK(parse_ok("00:00:00:00:00:00", zeros));
    HT_CHECK(parse_ok("000000000000", zeros));

    /* Rejected: wrong separator, wrong length, non-hex, empty, NULL. */
    uint8_t mac[6] = { 0 };
    HT_CHECK(!cpe_web_mac_parse("aa-bb-cc-dd-ee-ff", mac));
    HT_CHECK(!cpe_web_mac_parse("aa:bb:cc:dd:ee-ff", mac));
    HT_CHECK(!cpe_web_mac_parse("aa:bb:cc:dd:ee", mac));
    HT_CHECK(!cpe_web_mac_parse("aa:bb:cc:dd:ee:ff:", mac));
    HT_CHECK(!cpe_web_mac_parse("aabbccddeeff00", mac));
    HT_CHECK(!cpe_web_mac_parse("gg:bb:cc:dd:ee:ff", mac));
    HT_CHECK(!cpe_web_mac_parse("a:bb:cc:dd:ee:ff", mac));
    HT_CHECK(!cpe_web_mac_parse("", mac));
    HT_CHECK(!cpe_web_mac_parse(NULL, mac));
    /* A UTF-8 character is 3 bytes, so this is not 12 characters long. */
    HT_CHECK(!cpe_web_mac_parse("\xe4\xb8\xad", mac));
}

static void test_mac_format_roundtrip(void)
{
    char hex[CPE_WEB_MAC_HEX_BUFSIZE];
    char colon[CPE_WEB_MAC_COLON_BUFSIZE];
    uint8_t mac[6] = { 0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f };

    cpe_web_mac_to_hex(mac, hex);
    HT_CHECK_STR(hex, "0a1b2c3d4e5f");
    cpe_web_mac_to_colon(mac, colon);
    HT_CHECK_STR(colon, "0a:1b:2c:3d:4e:5f");

    /* Both forms parse back to the same address. */
    uint8_t back[6] = { 0 };
    HT_CHECK(cpe_web_mac_parse(hex, back));
    HT_CHECK_MEM(back, mac, sizeof(mac));
    memset(back, 0, sizeof(back));
    HT_CHECK(cpe_web_mac_parse(colon, back));
    HT_CHECK_MEM(back, mac, sizeof(mac));
}

/* ------------------------------------------------------------------------- */
/* Nickname whitelist                                                        */
/* ------------------------------------------------------------------------- */

static void test_station_name_valid(void)
{
    HT_CHECK(cpe_web_station_name_valid("Phone"));
    HT_CHECK(cpe_web_station_name_valid("my-phone_2"));
    HT_CHECK(cpe_web_station_name_valid("a@b (2)"));
    HT_CHECK(cpe_web_station_name_valid("Living Room TV"));
    HT_CHECK(cpe_web_station_name_valid("192.168.4.2"));

    /* Rejected characters: anything that could break a page or a log line. */
    HT_CHECK(!cpe_web_station_name_valid("bad/name"));
    HT_CHECK(!cpe_web_station_name_valid("bad\\name"));
    HT_CHECK(!cpe_web_station_name_valid("<script>"));
    HT_CHECK(!cpe_web_station_name_valid("quote\""));
    HT_CHECK(!cpe_web_station_name_valid("new\nline"));
    HT_CHECK(!cpe_web_station_name_valid("\xe4\xb8\xad"));

    /* Empty, NULL, and a name of spaces only (nothing visible in it). */
    HT_CHECK(!cpe_web_station_name_valid(""));
    HT_CHECK(!cpe_web_station_name_valid(NULL));
    HT_CHECK(!cpe_web_station_name_valid("   "));
    /* Spaces around a visible character are fine. */
    HT_CHECK(cpe_web_station_name_valid("  x  "));

    /* Length limit: 32 characters. */
    char at_limit[CPE_WEB_STA_NAME_MAX_LEN + 1];
    memset(at_limit, 'a', sizeof(at_limit) - 1);
    at_limit[CPE_WEB_STA_NAME_MAX_LEN] = '\0';
    HT_CHECK_INT((long long)strlen(at_limit), CPE_WEB_STA_NAME_MAX_LEN);
    HT_CHECK(cpe_web_station_name_valid(at_limit));

    char over_limit[CPE_WEB_STA_NAME_MAX_LEN + 2];
    memset(over_limit, 'a', sizeof(over_limit) - 1);
    over_limit[CPE_WEB_STA_NAME_MAX_LEN + 1] = '\0';
    HT_CHECK(!cpe_web_station_name_valid(over_limit));
}

void test_station_names(void)
{
    test_mac_parse();
    test_mac_format_roundtrip();
    test_station_name_valid();
}
