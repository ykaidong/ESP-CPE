/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure helpers of the SMS forwarding component; see cpe_sms_mqtt_proto.h.
 * Keeping the JSON payloads here also keeps the MQTT task free of cJSON work
 * that the host tests can cover.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "cpe_sms_mqtt_proto.h"


void cpe_sms_mqtt_config_defaults(cpe_sms_mqtt_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = 8883;
}

bool cpe_sms_mqtt_host_valid(const char *host)
{
    if (host == NULL) {
        return false;
    }
    size_t len = strlen(host);
    if (len == 0 || len > CPE_SMS_MQTT_HOST_MAX) {
        return false;
    }
    /* IPv6 literals would need bracketed URIs; the page asks for a hostname or
     * an IPv4 address, and rejecting ':' here keeps the URI safe to build. */
    bool has_alnum = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)host[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            has_alnum = true;
        } else if (c != '.' && c != '-' && c != '_') {
            return false;
        }
    }
    return has_alnum;
}

bool cpe_sms_mqtt_topic_valid(const char *topic)
{
    if (topic == NULL) {
        return false;
    }
    size_t len = strlen(topic);
    if (len == 0 || len > CPE_SMS_MQTT_TOPIC_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)topic[i];
        /* Spaces and control bytes are legal in MQTT but only produce support
         * questions; wildcards are refused because the downlink subscription
         * must name one topic. Bytes >= 0x80 are UTF-8 and allowed. */
        if (c < 0x21 || c == 0x7f || c == '+' || c == '#') {
            return false;
        }
    }
    return true;
}

bool cpe_sms_mqtt_pem_valid(const char *pem)
{
    if (pem == NULL) {
        return false;
    }
    size_t len = strlen(pem);
    if (len == 0 || len > CPE_SMS_MQTT_PEM_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)pem[i];
        if (c == '\r' || c == '\n' || c == '\t') {
            continue;
        }
        if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    /* The user can only install a trust anchor. A private key here would be
     * silently ignored at best; refusing the document is clearer. */
    if (strstr(pem, "PRIVATE KEY") != NULL) {
        return false;
    }
    static const char begin[] = "-----BEGIN CERTIFICATE-----";
    static const char end[] = "-----END CERTIFICATE-----";
    const char *start = strstr(pem, begin);
    if (start == NULL) {
        return false;
    }
    const char *stop = strstr(start + sizeof(begin) - 1, end);
    return stop != NULL;
}


bool cpe_sms_mqtt_normalize_number(const char *in,
                                   char out[CPE_SMS_MQTT_NUMBER_MAX + 1])
{
    if (in == NULL || out == NULL) {
        return false;
    }
    const char *p = in;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    bool plus = (*p == '+');
    if (plus) {
        p++;
    }

    char digits[CPE_SMS_MQTT_NUMBER_MAX];
    size_t n = 0;
    for (; *p != '\0'; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '-' || c == '(' || c == ')') {
            continue;
        }
        if (c < '0' || c > '9') {
            return false;
        }
        if (n >= sizeof(digits)) {
            return false;
        }
        digits[n++] = c;
    }
    if (n < 3 || n > 20) {
        return false;
    }

    size_t o = 0;
    if (plus) {
        out[o++] = '+';
    }
    memcpy(out + o, digits, n);
    out[o + n] = '\0';
    return true;
}

