/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for components/web_console/router_web/json_util.c. The escaping and the
 * depth guard sit between untrusted input (station names, modem strings, request
 * bodies) and the rest of the device, so a regression there is a security one.
 */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "http_internal.h"
#include "test_harness.h"

/* ------------------------------------------------------------------------- */
/* Growable buffer                                                           */
/* ------------------------------------------------------------------------- */

static void test_sbuf_basics(void)
{
    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 64);
    HT_CHECK(!sb.failed);
    HT_CHECK_INT(sb.len, 0);

    cpe_web_sbuf_append(&sb, "hello");
    cpe_web_sbuf_appendf(&sb, " %d", 42);
    HT_CHECK_STR(sb.data, "hello 42");
    HT_CHECK_INT(sb.len, 8);

    /* A NULL string is skipped instead of ending the response. */
    cpe_web_sbuf_append(&sb, NULL);
    HT_CHECK_INT(sb.len, 8);
    HT_CHECK_STR(sb.data, "hello 42");
    cpe_web_sbuf_free(&sb);
}

static void test_sbuf_grows(void)
{
    cpe_web_sbuf_t sb;
    /* Starts too small on purpose: every append has to grow it. */
    cpe_web_sbuf_init(&sb, 1);
    for (int i = 0; i < 20; i++) {
        cpe_web_sbuf_appendf(&sb, "%d;", i);
    }
    HT_CHECK(!sb.failed);
    HT_CHECK_STR(sb.data, "0;1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;");
    HT_CHECK_INT(sb.len, strlen(sb.data));
    cpe_web_sbuf_free(&sb);
}

static void test_sbuf_free_stops_appending(void)
{
    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 32);
    cpe_web_sbuf_free(&sb);
    HT_CHECK(sb.data == NULL);
    HT_CHECK(sb.failed);
    /* Everything after a free is a no-op, not a use after free. */
    cpe_web_sbuf_append(&sb, "x");
    cpe_web_sbuf_appendf(&sb, "y");
    cpe_web_sbuf_append_json_string(&sb, "z");
    HT_CHECK(sb.data == NULL);
}

/* ------------------------------------------------------------------------- */
/* JSON string escaping                                                      */
/* ------------------------------------------------------------------------- */

static void check_escaped(const char *in, const char *want)
{
    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 64);
    cpe_web_sbuf_append_json_string(&sb, in);
    HT_CHECK(!sb.failed);
    HT_CHECK_STR(sb.data, want);
    cpe_web_sbuf_free(&sb);
}

static void test_json_escaping(void)
{
    check_escaped("plain", "\"plain\"");
    check_escaped(NULL, "\"\"");
    check_escaped("", "\"\"");
    check_escaped("a\"b", "\"a\\\"b\"");
    check_escaped("back\\slash", "\"back\\\\slash\"");
    check_escaped("\n\t\r\b\f", "\"\\n\\t\\r\\b\\f\"");
    /* No short escape: control characters and DEL go out as \u00xx. */
    check_escaped("\x01", "\"\\u0001\"");
    check_escaped("\x1f", "\"\\u001f\"");
    check_escaped("\x7f", "\"\\u007f\"");
    /* UTF-8 is copied verbatim, byte for byte. */
    check_escaped("\xe4\xb8\xad", "\"\xe4\xb8\xad\"");
    /* Runs and escapes mixed: the run is copied in one piece. */
    check_escaped("ab\ncd\"ef", "\"ab\\ncd\\\"ef\"");
    check_escaped("\"\"", "\"\\\"\\\"\"");
    /* Everything that needs no escape in one long run. */
    check_escaped("abcdefghijklmnopqrstuvwxyz0123456789 ~!@#$%^&*()",
                  "\"abcdefghijklmnopqrstuvwxyz0123456789 ~!@#$%^&*()\"");
}

/* ------------------------------------------------------------------------- */
/* Depth guard                                                               */
/* ------------------------------------------------------------------------- */

