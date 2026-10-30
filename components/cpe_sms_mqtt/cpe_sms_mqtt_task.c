/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The running side of SMS forwarding.
 *
 * Three tasks keep the concerns apart: the control task owns the MQTT client
 * lifecycle, the watcher task lists the modem's inbox and publishes what is
 * new, and the downlink worker executes send commands without delaying either.
 * The client only exists while forwarding is enabled and the WAN link is up,
 * so a wrong broker address can never hold the data path hostage.
 */

#include <stddef.h>
#include <string.h>

#include "esp_err.h"

#include "cpe_sms_mqtt.h"

#if CONFIG_CPE_SMS_MQTT_ENABLE

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cpe_sms.h"
#include "cpe_sms_mqtt_store.h"
#include "cpe_status.h"
#include "cpe_system.h"

static const char *TAG = "sms_mqtt";

#define CTRL_PERIOD_MS        2000
#define WATCH_PERIOD_MS       1000
/* A burst of +CMTI indications is folded into one listing. */
#define RX_DEBOUNCE_MS        500
/* An indication can be lost (URC overflow, modem reset), so the inbox is
 * re-read periodically anyway. */
#define FULL_SCAN_MS          (5 * 60 * 1000)
#define SCAN_RETRY_MS         2000
#define LIST_TIMEOUT_MS       20000
#define SEND_TIMEOUT_MS       65000
#define PENDING_MAX           8
#define CMD_QUEUE_DEPTH       4
#define CMD_SEEN_SIZE         16
#define PAYLOAD_MAX           6144
#define SNTP_SERVER           "cn.pool.ntp.org"
/* Anything below this is the 1970 epoch plus a few days: the clock has not been
 * set, so the timestamps are only good for ordering. */
#define TIME_SANE_EPOCH       1700000000

typedef struct {
    int msg_id;
    uint32_t hash;
} pending_t;

typedef struct {
    char *payload;
    size_t len;
} cmd_item_t;

static SemaphoreHandle_t s_cfg_lock;
static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_client_lock;
static QueueHandle_t s_cmd_queue;
static bool s_inited;

static cpe_sms_mqtt_config_t s_cfg;

static esp_mqtt_client_handle_t s_client;
static char s_uri[96];
static char s_client_id[24];
/* Owned by the control task: replaced right after client_stop(), so no live
 * client ever points at a freed document. NULL means the built-in bundle. */
static char *s_ca;

static volatile bool s_connected;
static volatile bool s_reload;
static volatile bool s_force_scan;
static volatile bool s_sntp_started;

static cpe_sms_mqtt_ring_t s_ring;
static pending_t s_pending[PENDING_MAX];
static uint8_t s_pending_count;
static volatile bool s_ring_dirty;

static uint32_t s_cmd_seen[CMD_SEEN_SIZE];
static uint8_t s_cmd_seen_next;
static uint8_t s_cmd_seen_count;

static cpe_sms_mqtt_status_t s_status;

/* ------------------------------------------------------------- helpers --- */

static void status_set_state(cpe_sms_mqtt_state_t state, const char *error)
{
    if (s_state_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_status.state = state;
    if (error != NULL) {
        snprintf(s_status.last_error, sizeof(s_status.last_error), "%s", error);
    } else if (state == CPE_SMS_MQTT_CONNECTED || state == CPE_SMS_MQTT_DISABLED) {
        s_status.last_error[0] = '\0';
    }
    xSemaphoreGive(s_state_lock);
}

static void cfg_snapshot(cpe_sms_mqtt_config_t *out)
{
    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_cfg_lock);
}

static void ca_reload(void)
{
    free(s_ca);
    s_ca = cpe_sms_mqtt_store_cert_load();
}

static void pending_clear(void)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_pending_count = 0;
    xSemaphoreGive(s_state_lock);
}

static bool seen_or_in_flight(uint32_t hash)
{
    if (cpe_sms_mqtt_ring_contains(&s_ring, hash)) {
        return true;
    }
    for (uint8_t i = 0; i < s_pending_count; i++) {
        if (s_pending[i].hash == hash) {
            return true;
        }
    }
    return false;
}

static bool command_seen(uint32_t hash)
{
    for (uint8_t i = 0; i < s_cmd_seen_count; i++) {
        if (s_cmd_seen[i] == hash) {
            return true;
        }
    }
    return false;
}

