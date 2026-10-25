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
#define CPE_SMS_RAW_CAP            16384
#define CPE_SMS_SEND_MIN_INTERVAL_MS 5000

static cpe_sms_state_t s_state = CPE_SMS_UNSUPPORTED;
static SemaphoreHandle_t s_lock;
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

static void sms_restore_gsm(const cpe_at_io_t *io)
{
    char resp[64];
    (void)io->send_line("AT+CSCS=\"GSM\"");
    (void)io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp));
}

/* ------------------------------ capability ------------------------------ */

static esp_err_t sms_probe_body(const cpe_at_io_t *io, void *arg)
{
    (void)arg;
    char resp[64];
    if (io->send_line("AT+CMGF=1") != ESP_OK) {
        return ESP_FAIL;
    }
    if (io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp)) != ESP_OK) {
        return ESP_FAIL;
    }
    return strstr(resp, "OK") != NULL ? ESP_OK : ESP_FAIL;
}

static void sms_probe(void)
{
    esp_err_t r = cpe_at_session(sms_probe_body, NULL, CPE_SMS_CMD_TIMEOUT_MS + 1000);
    sms_set_state(r == ESP_OK ? CPE_SMS_READY : CPE_SMS_UNSUPPORTED);
    ESP_LOGI(TAG, "SMS capability %s", r == ESP_OK ? "ready" : "unsupported");
}

/* -------------------------------- list ---------------------------------- */

typedef struct {
    char *raw;
    size_t cap;
} sms_list_ctx_t;

static esp_err_t sms_list_body(const cpe_at_io_t *io, void *arg)
{
    sms_list_ctx_t *ctx = arg;
    char resp[64];

    if (io->send_line("AT+CSCS=\"UCS2\"") != ESP_OK) {
        return ESP_FAIL;
    }
    (void)io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp));

    if (io->send_line("AT+CMGL=\"ALL\"") != ESP_OK) {
        sms_restore_gsm(io);
        return ESP_FAIL;
    }
    esp_err_t r = io->read_until(NULL, 0, CPE_SMS_LIST_TIMEOUT_MS, ctx->raw, ctx->cap);
    if (r == ESP_OK && strstr(ctx->raw, "ERROR") != NULL) {
        /* Some firmware wants the parameter in UCS2 too. */
        if (io->send_line("AT+CMGL=\"0041004C004C\"") != ESP_OK) {
            sms_restore_gsm(io);
            return ESP_FAIL;
        }
        r = io->read_until(NULL, 0, CPE_SMS_LIST_TIMEOUT_MS, ctx->raw, ctx->cap);
    }
    sms_restore_gsm(io);
    return r;
}

esp_err_t cpe_sms_list(cpe_sms_list_t *out, uint32_t timeout_ms)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sms_get_state() != CPE_SMS_READY) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    char *raw = malloc(CPE_SMS_RAW_CAP);
    if (raw == NULL) {
        return ESP_ERR_NO_MEM;
    }
    sms_list_ctx_t ctx = { .raw = raw, .cap = CPE_SMS_RAW_CAP };
    esp_err_t err = cpe_at_session(sms_list_body, &ctx, timeout_ms);
    if (err == ESP_OK && !cpe_sms_parse_cmgl(raw, out)) {
        err = ESP_FAIL;
    }
    free(raw);
    return err;
}

/* -------------------------------- send ---------------------------------- */

typedef struct {
    const char *number;
    const char *text;
    const char **err_str;
} sms_send_ctx_t;

