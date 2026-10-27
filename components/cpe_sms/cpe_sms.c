/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SMS over the modem AT interface. The AT port stays owned by cpe_at; every
 * transaction runs as a cpe_at_session body on the AT task, so telemetry and a
 * pending modem reset are never starved (the body's read_until aborts on
 * reset). Capability is probed on the rising edge of cpe_at_ready(), so a
 * re-enumerated modem is re-probed and a non-SMS modem is never hammered.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "cpe_at.h"
#include "cpe_at_urc.h"
#include "cpe_sms.h"
#include "cpe_sms_parse.h"
#include "cpe_sms_ucs2.h"

#if CONFIG_CPE_SMS_ENABLE && CONFIG_CPE_AT_ENABLE

static const char *TAG = "cpe_sms";

#define CPE_SMS_PROBE_POLL_MS      2000
#define CPE_SMS_CMD_TIMEOUT_MS     2000
#define CPE_SMS_LIST_TIMEOUT_MS    20000
#define CPE_SMS_SEND_TIMEOUT_MS    60000
#define CPE_SMS_DELETE_TIMEOUT_MS  10000
/* Worst case is a full inbox of 24 records: each PDU is at most 306 hex chars
 * on one line plus a ~40-char header. */
#define CPE_SMS_RAW_CAP            20480
#define CPE_SMS_CMD_RESP           128
#define CPE_SMS_SEND_MIN_INTERVAL_MS 5000
#define CPE_SMS_MAINT_INTERVAL_MS  60000
#define CPE_SMS_REPROBE_MS         300000

static cpe_sms_state_t s_state = CPE_SMS_UNSUPPORTED;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_op_lock;
static int64_t s_last_send_us;
static TaskHandle_t s_task;

static cpe_sms_state_t sms_get_state(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cpe_sms_state_t st = s_state;
    xSemaphoreGive(s_lock);
    return st;
}

static void sms_set_state(cpe_sms_state_t st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = st;
    xSemaphoreGive(s_lock);
}

static bool sms_rate_limited(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool limited = (esp_timer_get_time() - s_last_send_us) <
                   (int64_t)CPE_SMS_SEND_MIN_INTERVAL_MS * 1000LL;
    xSemaphoreGive(s_lock);
    return limited;
}

static void sms_mark_sent(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last_send_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

/* cpe_at_session refuses a second body rather than queuing it, so serialize
 * the SMS operations here: a list poll that lands during a send then waits for
 * it instead of surfacing a spurious failure. The wait comes out of the
 * caller's budget, which the session call below also uses. */
static bool sms_op_begin(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_op_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void sms_op_end(void)
{
    xSemaphoreGive(s_op_lock);
}

static uint32_t sms_remaining_ms(int64_t start_us, uint32_t budget_ms)
{
    int64_t spent_ms = (esp_timer_get_time() - start_us) / 1000;
    if (spent_ms < 0 || spent_ms >= (int64_t)budget_ms) {
        return 0;
    }
    return budget_ms - (uint32_t)spent_ms;
}

/* Text-entry mode is only used for sending; restore the PDU-mode/GSM state the
 * receive path and telemetry expect. */
static void sms_restore_idle(const cpe_at_io_t *io)
{
    char resp[64];
    (void)io->send_line("AT+CSCS=\"GSM\"");
    (void)io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp));
    (void)io->send_line("AT+CMGF=0");
    (void)io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp));
}

/* ------------------------------ capability ------------------------------ */

static esp_err_t sms_probe_body(const cpe_at_io_t *io, void *arg)
{
    (void)arg;
    char resp[CPE_SMS_CMD_RESP];
    if (io->send_line("AT+CMGF=0") != ESP_OK) {
        return ESP_FAIL;
    }
    if (io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp)) != ESP_OK ||
        strstr(resp, "OK") == NULL) {
        return ESP_FAIL;
    }

    /* Pin all three storage areas (read/list, write/send, receive) to the
     * modem's own memory so reception never silently depends on the SIM. */
    if (io->send_line("AT+CPMS=\"ME\",\"ME\",\"ME\"") != ESP_OK ||
        io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp)) != ESP_OK ||
        strstr(resp, "OK") == NULL) {
        ESP_LOGW(TAG, "AT+CPMS ME rejected; keeping modem default storage");
    }

    /* Route new messages as +CMTI indications while keeping them in storage;
     * the body is fetched by the regular list operation. Indications are an
     * optimisation only, so a modem that rejects the setting stays usable. */
    if (io->send_line("AT+CNMI=2,1,0,0,0") != ESP_OK ||
        io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp)) != ESP_OK ||
        strstr(resp, "OK") == NULL) {
        ESP_LOGW(TAG, "AT+CNMI rejected; new-message indications unavailable");
    }
    return ESP_OK;
}

