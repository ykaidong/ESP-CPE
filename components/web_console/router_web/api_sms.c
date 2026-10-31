/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SMS endpoints. Listing (CMGL in PDU mode) and sending can take seconds (CMGL
 * up to 20 s, CMGS up to 60 s), so the AT work runs on a helper task with an
 * async copy of the request - the single HTTP server task must never be
 * blocked.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cpe_sms.h"
#include "http_internal.h"

static const char *TAG = "web_sms";

#define SMS_LIST_TIMEOUT_MS   30000
#define SMS_SEND_TIMEOUT_MS   60000
#define SMS_DELETE_TIMEOUT_MS 10000

typedef struct {
    httpd_req_t *req;
    char number[32];
    char text[512];
} sms_send_job_t;

typedef struct {
    httpd_req_t *req;
    int indexes[CPE_SMS_MAX_PARTS];
    size_t count;
} sms_delete_job_t;

/* -------------------------------- notify --------------------------------- */

/* Cheap poll: no AT session, so the console can check for changes often and
 * only pay for a full listing when there is something new. */
esp_err_t api_sms_notify_get_handler(httpd_req_t *req)
{
    cpe_sms_state_t state = cpe_sms_state();
    /* "supported" only turns false once the modem answered the probe and
     * rejected it. A modem that is still booting or unplugged stays unknown, so
     * the console keeps the SMS entry instead of hiding it until a reload. */
    bool supported = state != CPE_SMS_UNSUPPORTED;
    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 64);
    cpe_web_sbuf_appendf(&sb,
                         "{\"status\":\"ok\",\"generation\":%u,\"ready\":%s,"
                         "\"supported\":%s}",
                         (unsigned)cpe_sms_inbox_generation(),
                         state == CPE_SMS_READY ? "true" : "false",
                         supported ? "true" : "false");
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

/* --------------------------------- list ---------------------------------- */

/* A busy AT port or a timeout says "try again", not "the device broke". */
static const char *sms_error_code(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_SUPPORTED:   return "sms_not_supported";
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_INVALID_STATE:   return "sms_busy";
    case ESP_ERR_NO_MEM:          return "out_of_memory";
    default:                      return "internal_error";
    }
}

/* Storage status as the UI needs it: unread styling and the direction of the
 * message (2 and 3 are stored submits, i.e. written from this device). */
static const char *sms_stat_name(cpe_sms_stat_t stat)
{
    switch (stat) {
    case CPE_SMS_STAT_UNREAD: return "unread";
    case CPE_SMS_STAT_READ:   return "read";
    case CPE_SMS_STAT_UNSENT: return "unsent";
    case CPE_SMS_STAT_SENT:   return "sent";
    default:                  return "unknown";
    }
}

/* Send the buffered JSON as one HTTP chunk and start over. */
static esp_err_t sms_list_flush(httpd_req_t *req, cpe_web_sbuf_t *sb)
{
    if (sb->failed) {
        return ESP_ERR_NO_MEM;
    }
    if (sb->len == 0) {
        return ESP_OK;
    }
    esp_err_t err = httpd_resp_send_chunk(req, sb->data, sb->len);
    sb->len = 0;
    return err;
}