static void command_remember(uint32_t hash)
{
    if (command_seen(hash)) {
        return;
    }
    if (s_cmd_seen_count < CMD_SEEN_SIZE) {
        s_cmd_seen[s_cmd_seen_count++] = hash;
        return;
    }
    s_cmd_seen[s_cmd_seen_next] = hash;
    s_cmd_seen_next = (uint8_t)((s_cmd_seen_next + 1) % CMD_SEEN_SIZE);
}

/* Publish at QoS 1 and remember the hash until the broker acknowledges it, so a
 * message that never made it out is retried by the next scan. The state lock is
 * held across the publish: the event handler takes the same lock, and that
 * closes the window where PUBLISHED could arrive before the entry exists. */
static bool publish_sms(const cpe_sms_mqtt_config_t *cfg, const char *payload,
                        size_t len, uint32_t hash)
{
    bool ok = false;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (s_client != NULL && s_connected && s_pending_count < PENDING_MAX) {
        int msg_id = esp_mqtt_client_publish(s_client, cfg->topic_up, payload,
                                             (int)len, 1, 0);
        if (msg_id >= 0) {
            s_pending[s_pending_count].msg_id = msg_id;
            s_pending[s_pending_count].hash = hash;
            s_pending_count++;
            ok = true;
        }
    }
    xSemaphoreGive(s_client_lock);
    xSemaphoreGive(s_state_lock);
    if (!ok) {
        ESP_LOGW(TAG, "Publish failed, message kept for the next scan");
    }
    return ok;
}

static void publish_ack(const cpe_sms_mqtt_config_t *cfg, const char *id, bool ok,
                        const char *error)
{
    char payload[CPE_SMS_MQTT_ID_MAX + 128];
    size_t len = cpe_sms_mqtt_build_ack(payload, sizeof(payload), id, ok, error);
    if (len == 0) {
        return;
    }
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (s_client != NULL && s_connected) {
        (void)esp_mqtt_client_publish(s_client, cfg->topic_up, payload, (int)len, 1, 0);
    }
    xSemaphoreGive(s_client_lock);
}

/* ----------------------------------------------------------- MQTT events -- */

static const char *mqtt_error_code(const esp_mqtt_event_handle_t event)
{
    const esp_mqtt_error_codes_t *err = event->error_handle;
    if (err == NULL) {
        return "mqtt_error";
    }
    switch (err->error_type) {
    case MQTT_ERROR_TYPE_TCP_TRANSPORT:
        /* No socket at all happens before any TLS work, and calling it a
         * certificate problem sends the user looking in the wrong place. */
        if (err->esp_tls_last_esp_err == ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET) {
            return "socket_error";
        }
        return err->esp_tls_last_esp_err != 0 ? "tls_error" : "network_error";
    case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
        return "connection_refused";
    case MQTT_ERROR_TYPE_SUBSCRIBE_FAILED:
        return "subscribe_failed";
    default:
        return "mqtt_error";
    }
}

