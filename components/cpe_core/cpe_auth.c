/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "cpe_auth.h"
#include "cpe_config.h"

static const char *TAG = "cpe_auth";

#define CPE_MAX_SESSIONS            4
#define CPE_SESSION_IDLE_US         (30LL * 60LL * 1000LL * 1000LL)
#define CPE_MAX_FAILURES            5
#define CPE_LOCKOUT_BASE_US         (60LL * 1000LL * 1000LL)
#define CPE_LOCKOUT_MAX_US          (600LL * 1000LL * 1000LL)
#define CPE_LOCKOUT_MAX_LEVEL       3

typedef struct {
    bool used;
    char token[CPE_SESSION_TOKEN_LEN];
    char csrf[CPE_CSRF_TOKEN_LEN];
    int64_t last_used_us;
} cpe_session_t;

static cpe_session_t s_sessions[CPE_MAX_SESSIONS];
static SemaphoreHandle_t s_lock;
static bool s_inited;
static int s_fail_count;
static int64_t s_lock_until_us;
static int s_lockout_level;

static void random_token(char *out, size_t out_len)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < 16 && (i * 2 + 1) < out_len; i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 0x0f];
    }
    out[out_len - 1] = '\0';
}

static bool token_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la != lb || la == 0) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < la; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/* Must be called with s_lock held. */
static cpe_session_t *find_session(const char *token)
{
    if (token == NULL || token[0] == '\0') {
        return NULL;
    }
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < CPE_MAX_SESSIONS; i++) {
        cpe_session_t *s = &s_sessions[i];
        if (!s->used) {
            continue;
        }
        if (now - s->last_used_us > CPE_SESSION_IDLE_US) {
            memset(s, 0, sizeof(*s));
            continue;
        }
        if (token_equal(s->token, token)) {
            return s;
        }
    }
    return NULL;
}

esp_err_t cpe_auth_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_sessions, 0, sizeof(s_sessions));
    s_fail_count = 0;
    s_lock_until_us = 0;
    s_lockout_level = 0;
    s_inited = true;
    ESP_LOGI(TAG, "Auth initialized (session + CSRF, PBKDF2 password)");
    return ESP_OK;
}

cpe_auth_result_t cpe_auth_login(const char *username, const char *password,
                                 char *session, size_t session_len,
                                 char *csrf, size_t csrf_len,
                                 bool *must_change)
{
    if (session == NULL || session_len < CPE_SESSION_TOKEN_LEN ||
            csrf == NULL || csrf_len < CPE_CSRF_TOKEN_LEN) {
        return CPE_AUTH_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return CPE_AUTH_ERR_INVALID_ARG;
    }

    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_lock_until_us > now) {
        xSemaphoreGive(s_lock);
        return CPE_AUTH_ERR_LOCKED;
    }

    cpe_config_t cfg;
    if (cpe_config_get(&cfg) != ESP_OK) {
        xSemaphoreGive(s_lock);
        return CPE_AUTH_ERR_CREDENTIALS;
    }

    bool ok = (username != NULL) && token_equal(username, cfg.admin_user);
    if (ok) {
        ok = cpe_config_check_admin_password(password);
    }

    if (!ok) {
        s_fail_count++;
        if (s_fail_count >= CPE_MAX_FAILURES) {
            s_fail_count = 0;
            /* Exponential backoff: 60s, 120s, 240s, then 600s cap. This makes
             * repeated guessing progressively more expensive while a normal
             * user only ever sees the first 60s lockout. */
            int64_t lockout = CPE_LOCKOUT_BASE_US << s_lockout_level;
            if (lockout > CPE_LOCKOUT_MAX_US) {
                lockout = CPE_LOCKOUT_MAX_US;
            }
            if (s_lockout_level < CPE_LOCKOUT_MAX_LEVEL) {
                s_lockout_level++;
            }
            s_lock_until_us = now + lockout;
            ESP_LOGW(TAG, "Too many failed logins, locked for %lld s",
                     (long long)(lockout / 1000000LL));
            xSemaphoreGive(s_lock);
            return CPE_AUTH_ERR_LOCKED;
        }
        xSemaphoreGive(s_lock);
        return CPE_AUTH_ERR_CREDENTIALS;
    }

    s_fail_count = 0;
    s_lockout_level = 0;

    /* Allocate an unused slot, or evict the least recently used one. */
    cpe_session_t *slot = NULL;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < CPE_MAX_SESSIONS; i++) {
        if (!s_sessions[i].used) {
            slot = &s_sessions[i];
            break;
        }
        if (s_sessions[i].last_used_us < oldest) {
            oldest = s_sessions[i].last_used_us;
            slot = &s_sessions[i];
        }
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    random_token(slot->token, CPE_SESSION_TOKEN_LEN);
    random_token(slot->csrf, CPE_CSRF_TOKEN_LEN);
    slot->last_used_us = now;

    snprintf(session, session_len, "%s", slot->token);
    snprintf(csrf, csrf_len, "%s", slot->csrf);
    if (must_change) {
        *must_change = cpe_config_admin_must_change();
    }

    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Login succeeded for user \"%s\"", cfg.admin_user);
    return CPE_AUTH_OK;
}

bool cpe_auth_session_valid(const char *session)
{
    if (!s_inited) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cpe_session_t *s = find_session(session);
    if (s != NULL) {
        s->last_used_us = esp_timer_get_time();
    }
    xSemaphoreGive(s_lock);
    return s != NULL;
}

bool cpe_auth_csrf_valid(const char *session, const char *csrf)
{
    if (!s_inited) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cpe_session_t *s = find_session(session);
    bool ok = (s != NULL) && token_equal(s->csrf, csrf);
    if (ok) {
        s->last_used_us = esp_timer_get_time();
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void cpe_auth_logout(const char *session)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cpe_session_t *s = find_session(session);
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
    }
    xSemaphoreGive(s_lock);
}

void cpe_auth_logout_all(void)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_sessions, 0, sizeof(s_sessions));
    xSemaphoreGive(s_lock);
}

esp_err_t cpe_auth_change_password(const char *old_password, const char *new_password)
{
    if (!s_inited || old_password == NULL || new_password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cpe_config_validate_wifi_password(new_password) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cpe_config_check_admin_password(old_password)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = cpe_config_set_admin_password(new_password, false);
    if (err == ESP_OK) {
        cpe_auth_logout_all();
        ESP_LOGI(TAG, "Admin password changed, all sessions invalidated");
    }
    return err;
}

bool cpe_auth_must_change(void)
{
    if (!s_inited) {
        return true;
    }
    return cpe_config_admin_must_change();
}

uint32_t cpe_auth_lockout_remaining_s(void)
{
    if (!s_inited) {
        return 0;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t remain = s_lock_until_us - now;
    xSemaphoreGive(s_lock);
    return remain > 0 ? (uint32_t)(remain / 1000000LL) : 0;
}