static bool depth_ok(const char *body, int max_depth)
{
    return cpe_web_json_depth_ok(body, strlen(body), max_depth);
}

static void test_json_depth(void)
{
    HT_CHECK(depth_ok("{}", 1));
    HT_CHECK(!depth_ok("{\"a\":{}}", 1));
    HT_CHECK(depth_ok("{\"a\":{}}", 2));
    HT_CHECK(depth_ok("[1,[2,[3]]]", 3));
    HT_CHECK(!depth_ok("[1,[2,[3]]]", 2));
    /* Braces inside a string are content, not nesting. */
    HT_CHECK(depth_ok("{\"a\":\"{{{{\"}", 1));
    /* An escaped quote must not end the string early. */
    HT_CHECK(depth_ok("{\"a\":\"\\\"{\"}", 1));
    /* A closing brace without a matching opening one cannot go negative. */
    HT_CHECK(depth_ok("]}}}}", 1));
    /* An unterminated string swallows the rest: nothing is counted. */
    HT_CHECK(depth_ok("{\"a\":\"{{{{", 1));
    HT_CHECK(depth_ok("", 1));
}

/* ------------------------------------------------------------------------- */
/* Field getters                                                             */
/* ------------------------------------------------------------------------- */

static void test_get_string_unescaped(void)
{
    cJSON *root = cJSON_Parse("{\"a\":\"x\",\"empty\":\"\",\"n\":null,\"num\":5,"
                              "\"obj\":{},\"long\":\"0123456789\"}");
    HT_CHECK(root != NULL);
    char buf[8];

    HT_CHECK_INT(cpe_web_json_get_string_unescaped(root, "a", buf, sizeof(buf)), ESP_OK);
    HT_CHECK_STR(buf, "x");

    HT_CHECK_INT(cpe_web_json_get_string_unescaped(root, "empty", buf, sizeof(buf)), ESP_OK);
    HT_CHECK_STR(buf, "");

    /* Missing and null are the same thing for a caller. */
    HT_CHECK_INT(cpe_web_json_get_string_unescaped(root, "missing", buf, sizeof(buf)),
                 ESP_ERR_NOT_FOUND);
    HT_CHECK_INT(cpe_web_json_get_string_unescaped(root, "n", buf, sizeof(buf)),
                 ESP_ERR_NOT_FOUND);

    /* Present but unusable: 400, not silently ignored. */
    HT_CHECK_INT(cpe_web_json_get_string_unescaped(root, "num", buf, sizeof(buf)),
                 ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_web_json_get_string_unescaped(root, "obj", buf, sizeof(buf)),
                 ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_web_json_get_string_unescaped(root, "long", buf, sizeof(buf)),
                 ESP_ERR_INVALID_SIZE);

    /* Bad arguments, not bad input. */
    HT_CHECK_INT(cpe_web_json_get_string_unescaped(NULL, "a", buf, sizeof(buf)),
                 ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_web_json_get_string_unescaped(root, NULL, buf, sizeof(buf)),
                 ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_web_json_get_string_unescaped(root, "a", NULL, sizeof(buf)),
                 ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_web_json_get_string_unescaped(root, "a", buf, 0), ESP_ERR_INVALID_ARG);

    cJSON_Delete(root);
}