static void queue_downlink(esp_mqtt_event_handle_t event)
{
    if (event->data == NULL || event->data_len <= 0 || event->data_len > PAYLOAD_MAX ||
        event->current_data_offset != 0 || event->total_data_len != event->data_len) {
        ESP_LOGW(TAG, "Dropped an unusable downlink payload");
        return;
    }
    if (s_cmd_queue == NULL) {
        return;
    }
    cmd_item_t item = {.payload = malloc((size_t)event->data_len + 1),
                       .len = (size_t)event->data_len};
    if (item.payload == NULL) {
        ESP_LOGW(TAG, "No memory for a downlink command");
        return;
    }
    memcpy(item.payload, event->data, item.len);
    item.payload[item.len] = '\0';
    if (xQueueSend(s_cmd_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Downlink queue full, command dropped");
        free(item.payload);
    }
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_BEFORE_CONNECT:
        status_set_state(CPE_SMS_MQTT_CONNECTING, NULL);
        break;

    case MQTT_EVENT_CONNECTED: {
        cpe_sms_mqtt_config_t cfg;
        cfg_snapshot(&cfg);
        s_connected = true;
        s_force_scan = true;
        status_set_state(CPE_SMS_MQTT_CONNECTED, NULL);
        ESP_LOGI(TAG, "Connected to %s", cfg.host);
        esp_mqtt_client_subscribe(event->client, cfg.topic_down, 1);
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        pending_clear();
        status_set_state(CPE_SMS_MQTT_CONNECTING, NULL);
        break;

    case MQTT_EVENT_ERROR: {
        s_connected = false;
        pending_clear();
        const char *code = mqtt_error_code(event);
        status_set_state(CPE_SMS_MQTT_ERROR, code);
        esp_err_t tls_err = event->error_handle != NULL
                                ? event->error_handle->esp_tls_last_esp_err : 0;
        if (tls_err != 0) {
            ESP_LOGW(TAG, "MQTT error: %s (esp-tls 0x%x)", code, (unsigned)tls_err);
        } else {
            ESP_LOGW(TAG, "MQTT error: %s", code);
        }
        break;
    }

    case MQTT_EVENT_PUBLISHED:
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        for (uint8_t i = 0; i < s_pending_count; i++) {
            if (s_pending[i].msg_id != event->msg_id) {
                continue;
            }
            cpe_sms_mqtt_ring_add(&s_ring, s_pending[i].hash);
            s_ring_dirty = true;
            s_pending[i] = s_pending[--s_pending_count];
            break;
        }
        xSemaphoreGive(s_state_lock);
        break;

    case MQTT_EVENT_DATA: {
        cpe_sms_mqtt_config_t cfg;
        cfg_snapshot(&cfg);
        size_t want = strlen(cfg.topic_down);
        if (event->topic_len == (int)want && want > 0 &&
            memcmp(event->topic, cfg.topic_down, want) == 0) {
            queue_downlink(event);
        }
        break;
    }

    default:
        break;
    }
}

/* --------------------------------------------------------- client start --- */

static void client_stop(void)
{
    pending_clear();

    /* Detach under the lock so the watcher and the worker stop publishing, then
     * destroy outside it: destroy waits for the MQTT task to exit, and that task
     * takes the state lock from its event handler. */
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    esp_mqtt_client_handle_t client = s_client;
    s_client = NULL;
    s_connected = false;
    xSemaphoreGive(s_client_lock);

    if (client != NULL) {
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
    }
}

static esp_err_t client_start(const cpe_sms_mqtt_config_t *cfg)
{
    snprintf(s_uri, sizeof(s_uri), "mqtts://%s:%u", cfg->host, (unsigned)cfg->port);

    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_client_id, sizeof(s_client_id), "cpe-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_mqtt_client_config_t c = {0};
    c.broker.address.uri = s_uri;
    c.credentials.client_id = s_client_id;
    /* Both strings are copied into the client state at init, so the snapshot
     * on this stack does not need to outlive the call. */
    if (cfg->username[0] != '\0') {
        c.credentials.username = cfg->username;
        if (cfg->password[0] != '\0') {
            c.credentials.authentication.password = cfg->password;
        }
    }
    c.session.keepalive = 60;
    c.session.disable_clean_session = true;
    c.network.reconnect_timeout_ms = 5000;
    c.network.timeout_ms = 10000;
    c.buffer.size = PAYLOAD_MAX;
    c.task.stack_size = 6144;
    c.task.priority = 5;
    /* A user-installed CA replaces the public bundle rather than extending it,
     * so a private broker cannot be reached through some other public chain. */
    if (s_ca != NULL) {
        c.broker.verification.certificate = s_ca;
        c.broker.verification.certificate_len = strlen(s_ca) + 1;
    } else {
        c.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&c);
    if (client == NULL) {
        ESP_LOGE(TAG, "Could not create the MQTT client");
        status_set_state(CPE_SMS_MQTT_ERROR, "mqtt_init_failed");
        return ESP_ERR_NO_MEM;
    }
    /* Publish the handle before start: the first events (CONNECTED sets the
     * scan flag) race with the watcher, and a NULL handle would drop the first
     * round of messages. */
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    s_client = client;
    s_connected = false;
    xSemaphoreGive(s_client_lock);

    esp_err_t err = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                                   on_mqtt_event, NULL);
    if (err == ESP_OK) {
        err = esp_mqtt_client_start(client);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the MQTT client: %s", esp_err_to_name(err));
        xSemaphoreTake(s_client_lock, portMAX_DELAY);
        s_client = NULL;
        xSemaphoreGive(s_client_lock);
        esp_mqtt_client_destroy(client);
        status_set_state(CPE_SMS_MQTT_ERROR, "mqtt_start_failed");
        return err;
    }

    status_set_state(CPE_SMS_MQTT_CONNECTING, NULL);
    ESP_LOGI(TAG, "Forwarding to %s", s_uri);
    return ESP_OK;
}