/* Returns false when another SMS operation owns the AT session; the caller
 * retries later. */
static bool sms_probe(void)
{
    if (!sms_op_begin(0)) {
        return false;
    }
    esp_err_t r = cpe_at_session(sms_probe_body, NULL, CPE_SMS_CMD_TIMEOUT_MS + 1000);
    sms_op_end();
    sms_set_state(r == ESP_OK ? CPE_SMS_READY : CPE_SMS_UNSUPPORTED);
    ESP_LOGI(TAG, "SMS capability %s", r == ESP_OK ? "ready" : "unsupported");
    return true;
}

/* -------------------------------- list ---------------------------------- */

typedef struct {
    cpe_sms_list_t *out;
    char *raw;
    size_t cap;
} sms_list_ctx_t;

static bool sms_cpms_query(const cpe_at_io_t *io, int *used, int *total)
{
    char resp[CPE_SMS_CMD_RESP];
    if (io->send_line("AT+CPMS?") != ESP_OK) {
        return false;
    }
    if (io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp)) != ESP_OK) {
        return false;
    }
    return cpe_sms_parse_cpms(resp, used, total);
}

/* PDU mode returns the concatenation UDH with the text, so one list command is
 * enough: no per-record round trips and no vendor-private command. */
static esp_err_t sms_fetch(const cpe_at_io_t *io, sms_list_ctx_t *ctx)
{
    char resp[CPE_SMS_CMD_RESP];

    if (io->send_line("AT+CMGF=0") != ESP_OK) {
        return ESP_FAIL;
    }
    if (io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp)) != ESP_OK ||
        strstr(resp, "OK") == NULL) {
        return ESP_FAIL;
    }

    if (io->send_line("AT+CMGL=4") != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t r = io->read_until(NULL, 0, CPE_SMS_LIST_TIMEOUT_MS, ctx->raw, ctx->cap);
    if (r != ESP_OK) {
        return r;
    }
    /* An ERROR final line parses as an empty list; surface it instead of
     * showing the inbox as empty. */
    if (strstr(ctx->raw, "ERROR") != NULL) {
        return ESP_FAIL;
    }
    if (!cpe_sms_parse_cmgl_pdu(ctx->raw, ctx->out)) {
        return ESP_FAIL;
    }

    cpe_sms_merge_parts(ctx->out);
    (void)sms_cpms_query(io, &ctx->out->storage_used, &ctx->out->storage_capacity);
    return ESP_OK;
}

static esp_err_t sms_list_body(const cpe_at_io_t *io, void *arg)
{
    return sms_fetch(io, arg);
}

esp_err_t cpe_sms_list(cpe_sms_list_t *out, uint32_t timeout_ms)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sms_get_state() != CPE_SMS_READY) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    int64_t start_us = esp_timer_get_time();
    if (!sms_op_begin(timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }
    char *raw = malloc(CPE_SMS_RAW_CAP);
    if (raw == NULL) {
        sms_op_end();
        return ESP_ERR_NO_MEM;
    }
    sms_list_ctx_t ctx = { .out = out, .raw = raw, .cap = CPE_SMS_RAW_CAP };
    esp_err_t err = cpe_at_session(sms_list_body, &ctx,
                                   sms_remaining_ms(start_us, timeout_ms));
    free(raw);
    sms_op_end();
    return err;
}

/* -------------------------------- send ---------------------------------- */

typedef struct {
    const char *number;
    const char *text;
    const char **err_str;
} sms_send_ctx_t;

/* One short control command; it succeeds only on a plain OK. */
static bool sms_cmd(const cpe_at_io_t *io, const char *cmd)
{
    char resp[CPE_SMS_CMD_RESP];
    if (io->send_line(cmd) != ESP_OK ||
        io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp)) != ESP_OK) {
        return false;
    }
    return strstr(resp, "OK") != NULL;
}

