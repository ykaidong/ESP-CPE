/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * URC subscriptions and delivery. The AT task hands every complete line that
 * is not a result line to cpe_at_urc_dispatch(); each matching subscription
 * gets a private copy queued to the URC task. Handlers therefore run in
 * ordinary task context: they may block and may use the AT session API.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "cpe_at_urc.h"
#include "cpe_at_urc_internal.h"
#include "cpe_at_urc_match.h"

#if CONFIG_CPE_AT_ENABLE

static const char *TAG = "cpe_at_urc";

#define AT_URC_QUEUE_DEPTH 8
#define AT_URC_TASK_STACK  4096
#define AT_URC_TASK_PRIO   3

typedef struct {
    bool used;
    char prefixes[CPE_AT_URC_MAX_PREFIXES][CPE_AT_URC_PREFIX_MAX_LEN];
    size_t prefix_count;
    cpe_at_urc_handler_t handler;
    void *ctx;
    const cpe_at_urc_subscription_t *sub;
} at_urc_entry_t;

typedef struct {
    cpe_at_urc_handler_t handler;
    void *ctx;
    char *line;
} at_urc_event_t;

static at_urc_entry_t s_entries[CPE_AT_URC_MAX_SUBSCRIBERS];
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static uint32_t s_dropped;

static void at_urc_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void at_urc_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static void at_urc_task(void *arg)
{
    at_urc_event_t ev;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ev.handler(ev.line, ev.ctx);
        free(ev.line);
    }
}

esp_err_t cpe_at_urc_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_queue = xQueueCreate(AT_URC_QUEUE_DEPTH, sizeof(at_urc_event_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(at_urc_task, "cpe_urc", AT_URC_TASK_STACK, NULL,
                    AT_URC_TASK_PRIO, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t cpe_at_urc_subscribe(const cpe_at_urc_subscription_t *sub)
{
    if (sub == NULL || sub->handler == NULL || sub->prefix_count == 0 ||
        sub->prefix_count > CPE_AT_URC_MAX_PREFIXES) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sub->prefix_count; i++) {
        const char *p = sub->prefixes[i];
        if (p == NULL || p[0] == '\0' ||
            strnlen(p, CPE_AT_URC_PREFIX_MAX_LEN) >= CPE_AT_URC_PREFIX_MAX_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    at_urc_lock();
    int slot = -1;
    for (int i = 0; i < CPE_AT_URC_MAX_SUBSCRIBERS; i++) {
        if (s_entries[i].used && s_entries[i].sub == sub) {
            at_urc_unlock();
            return ESP_OK;
        }
        if (!s_entries[i].used && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        at_urc_unlock();
        return ESP_ERR_NO_MEM;
    }

    at_urc_entry_t *e = &s_entries[slot];
    for (size_t i = 0; i < sub->prefix_count; i++) {
        snprintf(e->prefixes[i], sizeof(e->prefixes[i]), "%s", sub->prefixes[i]);
    }
    e->prefix_count = sub->prefix_count;
    e->handler = sub->handler;
    e->ctx = sub->ctx;
    e->sub = sub;
    e->used = true;
    at_urc_unlock();
    return ESP_OK;
}

esp_err_t cpe_at_urc_unsubscribe(const cpe_at_urc_subscription_t *sub)
{
    if (sub == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    at_urc_lock();
    for (int i = 0; i < CPE_AT_URC_MAX_SUBSCRIBERS; i++) {
        if (s_entries[i].used && s_entries[i].sub == sub) {
            s_entries[i].used = false;
            err = ESP_OK;
        }
    }
    at_urc_unlock();
    return err;
}

static void at_urc_prefix_ptrs(const at_urc_entry_t *e,
                               const char *ptrs[CPE_AT_URC_MAX_PREFIXES])
{
    for (size_t i = 0; i < e->prefix_count; i++) {
        ptrs[i] = e->prefixes[i];
    }
}

bool cpe_at_urc_dispatch(const char *line, size_t len)
{
    cpe_at_urc_handler_t handlers[CPE_AT_URC_MAX_SUBSCRIBERS];
    void *contexts[CPE_AT_URC_MAX_SUBSCRIBERS];
    size_t matches = 0;

    if (line == NULL || s_queue == NULL) {
        return false;
    }

    at_urc_lock();
    for (int i = 0; i < CPE_AT_URC_MAX_SUBSCRIBERS; i++) {
        at_urc_entry_t *e = &s_entries[i];
        if (!e->used) {
            continue;
        }
        const char *prefixes[CPE_AT_URC_MAX_PREFIXES];
        at_urc_prefix_ptrs(e, prefixes);
        if (cpe_at_urc_line_matches(line, len, prefixes, e->prefix_count)) {
            handlers[matches] = e->handler;
            contexts[matches] = e->ctx;
            matches++;
        }
    }
    at_urc_unlock();

    for (size_t i = 0; i < matches; i++) {
        char *copy = malloc(len + 1);
        if (copy == NULL) {
            s_dropped++;
            continue;
        }
        memcpy(copy, line, len);
        copy[len] = '\0';
        at_urc_event_t ev = {
            .handler = handlers[i],
            .ctx = contexts[i],
            .line = copy,
        };
        if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
            free(copy);
            s_dropped++;
        }
    }
    if (s_dropped > 0 && (s_dropped == 1 || s_dropped % 100 == 0)) {
        ESP_LOGW(TAG, "Dropped %u URC(s): delivery queue full or out of memory",
                 (unsigned)s_dropped);
    }
    return matches > 0;
}

bool cpe_at_urc_partial_subscribed(const char *partial, size_t len)
{
    bool match = false;

    if (partial == NULL || s_queue == NULL) {
        return false;
    }

    at_urc_lock();
    for (int i = 0; i < CPE_AT_URC_MAX_SUBSCRIBERS && !match; i++) {
        at_urc_entry_t *e = &s_entries[i];
        if (!e->used) {
            continue;
        }
        const char *prefixes[CPE_AT_URC_MAX_PREFIXES];
        at_urc_prefix_ptrs(e, prefixes);
        match = cpe_at_urc_partial_matches(partial, len, prefixes, e->prefix_count);
    }
    at_urc_unlock();
    return match;
}

bool cpe_at_urc_has_subscribers(void)
{
    bool any = false;

    at_urc_lock();
    for (int i = 0; i < CPE_AT_URC_MAX_SUBSCRIBERS && !any; i++) {
        any = s_entries[i].used;
    }
    at_urc_unlock();
    return any;
}

#else /* CONFIG_CPE_AT_ENABLE */

esp_err_t cpe_at_urc_init(void)
{
    return ESP_OK;
}

esp_err_t cpe_at_urc_subscribe(const cpe_at_urc_subscription_t *sub)
{
    (void)sub;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cpe_at_urc_unsubscribe(const cpe_at_urc_subscription_t *sub)
{
    (void)sub;
    return ESP_ERR_NOT_SUPPORTED;
}

bool cpe_at_urc_dispatch(const char *line, size_t len)
{
    (void)line;
    (void)len;
    return false;
}

bool cpe_at_urc_partial_subscribed(const char *partial, size_t len)
{
    (void)partial;
    (void)len;
    return false;
}

bool cpe_at_urc_has_subscribers(void)
{
    return false;
}

#endif /* CONFIG_CPE_AT_ENABLE */
