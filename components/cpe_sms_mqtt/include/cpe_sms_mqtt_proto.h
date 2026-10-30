/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure logic of SMS forwarding: configuration shapes, number and topic
 * validation, the wire payloads and the duplicate filter. Nothing here touches
 * ESP-IDF, NVS or the network, so tests/host can exercise it in a normal
 * process. See cpe_sms_mqtt.h for the runtime API.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPE_SMS_MQTT_HOST_MAX       64
#define CPE_SMS_MQTT_TOPIC_MAX      128
#define CPE_SMS_MQTT_USER_MAX       64
#define CPE_SMS_MQTT_PASS_MAX       128
/* One optional '+' plus 20 digits. */
#define CPE_SMS_MQTT_NUMBER_MAX     21
#define CPE_SMS_MQTT_TEXT_MAX       512
#define CPE_SMS_MQTT_ID_MAX         64
#define CPE_SMS_MQTT_RING_SIZE      16
/* Room for a root CA plus an intermediate. The whole PEM is kept in NVS. */
#define CPE_SMS_MQTT_PEM_MAX        3900

typedef struct {
    bool enabled;
    bool allow_remote_send;      /* execute downlink send commands */
    char host[CPE_SMS_MQTT_HOST_MAX + 1];
    uint16_t port;
    /* Broker credentials; an empty username means anonymous access and forces
     * the password empty as well. */
    char username[CPE_SMS_MQTT_USER_MAX + 1];
    char password[CPE_SMS_MQTT_PASS_MAX + 1];
    char topic_up[CPE_SMS_MQTT_TOPIC_MAX + 1];
    char topic_down[CPE_SMS_MQTT_TOPIC_MAX + 1];
} cpe_sms_mqtt_config_t;

typedef struct {
    char id[CPE_SMS_MQTT_ID_MAX + 1];
    char to[CPE_SMS_MQTT_NUMBER_MAX + 1];
    char text[CPE_SMS_MQTT_TEXT_MAX + 1];
} cpe_sms_mqtt_cmd_t;

/* Recent message hashes, persisted so a reboot does not re-push the inbox.
 * An entry is only a 32-bit FNV hash; the collision risk over 16 entries is
 * negligible and a false positive can only skip one message. */
typedef struct {
    uint32_t hash[CPE_SMS_MQTT_RING_SIZE];
    uint8_t next;
    uint8_t count;
} cpe_sms_mqtt_ring_t;

/** Fill `cfg` with the factory defaults (disabled, port 8883). */
void cpe_sms_mqtt_config_defaults(cpe_sms_mqtt_config_t *cfg);

/** Validate the whole configuration. Returns false and sets *err (when non-NULL)
 *  to a static error code: "invalid_mqtt_host", "invalid_mqtt_port",
 *  "invalid_mqtt_topic" or "invalid_mqtt_auth".
 *  A disabled configuration skips the fields needed to connect (the user must
 *  be able to clear it), but a password without a username is always refused. */
bool cpe_sms_mqtt_config_check(const cpe_sms_mqtt_config_t *cfg, const char **err);

/** Hostname or IPv4 literal; no scheme, port, path or whitespace. */
bool cpe_sms_mqtt_host_valid(const char *host);

/** MQTT topic name: 1..128 bytes, printable, no '+' or '#'. */
bool cpe_sms_mqtt_topic_valid(const char *topic);

/** True when `pem` is a printable PEM document of at most CPE_SMS_MQTT_PEM_MAX
 *  bytes that contains a complete CERTIFICATE block. Private keys and other
 *  block types are refused: only a trust anchor belongs here. */
bool cpe_sms_mqtt_pem_valid(const char *pem);

/** Strip spaces, '-', '(' and ')' and keep one optional leading '+'. The result
 *  must have 3..20 digits. Returns false when the number is unusable. */
bool cpe_sms_mqtt_normalize_number(const char *in,
                                   char out[CPE_SMS_MQTT_NUMBER_MAX + 1]);

/** Parse a downlink command `{"id":"..","to":"..","text":".."}`. All three
 *  fields are required: `id` is the caller's correlation key, used for the
 *  acknowledgement and to suppress a redelivery of the same command. Returns
 *  false and sets *err (when non-NULL) to "invalid_command",
 *  "sms_bad_number" or "sms_text_too_long". A `len` of 0 means strlen. */
bool cpe_sms_mqtt_cmd_parse(const char *json, size_t len, cpe_sms_mqtt_cmd_t *out,
                            const char **err);

/** Build the uplink JSON for one received message. Returns the payload length,
 *  or 0 when the buffer is too small. */
size_t cpe_sms_mqtt_build_sms(char *buf, size_t cap, const char *from,
                              const char *text, const char *time, uint8_t parts,
                              uint32_t id_hash);

/** Build a command acknowledgement. `error` is ignored when `ok` is true. */
size_t cpe_sms_mqtt_build_ack(char *buf, size_t cap, const char *id, bool ok,
                              const char *error);

/** Stable hash of a received message, used as its identity on the wire and in
 *  the duplicate ring. */
uint32_t cpe_sms_mqtt_msg_hash(const char *from, const char *time, const char *text);

void cpe_sms_mqtt_ring_init(cpe_sms_mqtt_ring_t *ring);
bool cpe_sms_mqtt_ring_contains(const cpe_sms_mqtt_ring_t *ring, uint32_t hash);
/** Add unless already present. */
void cpe_sms_mqtt_ring_add(cpe_sms_mqtt_ring_t *ring, uint32_t hash);

#ifdef __cplusplus
}
#endif