static bool credential_text_ok(const char *text)
{
    for (const char *p = text; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

bool cpe_sms_mqtt_config_check(const cpe_sms_mqtt_config_t *cfg, const char **err)
{
    const char *e = NULL;
    if (cfg == NULL) {
        e = "invalid_mqtt_host";
        goto fail;
    }
    /* A password without a username is never meaningful: the broker would be
     * asked to authenticate anonymously with a secret attached. */
    if (cfg->password[0] != '\0' && cfg->username[0] == '\0') {
        e = "invalid_mqtt_auth";
        goto fail;
    }
    if (!credential_text_ok(cfg->username) || !credential_text_ok(cfg->password)) {
        e = "invalid_mqtt_auth";
        goto fail;
    }
    /* A disabled configuration keeps whatever the user typed last, so it must
     * always be savable; only an enabled one has to be usable. */
    if (!cfg->enabled) {
        return true;
    }
    if (!cpe_sms_mqtt_host_valid(cfg->host)) {
        e = "invalid_mqtt_host";
        goto fail;
    }
    if (cfg->port == 0) {
        e = "invalid_mqtt_port";
        goto fail;
    }
    if (!cpe_sms_mqtt_topic_valid(cfg->topic_up) ||
        !cpe_sms_mqtt_topic_valid(cfg->topic_down) ||
        strcmp(cfg->topic_up, cfg->topic_down) == 0) {
        e = "invalid_mqtt_topic";
        goto fail;
    }
    return true;

fail:
    if (err != NULL) {
        *err = e;
    }
    return false;
}

bool cpe_sms_mqtt_cmd_parse(const char *json, size_t len, cpe_sms_mqtt_cmd_t *out,
                            const char **err)
{
    const char *e = "invalid_command";
    if (json == NULL || out == NULL) {
        goto fail;
    }
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_ParseWithLength(json, len ? len : strlen(json));
    if (root == NULL) {
        goto fail;
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *to = cJSON_GetObjectItemCaseSensitive(root, "to");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    bool ok = false;
    if (cJSON_IsString(id) && id->valuestring != NULL &&
        id->valuestring[0] != '\0' && strlen(id->valuestring) <= CPE_SMS_MQTT_ID_MAX) {
        char number[CPE_SMS_MQTT_NUMBER_MAX + 1];
        if (!cJSON_IsString(to) || to->valuestring == NULL ||
            !cpe_sms_mqtt_normalize_number(to->valuestring, number)) {
            e = "sms_bad_number";
        } else if (!cJSON_IsString(text) || text->valuestring == NULL ||
                   text->valuestring[0] == '\0') {
            e = "invalid_command";
        } else if (strlen(text->valuestring) > CPE_SMS_MQTT_TEXT_MAX) {
            e = "sms_text_too_long";
        } else {
            memcpy(out->id, id->valuestring, strlen(id->valuestring) + 1);
            memcpy(out->to, number, sizeof(number));
            memcpy(out->text, text->valuestring, strlen(text->valuestring) + 1);
            ok = true;
        }
    }
    cJSON_Delete(root);
    if (!ok) {
        memset(out, 0, sizeof(*out));
        goto fail;
    }
    return true;

fail:
    if (err != NULL) {
        *err = e;
    }
    return false;
}

static size_t copy_payload(char *buf, size_t cap, cJSON *root)
{
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == NULL) {
        return 0;
    }
    size_t len = strlen(printed);
    if (cap == 0 || len + 1 > cap) {
        free(printed);
        return 0;
    }
    memcpy(buf, printed, len + 1);
    free(printed);
    return len;
}

size_t cpe_sms_mqtt_build_sms(char *buf, size_t cap, const char *from,
                              const char *text, const char *time, uint8_t parts,
                              uint32_t id_hash)
{
    if (buf == NULL) {
        return 0;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return 0;
    }
    char id[9];
    snprintf(id, sizeof(id), "%08x", (unsigned)id_hash);
    cJSON_AddStringToObject(root, "type", "sms");
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "from", from != NULL ? from : "");
    cJSON_AddStringToObject(root, "text", text != NULL ? text : "");
    cJSON_AddStringToObject(root, "time", time != NULL ? time : "");
    cJSON_AddNumberToObject(root, "parts", parts > 0 ? parts : 1);
    return copy_payload(buf, cap, root);
}

size_t cpe_sms_mqtt_build_ack(char *buf, size_t cap, const char *id, bool ok,
                              const char *error)
{
    if (buf == NULL || id == NULL) {
        return 0;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return 0;
    }
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", ok);
    if (!ok && error != NULL) {
        cJSON_AddStringToObject(root, "error", error);
    }
    return copy_payload(buf, cap, root);
}

static uint32_t hash_bytes(uint32_t h, const void *data, size_t len)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t hash_field(uint32_t h, const char *field)
{
    const char *value = field != NULL ? field : "";
    uint32_t len = (uint32_t)strlen(value);
    h = hash_bytes(h, &len, sizeof(len));
    return hash_bytes(h, value, len);
}

uint32_t cpe_sms_mqtt_msg_hash(const char *from, const char *time, const char *text)
{
    uint32_t h = 2166136261u;
    h = hash_field(h, from);
    h = hash_field(h, time);
    h = hash_field(h, text);
    return h;
}

void cpe_sms_mqtt_ring_init(cpe_sms_mqtt_ring_t *ring)
{
    if (ring != NULL) {
        memset(ring, 0, sizeof(*ring));
    }
}

bool cpe_sms_mqtt_ring_contains(const cpe_sms_mqtt_ring_t *ring, uint32_t hash)
{
    if (ring == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < ring->count && i < CPE_SMS_MQTT_RING_SIZE; i++) {
        if (ring->hash[i] == hash) {
            return true;
        }
    }
    return false;
}

void cpe_sms_mqtt_ring_add(cpe_sms_mqtt_ring_t *ring, uint32_t hash)
{
    if (ring == NULL || cpe_sms_mqtt_ring_contains(ring, hash)) {
        return;
    }
    if (ring->count >= CPE_SMS_MQTT_RING_SIZE) {
        ring->hash[ring->next] = hash;
        ring->next = (uint8_t)((ring->next + 1) % CPE_SMS_MQTT_RING_SIZE);
        return;
    }
    ring->hash[ring->count++] = hash;
}