static void sms_list_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    if (cpe_sms_state() != CPE_SMS_READY) {
        cpe_web_send_error(req, "sms_not_supported");
        httpd_req_async_handler_complete(req);
        vTaskDelete(NULL);
        return;
    }

    cpe_sms_list_t *list = calloc(1, sizeof(*list));
    if (list == NULL) {
        ESP_LOGE(TAG, "No memory for a %u byte SMS list (free=%u largest=%u)",
                 (unsigned)sizeof(*list), (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        cpe_web_send_error(req, "out_of_memory");
        httpd_req_async_handler_complete(req);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = cpe_sms_list(list, SMS_LIST_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SMS list failed: %s (free=%u largest=%u)", esp_err_to_name(err),
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        cpe_sms_list_clear(list);
        free(list);
        cpe_web_send_error(req, sms_error_code(err));
        httpd_req_async_handler_complete(req);
        vTaskDelete(NULL);
        return;
    }

    /* Stream the response in chunks of one message instead of building the
     * whole JSON first, so the peak stays a few KB on top of the decoded list. */
    esp_err_t send_err = httpd_resp_set_status(req, HTTPD_200);
    if (send_err == ESP_OK) {
        send_err = httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    }
    if (send_err == ESP_OK) {
        send_err = cpe_web_set_common_headers(req, true);
    }

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 1024);
    if (send_err == ESP_OK) {
        cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"storage\":{");
        if (list->storage_capacity > 0) {
            bool near_full = (int64_t)list->storage_used * 10 >=
                             (int64_t)list->storage_capacity * 9;
            cpe_web_sbuf_appendf(&sb,
                                 "\"used\":%d,\"capacity\":%d,\"near_full\":%s},",
                                 list->storage_used, list->storage_capacity,
                                 near_full ? "true" : "false");
        } else {
            cpe_web_sbuf_append(&sb, "\"used\":-1,\"capacity\":-1,\"near_full\":false},");
        }
        /* Only the first CPE_SMS_MAX_MESSAGES records are listed; the console
         * says so rather than pretending the inbox is complete. */
        cpe_web_sbuf_appendf(&sb, "\"truncated\":%s,\"messages\":[",
                             list->truncated ? "true" : "false");
    }

    for (size_t i = 0; send_err == ESP_OK && i < list->count; i++) {
        cpe_sms_msg_t *m = &list->items[i];
        if (i > 0) {
            cpe_web_sbuf_append(&sb, ",");
        }
        cpe_web_sbuf_appendf(&sb, "{\"index\":%d,\"stat\":\"%s\",\"truncated\":%s,\"indexes\":[",
                             m->index, sms_stat_name(m->stat),
                             m->truncated ? "true" : "false");
        for (uint8_t k = 0; k < m->index_count; k++) {
            cpe_web_sbuf_appendf(&sb, "%s%d", k > 0 ? "," : "", m->indexes[k]);
        }
        cpe_web_sbuf_append(&sb, "],\"from\":");
        cpe_web_sbuf_append_json_string(&sb, m->from);
        cpe_web_sbuf_append(&sb, ",\"time\":");
        cpe_web_sbuf_append_json_string(&sb, m->time);
        cpe_web_sbuf_append(&sb, ",\"body\":");
        cpe_web_sbuf_append_json_string(&sb, m->body);
        cpe_web_sbuf_append(&sb, "}");

        if (sb.failed) {
            send_err = ESP_ERR_NO_MEM;
        } else if (sb.len >= 512) {
            send_err = sms_list_flush(req, &sb);
        }
    }
    if (send_err == ESP_OK) {
        cpe_web_sbuf_append(&sb, "]}");
        send_err = sb.failed ? ESP_ERR_NO_MEM : sms_list_flush(req, &sb);
    }
    cpe_web_sbuf_free(&sb);
    cpe_sms_list_clear(list);
    free(list);

    if (send_err == ESP_OK) {
        (void)httpd_resp_send_chunk(req, NULL, 0);
    } else {
        /* The headers are already out, so no error body can follow: dropping
         * the connection is the only honest way to fail. */
        ESP_LOGW(TAG, "SMS list response aborted: %s (free=%u)",
                 esp_err_to_name(send_err), (unsigned)esp_get_free_heap_size());
    }
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

esp_err_t api_sms_list_get_handler(httpd_req_t *req)
{
    if (cpe_sms_state() != CPE_SMS_READY) {
        return cpe_web_send_error(req, "sms_not_supported");
    }
    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        return cpe_web_send_error(req, "internal_error");
    }
    if (xTaskCreate(sms_list_task, "sms_list", CPE_WEB_HELPER_TASK_STACK,
                    async_req, CPE_WEB_HELPER_TASK_PRIO, NULL) != pdPASS) {
        httpd_req_async_handler_complete(async_req);
        return cpe_web_send_error(req, "internal_error");
    }
    return ESP_OK;
}

/* --------------------------------- send ---------------------------------- */

static void sms_send_task(void *arg)
{
    sms_send_job_t *job = (sms_send_job_t *)arg;
    const char *err_code = NULL;
    esp_err_t err = cpe_sms_send(job->number, job->text, &err_code, SMS_SEND_TIMEOUT_MS);

    if (err == ESP_OK) {
        cpe_web_sbuf_t sb;
        cpe_web_sbuf_init(&sb, 64);
        cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"sms_sent\"}");
        cpe_web_send_sbuf_json(job->req, HTTPD_200, &sb, false);
    } else {
        const char *code = err_code != NULL ? err_code
                          : (err == ESP_ERR_NOT_SUPPORTED ? "sms_not_supported"
                                                          : "sms_send_failed");
        cpe_web_send_error(job->req, code);
    }

    httpd_req_async_handler_complete(job->req);
    free(job);
    vTaskDelete(NULL);
}