/* --------------------------------------------------------------- uplink --- */

static bool message_wanted(const cpe_sms_msg_t *msg)
{
    /* Only received messages: the sent ones carry the recipient in `from`, and
     * a half-transmitted draft is not worth forwarding. */
    if (msg->stat != CPE_SMS_STAT_UNREAD && msg->stat != CPE_SMS_STAT_READ) {
        return false;
    }
    return !msg->truncated && msg->from[0] != '\0' && msg->body != NULL &&
           msg->body[0] != '\0';
}

/* Returns true when the inbox was actually listed, so the caller only resets
 * its scan timers then: a busy AT port must not suppress the next attempt. */
static bool scan_and_publish(const cpe_sms_mqtt_config_t *cfg)
{
    cpe_sms_list_t *list = calloc(1, sizeof(*list));
    char *payload = malloc(PAYLOAD_MAX);
    if (list == NULL || payload == NULL) {
        ESP_LOGW(TAG, "No memory to scan the inbox");
        free(list);
        free(payload);
        return false;
    }

    esp_err_t err = cpe_sms_list(list, LIST_TIMEOUT_MS);
    if (err != ESP_OK) {
        /* Another AT user holds the port; the next scan retries. */
        cpe_sms_list_clear(list);
        free(list);
        free(payload);
        return false;
    }

    uint8_t sent = 0;
    for (size_t i = 0; i < list->count; i++) {
        const cpe_sms_msg_t *msg = &list->items[i];
        if (!message_wanted(msg)) {
            continue;
        }
        uint32_t hash = cpe_sms_mqtt_msg_hash(msg->from, msg->time, msg->body);

        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        bool skip = seen_or_in_flight(hash);
        xSemaphoreGive(s_state_lock);
        if (skip) {
            continue;
        }

        size_t len = cpe_sms_mqtt_build_sms(payload, PAYLOAD_MAX, msg->from, msg->body,
                                            msg->time, msg->part_total, hash);
        if (len == 0) {
            ESP_LOGW(TAG, "Could not encode a %u byte message", (unsigned)strlen(msg->body));
            continue;
        }
        if (publish_sms(cfg, payload, len, hash)) {
            sent++;
        }
        if (sent >= PENDING_MAX) {
            break;
        }
    }
    cpe_sms_list_clear(list);
    free(list);
    free(payload);
    if (sent > 0) {
        ESP_LOGI(TAG, "Forwarded %u message(s)", sent);
    }
    return true;
}

static void watch_task(void *arg)
{
    (void)arg;
    uint32_t gen_done = cpe_sms_rx_generation();
    uint32_t gen_seen = gen_done;
    int64_t gen_changed_at = 0;
    int64_t last_scan = esp_timer_get_time() / 1000;
    int64_t last_try = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WATCH_PERIOD_MS));

        if (s_ring_dirty) {
            cpe_sms_mqtt_ring_t copy;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            copy = s_ring;
            s_ring_dirty = false;
            xSemaphoreGive(s_state_lock);
            (void)cpe_sms_mqtt_store_ring_save(&copy);
        }

        if (!s_connected) {
            gen_seen = cpe_sms_rx_generation();
            gen_done = gen_seen;
            continue;
        }
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        uint8_t pending = s_pending_count;
        xSemaphoreGive(s_state_lock);
        if (pending >= PENDING_MAX) {
            /* Wait for the broker to acknowledge what is already on the wire. */
            continue;
        }

        cpe_sms_mqtt_config_t cfg;
        cfg_snapshot(&cfg);
        if (!cfg.enabled) {
            continue;
        }

        int64_t now = esp_timer_get_time() / 1000;
        uint32_t gen = cpe_sms_rx_generation();
        if (gen != gen_seen) {
            gen_seen = gen;
            gen_changed_at = now;
        }

        bool due = s_force_scan || (gen != gen_done && now - gen_changed_at >= RX_DEBOUNCE_MS) ||
                   (now - last_scan >= FULL_SCAN_MS);
        if (!due || now - last_try < SCAN_RETRY_MS) {
            continue;
        }
        last_try = now;
        if (scan_and_publish(&cfg)) {
            s_force_scan = false;
            last_scan = now;
            /* The listing marks records read, so the generation will not move
             * again on its own; remember what was covered. */
            gen_done = gen;
        }
    }
}

/* -------------------------------------------------------------- downlink --- */