static void test_get_int_flexible(void)
{
    cJSON *root = cJSON_Parse("{\"n\":6,\"s\":\"7\",\"neg\":-3,\"bad\":\"x\","
                              "\"frac\":1.5,\"null\":null,\"big\":1e12,\"bool\":true}");
    HT_CHECK(root != NULL);
    int value = -1;
    bool present = true;

    HT_CHECK_INT(cpe_web_json_get_int_flexible(root, "n", &value, &present), ESP_OK);
    HT_CHECK(present);
    HT_CHECK_INT(value, 6);

    /* Legacy clients send numbers as strings. */
    HT_CHECK_INT(cpe_web_json_get_int_flexible(root, "s", &value, &present), ESP_OK);
    HT_CHECK(present);
    HT_CHECK_INT(value, 7);

    HT_CHECK_INT(cpe_web_json_get_int_flexible(root, "neg", &value, &present), ESP_OK);
    HT_CHECK_INT(value, -3);

    HT_CHECK_INT(cpe_web_json_get_int_flexible(root, "bad", &value, &present),
                 ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_web_json_get_int_flexible(root, "frac", &value, &present),
                 ESP_ERR_INVALID_ARG);
    /* Present but out of range for an int. */
    HT_CHECK_INT(cpe_web_json_get_int_flexible(root, "big", &value, &present),
                 ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_web_json_get_int_flexible(root, "bool", &value, &present),
                 ESP_ERR_INVALID_ARG);

    /* Absent: no error, but the caller must see present == false. */
    HT_CHECK_INT(cpe_web_json_get_int_flexible(root, "missing", &value, &present), ESP_OK);
    HT_CHECK(!present);
    HT_CHECK_INT(cpe_web_json_get_int_flexible(root, "null", &value, &present), ESP_OK);
    HT_CHECK(!present);

    cJSON_Delete(root);
}

static void test_get_bool_flexible(void)
{
    cJSON *root = cJSON_Parse("{\"t\":true,\"f\":false,\"ts\":\"true\",\"fs\":\"false\","
                              "\"one\":\"1\",\"zero\":\"0\",\"n1\":1,\"n0\":0,"
                              "\"n2\":2,\"yes\":\"yes\",\"null\":null}");
    HT_CHECK(root != NULL);
    bool value = false;
    bool present = false;

    HT_CHECK_INT(cpe_web_json_get_bool_flexible(root, "t", &value, &present), ESP_OK);
    HT_CHECK(value && present);
    HT_CHECK_INT(cpe_web_json_get_bool_flexible(root, "f", &value, &present), ESP_OK);
    HT_CHECK(!value && present);

    HT_CHECK_INT(cpe_web_json_get_bool_flexible(root, "ts", &value, &present), ESP_OK);
    HT_CHECK(value);
    HT_CHECK_INT(cpe_web_json_get_bool_flexible(root, "fs", &value, &present), ESP_OK);
    HT_CHECK(!value);
    HT_CHECK_INT(cpe_web_json_get_bool_flexible(root, "one", &value, &present), ESP_OK);
    HT_CHECK(value);
    HT_CHECK_INT(cpe_web_json_get_bool_flexible(root, "zero", &value, &present), ESP_OK);
    HT_CHECK(!value);

    /* 0/1 as a JSON number is accepted too. */
    HT_CHECK_INT(cpe_web_json_get_bool_flexible(root, "n1", &value, &present), ESP_OK);
    HT_CHECK(value);
    HT_CHECK_INT(cpe_web_json_get_bool_flexible(root, "n0", &value, &present), ESP_OK);
    HT_CHECK(!value);

    /* Anything else is rejected rather than coerced. */
    HT_CHECK_INT(cpe_web_json_get_bool_flexible(root, "n2", &value, &present),
                 ESP_ERR_INVALID_ARG);
    HT_CHECK_INT(cpe_web_json_get_bool_flexible(root, "yes", &value, &present),
                 ESP_ERR_INVALID_ARG);

    HT_CHECK_INT(cpe_web_json_get_bool_flexible(root, "missing", &value, &present), ESP_OK);
    HT_CHECK(!present);
    HT_CHECK_INT(cpe_web_json_get_bool_flexible(root, "null", &value, &present), ESP_OK);
    HT_CHECK(!present);

    cJSON_Delete(root);
}

void test_json_util(void)
{
    test_sbuf_basics();
    test_sbuf_grows();
    test_sbuf_free_stops_appending();
    test_json_escaping();
    test_json_depth();
    test_get_string_unescaped();
    test_get_int_flexible();
    test_get_bool_flexible();
}
