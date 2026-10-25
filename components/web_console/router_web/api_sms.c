/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SMS endpoints. Listing and sending can take seconds (CMGL up to 20 s, CMGS
 * up to 60 s), so the AT work runs on a helper task with an async copy of the
 * request - the single HTTP server task must never be blocked.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cpe_sms.h"
#include "http_internal.h"

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
    int index;
} sms_delete_job_t;

/* --------------------------------- list ---------------------------------- */

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
        cpe_web_send_error(req, "out_of_memory");
        httpd_req_async_handler_complete(req);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = cpe_sms_list(list, SMS_LIST_TIMEOUT_MS);
    if (err != ESP_OK) {
        free(list);
        cpe_web_send_error(req, err == ESP_ERR_NOT_SUPPORTED ? "sms_not_supported" : "internal_error");
        httpd_req_async_handler_complete(req);
        vTaskDelete(NULL);
        return;
    }

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 512);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"supported\":true,\"messages\":[");
    for (size_t i = 0; i < list->count; i++) {
        cpe_sms_msg_t *m = &list->items[i];
        if (i > 0) {
            cpe_web_sbuf_append(&sb, ",");
        }
        cpe_web_sbuf_appendf(&sb, "{\"index\":%d,\"unread\":%s,\"from\":",
                             m->index, cpe_sms_stat_unread(m->stat) ? "true" : "false");
        cpe_web_sbuf_append_json_string(&sb, m->from);
        cpe_web_sbuf_append(&sb, ",\"time\":");
        cpe_web_sbuf_append_json_string(&sb, m->time);
        cpe_web_sbuf_append(&sb, ",\"body\":");
        cpe_web_sbuf_append_json_string(&sb, m->body);
        cpe_web_sbuf_append(&sb, "}");
    }
    cpe_web_sbuf_appendf(&sb, "],\"truncated\":%s}", list->truncated ? "true" : "false");
    free(list);

    cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
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
    bool ascii = false;
    if (!cpe_sms_text_measure(text, &ascii, NULL)) {
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
    esp_err_t err = cpe_sms_delete(job->index, SMS_DELETE_TIMEOUT_MS);

    if (err == ESP_OK) {
        cpe_web_sbuf_t sb;
        cpe_web_sbuf_init(&sb, 64);
        cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"sms_deleted\"}");
        cpe_web_send_sbuf_json(job->req, HTTPD_200, &sb, false);
    } else {
        cpe_web_send_error(job->req, err == ESP_ERR_NOT_SUPPORTED ? "sms_not_supported" : "internal_error");
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

    int index = -1;
    bool present = false;
    err = cpe_web_json_get_int_flexible(ctx.parser, "index", &index, &present);
    cpe_web_post_end(&ctx);
    if (err != ESP_OK || !present || index < 0) {
        return cpe_web_send_error(req, "invalid_request_body");
    }
    if (cpe_sms_state() != CPE_SMS_READY) {
        return cpe_web_send_error(req, "sms_not_supported");
    }

    sms_delete_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return cpe_web_send_error(req, "out_of_memory");
    }
    job->index = index;

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