/* Leave text-entry mode and put the character set back for telemetry. */
static void sms_abort_send(const cpe_at_io_t *io)
{
    const uint8_t esc = 0x1B;
    (void)io->write(&esc, 1);
    sms_restore_idle(io);
}

static esp_err_t sms_send_body(const cpe_at_io_t *io, void *arg)
{
    sms_send_ctx_t *ctx = arg;
    char resp[CPE_SMS_CMD_RESP];

    bool ascii = false;
    if (!cpe_sms_text_measure(ctx->text, &ascii, NULL)) {
        if (ctx->err_str) *ctx->err_str = "sms_text_too_long";
        return ESP_ERR_INVALID_ARG;
    }

    /* Reception runs in PDU mode; text-entry mode is switched on only here. */
    if (!sms_cmd(io, "AT+CMGF=1")) {
        sms_restore_idle(io);
        return ESP_FAIL;
    }

    char num_hex[CPE_SMS_MAX_ADDR_HEX];
    const char *addr = ctx->number;
    if (ascii) {
        if (!sms_cmd(io, "AT+CSCS=\"GSM\"") || !sms_cmd(io, "AT+CSMP=17,167,0,0")) {
            sms_restore_idle(io);
            return ESP_FAIL;
        }
    } else {
        if (!sms_cmd(io, "AT+CSCS=\"UCS2\"") || !sms_cmd(io, "AT+CSMP=17,167,0,8")) {
            sms_restore_idle(io);
            return ESP_FAIL;
        }
        if (cpe_sms_ucs2_encode(ctx->number, num_hex, sizeof(num_hex)) == 0) {
            sms_restore_idle(io);
            if (ctx->err_str) *ctx->err_str = "sms_bad_number";
            return ESP_ERR_INVALID_ARG;
        }
        addr = num_hex;
    }

    char cmd[192];
    int n = snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", addr);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        sms_restore_idle(io);
        return ESP_ERR_INVALID_SIZE;
    }
    if (io->send_line(cmd) != ESP_OK) {
        sms_restore_idle(io);
        return ESP_FAIL;
    }

    static const char *prompt[] = { ">" };
    if (io->read_until(prompt, 1, 8000, resp, sizeof(resp)) != ESP_OK) {
        sms_abort_send(io);
        if (ctx->err_str) *ctx->err_str = "sms_no_prompt";
        return ESP_ERR_TIMEOUT;
    }

    const uint8_t *payload;
    size_t payload_len;
    char body_hex[512];
    if (ascii) {
        payload = (const uint8_t *)ctx->text;
        payload_len = strlen(ctx->text);
    } else {
        size_t hl = cpe_sms_ucs2_encode(ctx->text, body_hex, sizeof(body_hex));
        if (hl == 0) {
            sms_abort_send(io);
            if (ctx->err_str) *ctx->err_str = "sms_bad_text";
            return ESP_ERR_INVALID_ARG;
        }
        payload = (const uint8_t *)body_hex;
        payload_len = hl;
    }

    if (io->write(payload, payload_len) != ESP_OK) {
        sms_abort_send(io);
        return ESP_FAIL;
    }
    uint8_t ctrl_z = 0x1A;
    if (io->write(&ctrl_z, 1) != ESP_OK) {
        sms_abort_send(io);
        return ESP_FAIL;
    }

    static const char *result_tokens[] = { "+CMGS:" };
    esp_err_t r = io->read_until(result_tokens, 1, CPE_SMS_SEND_TIMEOUT_MS, resp, sizeof(resp));
    sms_restore_idle(io);

    if (r != ESP_OK) {
        if (ctx->err_str) *ctx->err_str = "sms_send_timeout";
        return r;
    }
    if (strstr(resp, "+CMGS:") != NULL) {
        return ESP_OK;
    }
    if (ctx->err_str) *ctx->err_str = "sms_send_failed";
    return ESP_FAIL;
}

