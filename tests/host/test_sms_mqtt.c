/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for components/cpe_sms_mqtt/cpe_sms_mqtt_proto.c: number and topic
 * validation, the configuration gate, downlink command parsing, the wire
 * payloads and the duplicate ring.
 */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "cpe_sms_mqtt_proto.h"
#include "test_harness.h"

static void check_number(const char *in, bool want_ok, const char *want_out)
{
    char out[CPE_SMS_MQTT_NUMBER_MAX + 1];
    bool ok = cpe_sms_mqtt_normalize_number(in, out);
    HT_CHECK_INT(ok, want_ok);
    if (ok && want_out != NULL) {
        HT_CHECK_STR(out, want_out);
    }
}

static void test_numbers(void)
{
    check_number("+8613800138000", true, "+8613800138000");
    check_number(" 138 0013 8000 ", true, "13800138000");
    check_number("(010)-1234-5678", true, "01012345678");
    check_number("+86 138-0013-8000", true, "+8613800138000");
    /* 3 is the shortest plausible number and 20 digits the longest ITU E.164. */
    check_number("123", true, "123");
    check_number("12345678901234567890", true, "12345678901234567890");
    check_number("12", false, NULL);
    check_number("123456789012345678901", false, NULL);
    /* A '+' anywhere but the front, or two of them, is not a number we dial. */
    check_number("1+234", false, NULL);
    check_number("++123", false, NULL);
    check_number("123a456", false, NULL);
    check_number("", false, NULL);
    check_number("+", false, NULL);
}

static void test_topics(void)
{
    HT_CHECK(cpe_sms_mqtt_topic_valid("cpe/sms/in"));
    HT_CHECK(cpe_sms_mqtt_topic_valid("a"));
    HT_CHECK(cpe_sms_mqtt_topic_valid("网络/短信"));
    HT_CHECK(!cpe_sms_mqtt_topic_valid(""));
    HT_CHECK(!cpe_sms_mqtt_topic_valid("cpe/+/in"));
    HT_CHECK(!cpe_sms_mqtt_topic_valid("cpe/#"));
    HT_CHECK(!cpe_sms_mqtt_topic_valid("cpe sms"));
    HT_CHECK(!cpe_sms_mqtt_topic_valid("cpe\nsms"));

    char long_topic[CPE_SMS_MQTT_TOPIC_MAX + 2];
    memset(long_topic, 'a', sizeof(long_topic));
    long_topic[CPE_SMS_MQTT_TOPIC_MAX] = '\0';
    HT_CHECK(cpe_sms_mqtt_topic_valid(long_topic));
    long_topic[CPE_SMS_MQTT_TOPIC_MAX] = 'a';
    long_topic[CPE_SMS_MQTT_TOPIC_MAX + 1] = '\0';
    HT_CHECK(!cpe_sms_mqtt_topic_valid(long_topic));
}

static void test_hosts(void)
{
    HT_CHECK(cpe_sms_mqtt_host_valid("mqtt.example.com"));
    HT_CHECK(cpe_sms_mqtt_host_valid("192.168.1.10"));
    HT_CHECK(cpe_sms_mqtt_host_valid("my-broker_1"));
    HT_CHECK(!cpe_sms_mqtt_host_valid("mqtts://example.com"));
    HT_CHECK(!cpe_sms_mqtt_host_valid("example.com:8883"));
    HT_CHECK(!cpe_sms_mqtt_host_valid("::1"));
    HT_CHECK(!cpe_sms_mqtt_host_valid(""));
    HT_CHECK(!cpe_sms_mqtt_host_valid("---"));
}