esp_err_t api_sms_send_post_handler(httpd_req_t *req)
{
    static const cpe_web_post_opts_t opts = {
        .session = CPE_WEB_POST_SESSION_MUTABLE,
        .body = CPE_WEB_POST_BODY_REQUIRED,
        .parse_json = true,
    };
    cpe_web_post_ctx_t ctx;
    esp_err_t err = cpe_web_post_begin(req, &opts, &ctx);
    if (err != ESP_OK) {
        return err;
    }

    char number[32];
    char text[512];
    err = cpe_web_json_get_string_unescaped(ctx.parser, "number", number, sizeof(number));
    if (err == ESP_OK) {
        err = cpe_web_json_get_string_unescaped(ctx.parser, "text", text, sizeof(text));
    }
    cpe_web_post_end(&ctx);
    if (err != ESP_OK) {
        return cpe_web_send_error(req, "invalid_request_body");
    }
    if (!cpe_sms_number_valid(number)) {
        return cpe_web_send_error(req, "sms_bad_number");
    }
    if (!cpe_sms_text_measure(text, NULL, NULL)) {
        return cpe_web_send_error(req, "sms_text_too_long");
    }
    if (cpe_sms_state() != CPE_SMS_READY) {
        return cpe_web_send_error(req, "sms_not_supported");
    }

    sms_send_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return cpe_web_send_error(req, "out_of_memory");
    }
    snprintf(job->number, sizeof(job->number), "%s", number);
    snprintf(job->text, sizeof(job->text), "%s", text);

    httpd_req_t *async_req = NULL;
    err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        free(job);
        return cpe_web_send_error(req, "internal_error");
    }
    job->req = async_req;
    if (xTaskCreate(sms_send_task, "sms_send", CPE_WEB_HELPER_TASK_STACK,
                    job, CPE_WEB_HELPER_TASK_PRIO, NULL) != pdPASS) {
        free(job);
        httpd_req_async_handler_complete(async_req);
        return cpe_web_send_error(req, "internal_error");
    }
    return ESP_OK;
}

/* -------------------------------- delete --------------------------------- */

static void sms_delete_task(void *arg)
{
    sms_delete_job_t *job = (sms_delete_job_t *)arg;
    esp_err_t err = cpe_sms_delete_many(job->indexes, job->count, SMS_DELETE_TIMEOUT_MS);

    if (err == ESP_OK) {
        cpe_web_sbuf_t sb;
        cpe_web_sbuf_init(&sb, 64);
        cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"sms_deleted\"}");
        cpe_web_send_sbuf_json(job->req, HTTPD_200, &sb, false);
    } else {
        cpe_web_send_error(job->req, sms_error_code(err));
    }

    httpd_req_async_handler_complete(job->req);
    free(job);
    vTaskDelete(NULL);
}

esp_err_t api_sms_delete_post_handler(httpd_req_t *req)
{
    static const cpe_web_post_opts_t opts = {
        .session = CPE_WEB_POST_SESSION_MUTABLE,
        .body = CPE_WEB_POST_BODY_REQUIRED,
        .parse_json = true,
    };
    cpe_web_post_ctx_t ctx;
    esp_err_t err = cpe_web_post_begin(req, &opts, &ctx);
    if (err != ESP_OK) {
        return err;
    }

    sms_delete_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        cpe_web_post_end(&ctx);
        return cpe_web_send_error(req, "out_of_memory");
    }

    const cJSON *indexes = cJSON_GetObjectItemCaseSensitive(ctx.parser, "indexes");
    err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsArray(indexes)) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, indexes) {
            if (!cJSON_IsNumber(it) || it->valueint < 0 ||
                job->count >= CPE_SMS_MAX_PARTS) {
                job->count = 0;
                break;
            }
            job->indexes[job->count++] = it->valueint;
        }
        if (job->count > 0) {
            err = ESP_OK;
        }
    }
    cpe_web_post_end(&ctx);
    if (err != ESP_OK) {
        free(job);
        return cpe_web_send_error(req, "invalid_request_body");
    }
    if (cpe_sms_state() != CPE_SMS_READY) {
        free(job);
        return cpe_web_send_error(req, "sms_not_supported");
    }

    httpd_req_t *async_req = NULL;
    err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        free(job);
        return cpe_web_send_error(req, "internal_error");
    }
    job->req = async_req;
    if (xTaskCreate(sms_delete_task, "sms_del", CPE_WEB_HELPER_TASK_STACK,
                    job, CPE_WEB_HELPER_TASK_PRIO, NULL) != pdPASS) {
        free(job);
        httpd_req_async_handler_complete(async_req);
        return cpe_web_send_error(req, "internal_error");
    }
    return ESP_OK;
}