static void handle_command(const char *payload, size_t len)
{
    cpe_sms_mqtt_config_t cfg;
    cfg_snapshot(&cfg);

    cpe_sms_mqtt_cmd_t cmd;
    const char *err = NULL;
    if (!cpe_sms_mqtt_cmd_parse(payload, len, &cmd, &err)) {
        ESP_LOGW(TAG, "Rejected a downlink command: %s", err != NULL ? err : "invalid");
        publish_ack(&cfg, cmd.id, false, err != NULL ? err : "invalid_command");
        return;
    }
    if (!cfg.enabled) {
        return;
    }
    if (!cfg.allow_remote_send) {
        publish_ack(&cfg, cmd.id, false, "remote_send_disabled");
        return;
    }
    uint32_t id_hash = cpe_sms_mqtt_msg_hash(cmd.id, "", "");
    if (command_seen(id_hash)) {
        /* Broker redelivery after a reconnect: acknowledge, do not resend. */
        publish_ack(&cfg, cmd.id, true, NULL);
        return;
    }

    const char *send_err = NULL;
    esp_err_t err_send = cpe_sms_send(cmd.to, cmd.text, &send_err, SEND_TIMEOUT_MS);
    if (err_send == ESP_OK) {
        command_remember(id_hash);
        ESP_LOGI(TAG, "Remote send accepted");
        publish_ack(&cfg, cmd.id, true, NULL);
    } else {
        ESP_LOGW(TAG, "Remote send failed: %s",
                 send_err != NULL ? send_err : esp_err_to_name(err_send));
        publish_ack(&cfg, cmd.id, false, send_err != NULL ? send_err : "sms_send_failed");
    }
}

static void downlink_task(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_item_t item;
        if (xQueueReceive(s_cmd_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        handle_command(item.payload, item.len);
        free(item.payload);
    }
}

/* ---------------------------------------------------------------- control -- */

static void ctrl_task(void *arg)
{
    (void)arg;
    int64_t next_start_ms = 0;
    for (;;) {
        cpe_status_t st;
        cpe_status_get(&st);
        bool online = st.state == CPE_STATE_ONLINE;
        bool synced = time(NULL) >= TIME_SANE_EPOCH;
        int64_t now = esp_timer_get_time() / 1000;

        if (online && !s_sntp_started) {
            if (esp_netif_sntp_start() == ESP_OK) {
                s_sntp_started = true;
                ESP_LOGI(TAG, "NTP started for the timestamps");
            }
        }

        if (s_reload) {
            s_reload = false;
            client_stop();
            ca_reload();
        }

        cpe_sms_mqtt_config_t cfg;
        cfg_snapshot(&cfg);
        if (!cfg.enabled) {
            if (s_client != NULL) {
                client_stop();
            }
            status_set_state(CPE_SMS_MQTT_DISABLED, NULL);
        } else if (!online) {
            if (s_client != NULL) {
                client_stop();
            }
            status_set_state(CPE_SMS_MQTT_IDLE, NULL);
        } else if (s_client == NULL && now >= next_start_ms) {
            /* A client that cannot even be created (no memory) should not be
             * retried on every tick. */
            if (client_start(&cfg) != ESP_OK) {
                next_start_ms = now + 10000;
            }
        }

        cpe_sms_mqtt_status_t runtime = {0};
        runtime.wan_online = online;
        runtime.time_synced = synced;
        if (s_cmd_queue != NULL) {
            runtime.queued = (uint8_t)uxQueueMessagesWaiting(s_cmd_queue);
        }
        runtime.free_heap = esp_get_free_heap_size();

        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_status.wan_online = runtime.wan_online;
        s_status.time_synced = runtime.time_synced;
        s_status.cert_installed = s_ca != NULL;
        s_status.queued = runtime.queued;
        s_status.pending = s_pending_count;
        s_status.free_heap = runtime.free_heap;
        xSemaphoreGive(s_state_lock);

        vTaskDelay(pdMS_TO_TICKS(CTRL_PERIOD_MS));
    }
}

/* ------------------------------------------------------------------ API --- */

static void sntp_setup(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
    cfg.start = false;  /* started by the control task once the WAN is up */
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NTP is unavailable: %s", esp_err_to_name(err));
    }
}