static void test_pem(void)
{
    static const char cert[] =
        "-----BEGIN CERTIFICATE-----\r\n"
        "MIIBszCCAVmgAwIBAgIUQ2VydGlmaWNhdGVEYXRhMDAwMDAwMDAwMDAwMDAwMDAw\r\n"
        "-----END CERTIFICATE-----\r\n";

    HT_CHECK(cpe_sms_mqtt_pem_valid(cert));
    HT_CHECK(!cpe_sms_mqtt_pem_valid(NULL));
    HT_CHECK(!cpe_sms_mqtt_pem_valid(""));

    /* Two certificates: a root plus an intermediate stays valid. */
    char chain[512];
    snprintf(chain, sizeof(chain), "%s%s", cert, cert);
    HT_CHECK(cpe_sms_mqtt_pem_valid(chain));

    /* A private key is never accepted as a trust anchor. */
    HT_CHECK(!cpe_sms_mqtt_pem_valid(
        "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n"));
    HT_CHECK(!cpe_sms_mqtt_pem_valid(
        "-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----\n"
        "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n"));

    /* Truncated documents, stray binary bytes and reversed block order fail. */
    HT_CHECK(!cpe_sms_mqtt_pem_valid("-----BEGIN CERTIFICATE-----\nAAAA\n"));
    HT_CHECK(!cpe_sms_mqtt_pem_valid(
        "-----END CERTIFICATE-----\n-----BEGIN CERTIFICATE-----\n"));
    HT_CHECK(!cpe_sms_mqtt_pem_valid(
        "-----BEGIN CERTIFICATE-----\nAA\x01" "AA\n-----END CERTIFICATE-----\n"));
    HT_CHECK(!cpe_sms_mqtt_pem_valid("not a certificate\n"));

    char oversized[CPE_SMS_MQTT_PEM_MAX + 2];
    memset(oversized, 'A', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    HT_CHECK(!cpe_sms_mqtt_pem_valid(oversized));
}

static void test_config_check(void)
{
    cpe_sms_mqtt_config_t cfg;
    const char *err = NULL;

    /* A disabled block is always savable, even half filled. */
    cpe_sms_mqtt_config_defaults(&cfg);
    HT_CHECK(cpe_sms_mqtt_config_check(&cfg, &err));

    cfg.enabled = true;
    HT_CHECK(!cpe_sms_mqtt_config_check(&cfg, &err));
    HT_CHECK_STR(err, "invalid_mqtt_host");

    snprintf(cfg.host, sizeof(cfg.host), "mqtt.example.com");
    cfg.port = 8883;
    HT_CHECK(!cpe_sms_mqtt_config_check(&cfg, &err));
    HT_CHECK_STR(err, "invalid_mqtt_topic");

    snprintf(cfg.topic_up, sizeof(cfg.topic_up), "cpe/sms/up");
    snprintf(cfg.topic_down, sizeof(cfg.topic_down), "cpe/sms/up");
    HT_CHECK(!cpe_sms_mqtt_config_check(&cfg, &err));
    HT_CHECK_STR(err, "invalid_mqtt_topic");

    snprintf(cfg.topic_down, sizeof(cfg.topic_down), "cpe/sms/down");
    HT_CHECK(cpe_sms_mqtt_config_check(&cfg, &err));

    cfg.port = 0;
    HT_CHECK(!cpe_sms_mqtt_config_check(&cfg, &err));
    HT_CHECK_STR(err, "invalid_mqtt_port");

    cfg.port = 8883;
    HT_CHECK(cpe_sms_mqtt_config_check(&cfg, &err));

    /* Credentials: a password without a username is never valid, and control
     * characters are refused in both, even while forwarding is disabled. */
    snprintf(cfg.password, sizeof(cfg.password), "secret");
    HT_CHECK(!cpe_sms_mqtt_config_check(&cfg, &err));
    HT_CHECK_STR(err, "invalid_mqtt_auth");

    snprintf(cfg.username, sizeof(cfg.username), "cpe");
    HT_CHECK(cpe_sms_mqtt_config_check(&cfg, &err));

    snprintf(cfg.password, sizeof(cfg.password), "pa\x01" "ss");
    HT_CHECK(!cpe_sms_mqtt_config_check(&cfg, &err));
    HT_CHECK_STR(err, "invalid_mqtt_auth");

    snprintf(cfg.password, sizeof(cfg.password), "secret");
    cfg.enabled = false;
    cfg.username[0] = '\0';
    HT_CHECK(!cpe_sms_mqtt_config_check(&cfg, &err));

    cfg.password[0] = '\0';
    HT_CHECK(cpe_sms_mqtt_config_check(&cfg, &err));
}

static void parse_cmd(const char *json, bool want_ok, const char *want_err,
                      cpe_sms_mqtt_cmd_t *cmd)
{
    const char *err = NULL;
    bool ok = cpe_sms_mqtt_cmd_parse(json, 0, cmd, &err);
    HT_CHECK_INT(ok, want_ok);
    if (!ok && want_err != NULL) {
        HT_CHECK_STR(err, want_err);
    }
}

static void test_cmd_parse(void)
{
    cpe_sms_mqtt_cmd_t cmd;

    parse_cmd("{\"id\":\"abc-1\",\"to\":\"+86 138-0013-8000\","
              "\"text\":\"你好\\nworld\"}", true, NULL, &cmd);
    HT_CHECK_STR(cmd.id, "abc-1");
    HT_CHECK_STR(cmd.to, "+8613800138000");
    HT_CHECK_STR(cmd.text, "你好\nworld");

    parse_cmd("{\"id\":\"x\",\"to\":\"13800138000\",\"text\":\"hi\"}", true, NULL, &cmd);
    HT_CHECK_STR(cmd.to, "13800138000");

    parse_cmd("{\"to\":\"13800138000\",\"text\":\"hi\"}", false, "invalid_command", &cmd);
    parse_cmd("{\"id\":\"\",\"to\":\"13800138000\",\"text\":\"hi\"}", false,
              "invalid_command", &cmd);
    parse_cmd("{\"id\":\"x\",\"to\":\"nope\",\"text\":\"hi\"}", false,
              "sms_bad_number", &cmd);
    parse_cmd("{\"id\":\"x\",\"to\":\"13800138000\"}", false, "invalid_command", &cmd);
    parse_cmd("{\"id\":\"x\",\"to\":\"13800138000\",\"text\":\"\"}", false,
              "invalid_command", &cmd);
    parse_cmd("not json", false, "invalid_command", &cmd);

    char long_text[CPE_SMS_MQTT_TEXT_MAX + 64];
    size_t o = (size_t)snprintf(long_text, sizeof(long_text),
                                "{\"id\":\"x\",\"to\":\"13800138000\",\"text\":\"");
    memset(long_text + o, 'a', CPE_SMS_MQTT_TEXT_MAX + 1);
    o += CPE_SMS_MQTT_TEXT_MAX + 1;
    long_text[o++] = '"';
    long_text[o++] = '}';
    long_text[o] = '\0';
    parse_cmd(long_text, false, "sms_text_too_long", &cmd);

    char id[CPE_SMS_MQTT_ID_MAX + 64];
    o = (size_t)snprintf(id, sizeof(id), "{\"id\":\"");
    memset(id + o, 'i', CPE_SMS_MQTT_ID_MAX + 1);
    o += CPE_SMS_MQTT_ID_MAX + 1;
    snprintf(id + o, sizeof(id) - o, "\",\"to\":\"13800138000\",\"text\":\"hi\"}");
    parse_cmd(id, false, "invalid_command", &cmd);
}

static void test_build_sms(void)
{
    char buf[1024];
    size_t len = cpe_sms_mqtt_build_sms(buf, sizeof(buf), "+8613800138000",
                                        "he said \"hi\"\nok", "2026-10-27 10:00:00",
                                        2, 0x1234ABCDu);
    HT_CHECK(len > 0);
    HT_CHECK_INT(len, strlen(buf));

    cJSON *root = cJSON_Parse(buf);
    HT_CHECK(root != NULL);
    if (root != NULL) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        const cJSON *from = cJSON_GetObjectItemCaseSensitive(root, "from");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        const cJSON *time = cJSON_GetObjectItemCaseSensitive(root, "time");
        const cJSON *parts = cJSON_GetObjectItemCaseSensitive(root, "parts");
        HT_CHECK(cJSON_IsString(type));
        HT_CHECK_STR(type->valuestring, "sms");
        HT_CHECK_STR(id->valuestring, "1234abcd");
        HT_CHECK_STR(from->valuestring, "+8613800138000");
        HT_CHECK_STR(text->valuestring, "he said \"hi\"\nok");
        HT_CHECK_STR(time->valuestring, "2026-10-27 10:00:00");
        HT_CHECK_INT(parts->valueint, 2);
        cJSON_Delete(root);
    }

    /* A too small buffer reports 0 instead of writing past the end. */
    char small[8];
    HT_CHECK_INT(cpe_sms_mqtt_build_sms(small, sizeof(small), "1", "2", "3", 1, 0), 0);

    /* Missing fields become empty strings rather than "null". */
    len = cpe_sms_mqtt_build_sms(buf, sizeof(buf), NULL, NULL, NULL, 0, 0);
    HT_CHECK(len > 0);
    root = cJSON_Parse(buf);
    HT_CHECK(root != NULL);
    if (root != NULL) {
        HT_CHECK_STR(cJSON_GetObjectItemCaseSensitive(root, "from")->valuestring, "");
        HT_CHECK_STR(cJSON_GetObjectItemCaseSensitive(root, "text")->valuestring, "");
        HT_CHECK_STR(cJSON_GetObjectItemCaseSensitive(root, "time")->valuestring, "");
        HT_CHECK_INT(cJSON_GetObjectItemCaseSensitive(root, "parts")->valueint, 1);
        cJSON_Delete(root);
    }
}