esp_err_t cpe_sms_send(const char *number, const char *text, const char **err_str,
                       uint32_t timeout_ms)
{
    if (err_str) *err_str = NULL;
    if (!cpe_sms_number_valid(number)) {
        if (err_str) *err_str = "sms_bad_number";
        return ESP_ERR_INVALID_ARG;
    }
    if (sms_get_state() != CPE_SMS_READY) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (sms_rate_limited()) {
        if (err_str) *err_str = "sms_rate_limited";
        return ESP_ERR_INVALID_STATE;
    }
    int64_t start_us = esp_timer_get_time();
    if (!sms_op_begin(timeout_ms)) {
        if (err_str) *err_str = "sms_busy";
        return ESP_ERR_TIMEOUT;
    }
    sms_send_ctx_t ctx = { .number = number, .text = text, .err_str = err_str };
    esp_err_t err = cpe_at_session(sms_send_body, &ctx,
                                   sms_remaining_ms(start_us, timeout_ms));
    sms_op_end();
    if (err == ESP_OK) {
        sms_mark_sent();
    }
    return err;
}

/* ------------------------------- delete --------------------------------- */

typedef struct {
    const int *indexes;
    size_t count;
} sms_del_ctx_t;

static esp_err_t sms_del_body(const cpe_at_io_t *io, void *arg)
{
    sms_del_ctx_t *ctx = arg;
    char resp[64];
    for (size_t i = 0; i < ctx->count; i++) {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", ctx->indexes[i]);
        if (io->send_line(cmd) != ESP_OK) {
            return ESP_FAIL;
        }
        esp_err_t r = io->read_until(NULL, 0, CPE_SMS_DELETE_TIMEOUT_MS, resp, sizeof(resp));
        if (r != ESP_OK) {
            return r;
        }
        if (strstr(resp, "ERROR") != NULL) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t cpe_sms_delete_many(const int *indexes, size_t count, uint32_t timeout_ms)
{
    if (indexes == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; i++) {
        if (indexes[i] < 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (sms_get_state() != CPE_SMS_READY) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    int64_t start_us = esp_timer_get_time();
    if (!sms_op_begin(timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }
    sms_del_ctx_t ctx = { .indexes = indexes, .count = count };
    esp_err_t err = cpe_at_session(sms_del_body, &ctx,
                                   sms_remaining_ms(start_us, timeout_ms));
    sms_op_end();
    return err;
}

/* ------------------------------ maintenance ----------------------------- */

/* Keep reception alive when the inbox is full: the modem rejects new messages
 * once <mem3> has no free slot and the SMSC only retries for the validity
 * period. Free one complete old read message (all of its parts) so the next
 * retry lands. Unread messages are never touched. */
static esp_err_t sms_prune_body(const cpe_at_io_t *io, void *arg)
{
    (void)arg;
    int used = -1;
    int total = -1;
    if (!sms_cpms_query(io, &used, &total) || used < 0 || total <= 0) {
        return ESP_FAIL;
    }
    if (used < total) {
        return ESP_OK;
    }

    char *raw = malloc(CPE_SMS_RAW_CAP);
    cpe_sms_list_t *list = calloc(1, sizeof(*list));
    if (raw == NULL || list == NULL) {
        free(raw);
        free(list);
        return ESP_ERR_NO_MEM;
    }

    sms_list_ctx_t ctx = { .out = list, .raw = raw, .cap = CPE_SMS_RAW_CAP };
    esp_err_t err = sms_fetch(io, &ctx);
    if (err != ESP_OK) {
        free(raw);
        free(list);
        return err;
    }

    int oldest = -1;
    for (size_t i = 0; i < list->count; i++) {
        cpe_sms_msg_t *m = &list->items[i];
        if (m->stat == CPE_SMS_STAT_UNREAD) {
            continue;
        }
        if (oldest < 0) {
            oldest = (int)i;
            continue;
        }
        const char *when = m->time;
        const char *prev = list->items[oldest].time;
        if (when[0] != '\0' && (prev[0] == '\0' || strcmp(when, prev) < 0)) {
            oldest = (int)i;
        }
    }

    if (oldest < 0) {
        ESP_LOGW(TAG, "SMS storage full (%d/%d) but every message is unread", used, total);
        free(raw);
        free(list);
        return ESP_OK;
    }

    cpe_sms_msg_t *victim = &list->items[oldest];
    sms_del_ctx_t del = { .indexes = victim->indexes, .count = victim->index_count };
    err = sms_del_body(io, &del);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to delete SMS index %d", victim->index);
        free(raw);
        free(list);
        return err;
    }
    ESP_LOGI(TAG, "SMS storage full (%d/%d), deleted oldest read message (%u part(s))",
             used, total, (unsigned)victim->index_count);

    free(raw);
    free(list);
    return ESP_OK;
}

/* Returns false when the AT session is busy; the task retries on its next
 * tick. */
static bool sms_maintain(void)
{
    if (!sms_op_begin(0)) {
        return false;
    }
    (void)cpe_at_session(sms_prune_body, NULL, CPE_SMS_LIST_TIMEOUT_MS * 3);
    sms_op_end();
    return true;
}

/* -------------------------------- task ---------------------------------- */

/* New-message indications name the storage slot only; the body is fetched by
 * the regular list operation. */
static void sms_urc_new_message(const char *line, void *ctx)
{
    (void)ctx;
    int index = 0;
    if (cpe_sms_parse_cmti(line, &index)) {
        ESP_LOGI(TAG, "New SMS at storage index %d", index);
    }
}

static const cpe_at_urc_subscription_t s_urc_sub = {
    .prefixes = { "+CMTI:" },
    .prefix_count = 1,
    .handler = sms_urc_new_message,
    .ctx = NULL,
};

static void sms_task(void *arg)
{
    (void)arg;
    bool was_ready = false;
    int64_t last_maint_us = 0;
    int64_t last_probe_us = 0;
    for (;;) {
        bool ready = cpe_at_ready();
        int64_t now = esp_timer_get_time();
        if (!ready) {
            sms_set_state(CPE_SMS_UNSUPPORTED);
            last_probe_us = 0;
        } else if (!was_ready) {
            last_probe_us = sms_probe() ? now : 0;
            last_maint_us = 0;
        } else if (sms_get_state() != CPE_SMS_READY &&
                   (last_probe_us == 0 ||
                    now - last_probe_us >= (int64_t)CPE_SMS_REPROBE_MS * 1000)) {
            /* A rejection while the modem is still booting must not keep SMS
             * disabled until the next replug. */
            if (sms_probe()) {
                last_probe_us = now;
                if (sms_get_state() == CPE_SMS_READY) {
                    last_maint_us = 0;
                }
            }
        }
        if (ready && sms_get_state() == CPE_SMS_READY) {
            int64_t t = esp_timer_get_time();
            if (last_maint_us == 0 ||
                t - last_maint_us >= (int64_t)CPE_SMS_MAINT_INTERVAL_MS * 1000) {
                if (sms_maintain()) {
                    last_maint_us = t;
                }
            }
        }
        was_ready = ready;
        vTaskDelay(pdMS_TO_TICKS(CPE_SMS_PROBE_POLL_MS));
    }
}

esp_err_t cpe_sms_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_op_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL || s_op_lock == NULL) {
        if (s_lock != NULL) {
            vSemaphoreDelete(s_lock);
            s_lock = NULL;
        }
        if (s_op_lock != NULL) {
            vSemaphoreDelete(s_op_lock);
            s_op_lock = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(sms_task, "cpe_sms", 3072, NULL, 3, &s_task) != pdPASS) {
        vSemaphoreDelete(s_lock);
        vSemaphoreDelete(s_op_lock);
        s_lock = NULL;
        s_op_lock = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t urc_err = cpe_at_urc_subscribe(&s_urc_sub);
    if (urc_err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot subscribe to +CMTI (%s)", esp_err_to_name(urc_err));
    }
    return ESP_OK;
}

cpe_sms_state_t cpe_sms_state(void)
{
    return sms_get_state();
}

#else /* !(CONFIG_CPE_SMS_ENABLE && CONFIG_CPE_AT_ENABLE) */

esp_err_t cpe_sms_init(void) { return ESP_OK; }
cpe_sms_state_t cpe_sms_state(void) { return CPE_SMS_UNSUPPORTED; }
esp_err_t cpe_sms_list(cpe_sms_list_t *out, uint32_t timeout_ms)
{ (void)out; (void)timeout_ms; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t cpe_sms_send(const char *number, const char *text, const char **err_str, uint32_t timeout_ms)
{ (void)number; (void)text; (void)err_str; (void)timeout_ms; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t cpe_sms_delete_many(const int *indexes, size_t count, uint32_t timeout_ms)
{ (void)indexes; (void)count; (void)timeout_ms; return ESP_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_CPE_SMS_ENABLE */