static esp_err_t sms_send_body(const cpe_at_io_t *io, void *arg)
{
    sms_send_ctx_t *ctx = arg;
    char resp[128];

    bool ascii = false;
    if (!cpe_sms_text_measure(ctx->text, &ascii, NULL)) {
        if (ctx->err_str) *ctx->err_str = "sms_text_too_long";
        return ESP_ERR_INVALID_ARG;
    }

    char addr[160];
    if (ascii) {
        if (io->send_line("AT+CSCS=\"GSM\"") != ESP_OK) return ESP_FAIL;
        (void)io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp));
        if (io->send_line("AT+CSMP=17,167,0,0") != ESP_OK) return ESP_FAIL;
        (void)io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp));
        snprintf(addr, sizeof(addr), "%s", ctx->number);
    } else {
        if (io->send_line("AT+CSCS=\"UCS2\"") != ESP_OK) return ESP_FAIL;
        (void)io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp));
        if (io->send_line("AT+CSMP=17,167,0,8") != ESP_OK) return ESP_FAIL;
        (void)io->read_until(NULL, 0, CPE_SMS_CMD_TIMEOUT_MS, resp, sizeof(resp));
        char num_hex[160];
        if (cpe_sms_ucs2_encode(ctx->number, num_hex, sizeof(num_hex)) == 0) {
            if (ctx->err_str) *ctx->err_str = "sms_bad_number";
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(addr, sizeof(addr), "%s", num_hex);
    }

    char cmd[192];
    int n = snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", addr);
    if (n < 0 || n >= (int)sizeof(cmd)) return ESP_ERR_INVALID_SIZE;
    if (io->send_line(cmd) != ESP_OK) return ESP_FAIL;

    static const char *prompt[] = { ">" };
    if (io->read_until(prompt, 1, 8000, resp, sizeof(resp)) != ESP_OK) {
        uint8_t esc = 0x1B;
        (void)io->write(&esc, 1);
        sms_restore_gsm(io);
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
            uint8_t esc = 0x1B;
            (void)io->write(&esc, 1);
            sms_restore_gsm(io);
            if (ctx->err_str) *ctx->err_str = "sms_bad_text";
            return ESP_ERR_INVALID_ARG;
        }
        payload = (const uint8_t *)body_hex;
        payload_len = hl;
    }

    if (io->write(payload, payload_len) != ESP_OK) {
        uint8_t esc = 0x1B;
        (void)io->write(&esc, 1);
        sms_restore_gsm(io);
        return ESP_FAIL;
    }
    uint8_t ctrl_z = 0x1A;
    if (io->write(&ctrl_z, 1) != ESP_OK) {
        sms_restore_gsm(io);
        return ESP_FAIL;
    }

    static const char *result_tokens[] = { "+CMGS:" };
    esp_err_t r = io->read_until(result_tokens, 1, CPE_SMS_SEND_TIMEOUT_MS, resp, sizeof(resp));
    sms_restore_gsm(io);

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
    sms_send_ctx_t ctx = { .number = number, .text = text, .err_str = err_str };
    esp_err_t err = cpe_at_session(sms_send_body, &ctx, timeout_ms);
    if (err == ESP_OK) {
        sms_mark_sent();
    }
    return err;
}

/* ------------------------------- delete --------------------------------- */

typedef struct {
    int index;
} sms_del_ctx_t;

static esp_err_t sms_del_body(const cpe_at_io_t *io, void *arg)
{
    sms_del_ctx_t *ctx = arg;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", ctx->index);
    if (io->send_line(cmd) != ESP_OK) {
        return ESP_FAIL;
    }
    char resp[64];
    esp_err_t r = io->read_until(NULL, 0, CPE_SMS_DELETE_TIMEOUT_MS, resp, sizeof(resp));
    if (r != ESP_OK) {
        return r;
    }
    return strstr(resp, "ERROR") != NULL ? ESP_FAIL : ESP_OK;
}

esp_err_t cpe_sms_delete(int index, uint32_t timeout_ms)
{
    if (index < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sms_get_state() != CPE_SMS_READY) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    sms_del_ctx_t ctx = { .index = index };
    return cpe_at_session(sms_del_body, &ctx, timeout_ms);
}

/* -------------------------------- task ---------------------------------- */

static void sms_task(void *arg)
{
    (void)arg;
    bool was_ready = false;
    for (;;) {
        bool ready = cpe_at_ready();
        if (ready && !was_ready) {
            sms_probe();
        } else if (!ready) {
            sms_set_state(CPE_SMS_UNSUPPORTED);
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
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(sms_task, "cpe_sms", 3072, NULL, 3, &s_task) != pdPASS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
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
esp_err_t cpe_sms_delete(int index, uint32_t timeout_ms)
{ (void)index; (void)timeout_ms; return ESP_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_CPE_SMS_ENABLE */