static void test_build_ack(void)
{
    char buf[512];
    size_t len = cpe_sms_mqtt_build_ack(buf, sizeof(buf), "cmd-1", true, "ignored");
    HT_CHECK(len > 0);
    cJSON *root = cJSON_Parse(buf);
    HT_CHECK(root != NULL);
    if (root != NULL) {
        HT_CHECK_STR(cJSON_GetObjectItemCaseSensitive(root, "type")->valuestring, "ack");
        HT_CHECK_STR(cJSON_GetObjectItemCaseSensitive(root, "id")->valuestring, "cmd-1");
        HT_CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok")));
        HT_CHECK(cJSON_GetObjectItemCaseSensitive(root, "error") == NULL);
        cJSON_Delete(root);
    }

    len = cpe_sms_mqtt_build_ack(buf, sizeof(buf), "cmd-2", false, "sms_send_failed");
    HT_CHECK(len > 0);
    root = cJSON_Parse(buf);
    HT_CHECK(root != NULL);
    if (root != NULL) {
        HT_CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "ok")));
        HT_CHECK_STR(cJSON_GetObjectItemCaseSensitive(root, "error")->valuestring,
                     "sms_send_failed");
        cJSON_Delete(root);
    }

    char small[8];
    HT_CHECK_INT(cpe_sms_mqtt_build_ack(small, sizeof(small), "cmd-3", true, NULL), 0);
}