static void factory_reset_hook(void)
{
    cpe_sms_mqtt_store_erase_all();
    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    cpe_sms_mqtt_config_defaults(&s_cfg);
    xSemaphoreGive(s_cfg_lock);
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    cpe_sms_mqtt_ring_init(&s_ring);
    s_ring_dirty = false;
    /* An id remembered before the reset must not swallow a command published
     * afterwards. */
    s_cmd_seen_count = 0;
    s_cmd_seen_next = 0;
    xSemaphoreGive(s_state_lock);
    s_reload = true;
}

esp_err_t cpe_sms_mqtt_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_cfg_lock = xSemaphoreCreateMutex();
    s_state_lock = xSemaphoreCreateMutex();
    s_client_lock = xSemaphoreCreateMutex();
    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(cmd_item_t));
    if (s_cfg_lock == NULL || s_state_lock == NULL || s_client_lock == NULL ||
        s_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Could not create the forwarding primitives");
        return ESP_ERR_NO_MEM;
    }

    cpe_sms_mqtt_config_defaults(&s_cfg);
    (void)cpe_sms_mqtt_store_load(&s_cfg);
    cpe_sms_mqtt_store_ring_load(&s_ring);
    ca_reload();

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = s_cfg.enabled ? CPE_SMS_MQTT_IDLE : CPE_SMS_MQTT_DISABLED;
    s_status.cert_installed = s_ca != NULL;

    sntp_setup();
    esp_err_t err = cpe_system_register_factory_reset_hook(factory_reset_hook,
                                                           "sms_mqtt");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Factory reset will leave the forwarding settings behind");
    }

    if (xTaskCreate(ctrl_task, "sms_mqtt_ctrl", 4096, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(watch_task, "sms_mqtt_watch", 6144, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(downlink_task, "sms_mqtt_down", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the forwarding tasks");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "SMS forwarding ready (%s)", s_cfg.enabled ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t cpe_sms_mqtt_get_config(cpe_sms_mqtt_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cfg_lock == NULL) {
        cpe_sms_mqtt_config_defaults(out);
        return ESP_OK;
    }
    cfg_snapshot(out);
    return ESP_OK;
}

esp_err_t cpe_sms_mqtt_check_config(const cpe_sms_mqtt_config_t *cfg, const char **err)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return cpe_sms_mqtt_config_check(cfg, err) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t cpe_sms_mqtt_set_config(const cpe_sms_mqtt_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *err = NULL;
    if (!cpe_sms_mqtt_config_check(cfg, &err)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t rc = cpe_sms_mqtt_store_save(cfg);
    if (rc != ESP_OK) {
        return rc;
    }
    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    s_cfg = *cfg;
    xSemaphoreGive(s_cfg_lock);
    s_reload = true;
    return ESP_OK;
}

esp_err_t cpe_sms_mqtt_set_cert(const char *pem)
{
    if (!cpe_sms_mqtt_pem_valid(pem)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = cpe_sms_mqtt_store_cert_save(pem);
    if (err != ESP_OK) {
        return err;
    }
    s_reload = true;
    return ESP_OK;
}

esp_err_t cpe_sms_mqtt_clear_cert(void)
{
    esp_err_t err = cpe_sms_mqtt_store_cert_clear();
    if (err != ESP_OK) {
        return err;
    }
    s_reload = true;
    return ESP_OK;
}

void cpe_sms_mqtt_get_status(cpe_sms_mqtt_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_state_lock != NULL) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        *out = s_status;
        xSemaphoreGive(s_state_lock);
    }
}

#else /* !CONFIG_CPE_SMS_MQTT_ENABLE */

/* The component is always compiled so the management console can be built
 * without the option; every entry point degrades to "not supported". */
esp_err_t cpe_sms_mqtt_init(void) { return ESP_OK; }

esp_err_t cpe_sms_mqtt_get_config(cpe_sms_mqtt_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cpe_sms_mqtt_config_defaults(out);
    return ESP_OK;
}

esp_err_t cpe_sms_mqtt_check_config(const cpe_sms_mqtt_config_t *cfg, const char **err)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return cpe_sms_mqtt_config_check(cfg, err) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t cpe_sms_mqtt_set_config(const cpe_sms_mqtt_config_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cpe_sms_mqtt_set_cert(const char *pem)
{
    (void)pem;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cpe_sms_mqtt_clear_cert(void) { return ESP_ERR_NOT_SUPPORTED; }

void cpe_sms_mqtt_get_status(cpe_sms_mqtt_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = CPE_SMS_MQTT_DISABLED;
}

#endif /* CONFIG_CPE_SMS_MQTT_ENABLE */