static void test_hash_and_ring(void)
{
    uint32_t a = cpe_sms_mqtt_msg_hash("+8613800138000", "2026-10-27 10:00:00", "hello");
    uint32_t b = cpe_sms_mqtt_msg_hash("+8613800138000", "2026-10-27 10:00:00", "hello");
    uint32_t c = cpe_sms_mqtt_msg_hash("+8613800138000", "2026-10-27 10:00:00", "hellp");
    HT_CHECK_INT(a, b);
    HT_CHECK(a != c);

    /* Lengths are mixed in, so shifting a byte between fields changes the hash. */
    HT_CHECK(cpe_sms_mqtt_msg_hash("ab", "", "c") != cpe_sms_mqtt_msg_hash("a", "bc", ""));
    HT_CHECK(cpe_sms_mqtt_msg_hash(NULL, NULL, NULL) ==
             cpe_sms_mqtt_msg_hash("", "", ""));

    cpe_sms_mqtt_ring_t ring;
    cpe_sms_mqtt_ring_init(&ring);
    HT_CHECK_INT(ring.count, 0);
    HT_CHECK(!cpe_sms_mqtt_ring_contains(&ring, a));

    cpe_sms_mqtt_ring_add(&ring, a);
    HT_CHECK_INT(ring.count, 1);
    HT_CHECK(cpe_sms_mqtt_ring_contains(&ring, a));

    /* Adding a duplicate must not consume a second slot. */
    cpe_sms_mqtt_ring_add(&ring, a);
    HT_CHECK_INT(ring.count, 1);

    for (uint32_t i = 1; i <= CPE_SMS_MQTT_RING_SIZE; i++) {
        cpe_sms_mqtt_ring_add(&ring, 1000u + i);
    }
    HT_CHECK_INT(ring.count, CPE_SMS_MQTT_RING_SIZE);
    /* The oldest entry is now the one written first after the initial add. */
    HT_CHECK(!cpe_sms_mqtt_ring_contains(&ring, a));
    HT_CHECK(cpe_sms_mqtt_ring_contains(&ring, 1001u));
    HT_CHECK(cpe_sms_mqtt_ring_contains(&ring, 1000u + CPE_SMS_MQTT_RING_SIZE));

    /* A ring restored from NVS is a plain copy, so the round trip must keep it. */
    cpe_sms_mqtt_ring_t restored;
    memcpy(&restored, &ring, sizeof(restored));
    HT_CHECK(cpe_sms_mqtt_ring_contains(&restored, 1001u));
    HT_CHECK(!cpe_sms_mqtt_ring_contains(&restored, a));
}

void test_sms_mqtt(void)
{
    test_numbers();
    test_topics();
    test_hosts();
    test_pem();
    test_config_check();
    test_cmd_parse();
    test_build_sms();
    test_build_ack();
    test_hash_and_ring();
}
