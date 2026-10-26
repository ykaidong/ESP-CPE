/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Periodic maintenance: keeps a long-running CPE from silently degrading by
 * rebuilding the ECM interface and/or rebooting the device on a schedule.
 *
 * This is NOT the fault recovery path (that stays in cpe_wan/link_manager.c):
 * the actions here are planned, they do not consume the recovery budget and
 * they never run while an OTA upload or an unvalidated OTA image is pending.
 * A scheduled ECM rebuild is also postponed while the WAN is not online, so it
 * cannot wipe the budget of a recovery ladder that is already running.
 *
 * The periods are runtime configuration stored in NVS ("cpe_maint"), so the
 * management console can change them; the Kconfig values are only the defaults
 * used on first boot and after a factory reset.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "cpe_core.h"
#include "cpe_kv.h"
#include "cpe_ota.h"
#include "cpe_system.h"
#include "cpe_wan.h"

#include "cpe_maintenance.h"

static const char *TAG = "cpe_maint";

#define NVS_KEY_ECM_MIN     "ecm_min"
#define NVS_KEY_REBOOT_MIN  "rb_min"
#define NVS_KEY_RESET_MODEM "rst_modem"

/* 1 s tick: both periods are minutes, so this only bounds the trigger jitter. */
#define MAINT_TICK_MS       1000
/* How long a scheduled rebuild may take to be online again before it is
 * reported as failed. The link manager keeps recovering on its own. */
#define MAINT_ECM_SETTLE_MS 120000
/* Any planned action is deferred by this much while an OTA is in progress. */
#define MAINT_OTA_DEFER_MS  60000
/* Time the state page gets to report the pending reboot when the reboot helper
 * could not be scheduled, before the device is restarted anyway. */
#define MAINT_REBOOT_GRACE_MS 200

/* Stack and priority of the 1 s maintenance task. */
#define MAINT_TASK_STACK    3072
#define MAINT_TASK_PRIO     2

#define US_PER_MIN          60000000LL

typedef struct {
    cpe_maint_config_t cfg;
    int64_t next_ecm_us;      /* 0 = disabled */
    int64_t next_reboot_us;   /* 0 = disabled */
    int64_t ecm_started_us;   /* 0 = no rebuild waiting to settle */
    int64_t steady_since_us;  /* 0 = still in the boot phase */
    bool guard_checked;       /* the NVS guard was already cleared this boot */
    uint32_t sched_count;     /* consecutive scheduled reboots, 0 = healthy */
    bool frozen;              /* scheduled reboots stopped by the guard */
    /* Scheduled rebuilds since boot; the reboot counter is not kept here, the
     * authoritative one lives in NVS (cpe_maint_reboot_count_*). */
    uint32_t ecm_runs;
} maint_ctx_t;

/* Shared with the HTTP handlers. A spinlock is enough: only a few scalars are
 * copied and nothing blocks inside the critical section. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static cpe_maint_config_t s_cfg;
static bool s_cfg_ready;
static int64_t s_pub_next_ecm_us;
static int64_t s_pub_next_reboot_us;
static uint32_t s_pub_sched_count;
static bool s_pub_frozen;

static bool s_started;

static int64_t period_us(uint32_t minutes)
{
    return (int64_t)minutes * US_PER_MIN;
}

static bool config_equal(const cpe_maint_config_t *a, const cpe_maint_config_t *b)
{
    return a->ecm_period_min == b->ecm_period_min &&
           a->reboot_period_min == b->reboot_period_min &&
           a->reset_modem == b->reset_modem;
}

/* ------------------------------------------------------------------------- */
/* Persisted policy                                                          */
/* ------------------------------------------------------------------------- */

static void config_defaults(cpe_maint_config_t *cfg)
{
    cfg->ecm_period_min = CONFIG_CPE_MAINT_ECM_PERIOD_MIN;
    cfg->reboot_period_min = CONFIG_CPE_MAINT_REBOOT_PERIOD_MIN;
    cfg->reset_modem = CONFIG_CPE_MAINT_RESET_MODEM;
}

static void config_load(cpe_maint_config_t *cfg)
{
    config_defaults(cfg);

    cpe_kv_t kv;
    esp_err_t err = cpe_kv_open(&kv, CPE_KV_NS_MAINT, false);
    if (err != ESP_OK) {
        /* Never configured (or unreadable): the Kconfig defaults apply.
         * cpe_kv_open() already logged the unexpected cases. */
        return;
    }

    uint32_t value = 0;
    if (cpe_kv_get_u32(&kv, NVS_KEY_ECM_MIN, &value) == ESP_OK &&
            value <= CPE_MAINT_PERIOD_MAX_MIN) {
        cfg->ecm_period_min = value;
    }
    if (cpe_kv_get_u32(&kv, NVS_KEY_REBOOT_MIN, &value) == ESP_OK &&
            value <= CPE_MAINT_PERIOD_MAX_MIN) {
        cfg->reboot_period_min = value;
    }
    uint8_t flag = 0;
    if (cpe_kv_get_u8(&kv, NVS_KEY_RESET_MODEM, &flag) == ESP_OK) {
        cfg->reset_modem = (flag != 0);
    }
    cpe_kv_close(&kv);
}

static esp_err_t config_save(const cpe_maint_config_t *cfg)
{
    cpe_kv_t kv;
    esp_err_t err = cpe_kv_open(&kv, CPE_KV_NS_MAINT, true);
    if (err != ESP_OK) {
        return err;
    }
    err = cpe_kv_set_u32(&kv, NVS_KEY_ECM_MIN, cfg->ecm_period_min);
    if (err == ESP_OK) {
        err = cpe_kv_set_u32(&kv, NVS_KEY_REBOOT_MIN, cfg->reboot_period_min);
    }
    if (err == ESP_OK) {
        err = cpe_kv_set_u8(&kv, NVS_KEY_RESET_MODEM, cfg->reset_modem ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = cpe_kv_commit(&kv);
    }
    cpe_kv_close(&kv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist the maintenance policy: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t cpe_maint_config_get(cpe_maint_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_mux);
    if (s_cfg_ready) {
        *out = s_cfg;
    } else {
        config_defaults(out);
    }
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t cpe_maint_config_set(const cpe_maint_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->ecm_period_min > CPE_MAINT_PERIOD_MAX_MIN ||
            cfg->reboot_period_min > CPE_MAINT_PERIOD_MAX_MIN) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = config_save(cfg);
    if (err != ESP_OK) {
        return err;
    }

    /* Re-arm the published schedule as well, so the HTTP response and the next
     * status read already show the new deadline (the task re-arms its own copy
     * on the next tick). */
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    s_cfg = *cfg;
    s_cfg_ready = true;
    s_pub_next_ecm_us = cfg->ecm_period_min > 0 ? now + period_us(cfg->ecm_period_min) : 0;
    s_pub_next_reboot_us = cfg->reboot_period_min > 0 ? now + period_us(cfg->reboot_period_min) : 0;
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "Maintenance policy updated: ECM every %" PRIu32 " min, "
             "reboot every %" PRIu32 " min, reset modem: %s",
             cfg->ecm_period_min, cfg->reboot_period_min, cfg->reset_modem ? "yes" : "no");
    return ESP_OK;
}

esp_err_t cpe_maint_config_reset(void)
{
    cpe_kv_t kv;
    esp_err_t err = cpe_kv_open(&kv, CPE_KV_NS_MAINT, true);
    if (err == ESP_OK) {
        err = cpe_kv_erase_all(&kv);
        if (err == ESP_OK) {
            err = cpe_kv_commit(&kv);
        }
        cpe_kv_close(&kv);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to erase the maintenance policy: %s", esp_err_to_name(err));
        }
    }

    cpe_maint_config_t def;
    config_defaults(&def);
    portENTER_CRITICAL(&s_mux);
    s_cfg = def;
    s_cfg_ready = true;
    portEXIT_CRITICAL(&s_mux);
    return err;
}

esp_err_t cpe_maint_status_get(cpe_maint_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (s_cfg_ready) {
        out->config = s_cfg;
    } else {
        config_defaults(&out->config);
    }
    int64_t next_ecm = s_pub_next_ecm_us;
    int64_t next_reboot = s_pub_next_reboot_us;
    out->sched_reboot_count = s_pub_sched_count;
    out->reboot_frozen = s_pub_frozen;
    portEXIT_CRITICAL(&s_mux);

    out->next_ecm_in_s = -1;
    out->next_reboot_in_s = -1;
    if (next_ecm > 0) {
        int64_t left = (next_ecm - now) / 1000000LL;
        out->next_ecm_in_s = left > 0 ? left : 0;
    }
    if (next_reboot > 0) {
        int64_t left = (next_reboot - now) / 1000000LL;
        out->next_reboot_in_s = left > 0 ? left : 0;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Scheduling                                                                */
/* ------------------------------------------------------------------------- */

/* Publish the schedule and the reboot guard so the console shows what the task
 * is actually doing (called once per tick, cheap). */
static void publish(const maint_ctx_t *ctx)
{
    portENTER_CRITICAL(&s_mux);
    s_pub_next_ecm_us = ctx->next_ecm_us;
    s_pub_next_reboot_us = ctx->next_reboot_us;
    s_pub_sched_count = ctx->sched_count;
    s_pub_frozen = ctx->frozen;
    portEXIT_CRITICAL(&s_mux);
}

/* (Re)start both countdowns from "now": a period change takes effect
 * immediately instead of waiting for the previous deadline. */
static void schedule_from_config(maint_ctx_t *ctx, int64_t now)
{
    ctx->next_ecm_us = ctx->cfg.ecm_period_min > 0
                       ? now + period_us(ctx->cfg.ecm_period_min) : 0;
    ctx->next_reboot_us = ctx->cfg.reboot_period_min > 0
                          ? now + period_us(ctx->cfg.reboot_period_min) : 0;
}

/* An upload in progress, or an image that still has to be validated, must not
 * be interrupted: a reboot during PENDING_VERIFY makes the bootloader roll the
 * update back. */
static bool ota_busy(void)
{
    cpe_ota_info_t info;
    if (cpe_ota_get_info(&info) != ESP_OK) {
        return false;
    }
    return info.upload_in_progress || info.pending_verify;
}

/* Clear the scheduled-reboot guard once the device has been in a steady state
 * for CPE_HEALTHY_CLEAR_SECONDS. "Steady" means the link manager has left the
 * boot phase: an unreachable 4G link (no SIM, no modem) must NOT count as
 * unhealthy, otherwise a device without a SIM would stop its periodic reboot
 * after a few days. A firmware that crashes or reboots does not stay out of the
 * boot phase for long enough, so a crash loop keeps the counter and stops the
 * scheduled reboots. */
static void maint_track_health(maint_ctx_t *ctx, const cpe_status_t *st, int64_t now)
{
    if (st->state == CPE_STATE_BOOT) {
        ctx->steady_since_us = 0;
        return;
    }
    if (ctx->steady_since_us == 0) {
        ctx->steady_since_us = now;
        return;
    }
    if (ctx->guard_checked ||
            (now - ctx->steady_since_us) < (int64_t)CONFIG_CPE_HEALTHY_CLEAR_SECONDS * 1000000LL) {
        return;
    }

    ctx->guard_checked = true;
    if (ctx->sched_count != 0) {
        (void)cpe_maint_reboot_count_clear();
        ESP_LOGI(TAG, "Steady for %d s, scheduled-reboot guard cleared (was %" PRIu32 ")",
                 CONFIG_CPE_HEALTHY_CLEAR_SECONDS, ctx->sched_count);
        ctx->sched_count = 0;
        ctx->frozen = false;
    }
}

static void maint_check_ecm(maint_ctx_t *ctx, const cpe_status_t *st, int64_t now)
{
    if (ctx->ecm_started_us == 0) {
        return;
    }
    if (st->state == CPE_STATE_ONLINE) {
        ESP_LOGI(TAG, "Scheduled ECM rebuild finished, WAN online after %" PRIi64 " s",
                 (now - ctx->ecm_started_us) / 1000000LL);
        ctx->ecm_started_us = 0;
        return;
    }
    if ((now - ctx->ecm_started_us) >= (int64_t)MAINT_ECM_SETTLE_MS * 1000LL) {
        ESP_LOGW(TAG, "Scheduled ECM rebuild not online after %d s, "
                 "leaving the recovery to the link manager", MAINT_ECM_SETTLE_MS / 1000);
        ctx->ecm_started_us = 0;
    }
}

static void maint_ecm_rebuild(maint_ctx_t *ctx, const cpe_status_t *st, int64_t now)
{
    if (ota_busy()) {
        ctx->next_ecm_us = now + (int64_t)MAINT_OTA_DEFER_MS * 1000LL;
        ESP_LOGI(TAG, "Scheduled ECM rebuild deferred: OTA in progress or image pending verify");
        return;
    }
    if (ctx->ecm_started_us != 0) {
        /* The previous rebuild has not settled yet. */
        ctx->next_ecm_us = now + period_us(ctx->cfg.ecm_period_min);
        return;
    }
    /* The link manager owns fault recovery, and its scheduled-rebuild path
     * zeroes every recovery counter. Rebuilding now would wipe the budget of a
     * ladder that is already running, so wait for a healthy link: refreshing an
     * interface that has no WAN anyway buys nothing. */
    if (st->state != CPE_STATE_ONLINE) {
        ctx->next_ecm_us = now + period_us(ctx->cfg.ecm_period_min);
        ESP_LOGI(TAG, "Scheduled ECM rebuild postponed: WAN not online (%s)",
                 st->state_text);
        return;
    }

    ctx->ecm_started_us = now;
    ctx->next_ecm_us = now + period_us(ctx->cfg.ecm_period_min);
    ctx->ecm_runs++;
    ESP_LOGI(TAG, "Scheduled ECM rebuild #%" PRIu32 " (every %" PRIu32 " min, uptime %" PRIi64 " s)",
             ctx->ecm_runs, ctx->cfg.ecm_period_min, now / 1000000LL);
    cpe_wan_request_ecm_rebuild();
}

/* Returns only when the reboot was deferred or frozen. */
static void maint_reboot(maint_ctx_t *ctx, int64_t now)
{
    if (ota_busy()) {
        ctx->next_reboot_us = now + (int64_t)MAINT_OTA_DEFER_MS * 1000LL;
        ESP_LOGI(TAG, "Scheduled reboot deferred: OTA in progress or image pending verify");
        return;
    }

    uint32_t count = 0;
    if (cpe_maint_reboot_count_get(&count) != ESP_OK) {
        count = ctx->sched_count;
    }
    if (count >= CONFIG_CPE_MAINT_MAX_SCHED_REBOOTS) {
        ESP_LOGE(TAG, "Scheduled reboot frozen: %" PRIu32 " consecutive reboots without a "
                 "healthy period (limit %d), keeping the device running",
                 count, CONFIG_CPE_MAINT_MAX_SCHED_REBOOTS);
        cpe_status_set_error("scheduled reboot frozen (no healthy period)");
        ctx->frozen = true;
        ctx->sched_count = count;
        ctx->next_reboot_us = now + period_us(ctx->cfg.reboot_period_min);
        return;
    }

    uint32_t next = 0;
    (void)cpe_maint_reboot_count_increment(&next);
    ctx->sched_count = next;
    ctx->frozen = false;
    ESP_LOGI(TAG, "Scheduled reboot #%" PRIu32 " (every %" PRIu32 " min, uptime %" PRIi64 " s, "
             "reset modem: %s)",
             next, ctx->cfg.reboot_period_min, now / 1000000LL,
             ctx->cfg.reset_modem ? "yes" : "no");
    /* The shared helper waits for the log line and the NVS commit to settle
     * before restarting, without blocking the maintenance task. */
    if (cpe_wan_reboot_deferred(ctx->cfg.reset_modem, "scheduled reboot") != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(MAINT_REBOOT_GRACE_MS));
        cpe_wan_reboot(ctx->cfg.reset_modem);
    }
}

static void maintenance_task(void *arg)
{
    (void)arg;
    maint_ctx_t ctx = { 0 };
    int64_t now = esp_timer_get_time();

    cpe_maint_config_get(&ctx.cfg);
    schedule_from_config(&ctx, now);

    uint32_t count = 0;
    if (cpe_maint_reboot_count_get(&count) == ESP_OK) {
        ctx.sched_count = count;
    }
    publish(&ctx);

    ESP_LOGI(TAG, "Periodic maintenance: ECM rebuild every %" PRIu32 " min, "
             "reboot every %" PRIu32 " min (0 = disabled)",
             ctx.cfg.ecm_period_min, ctx.cfg.reboot_period_min);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MAINT_TICK_MS));
        now = esp_timer_get_time();

        /* The console can change the periods at any time; re-arm the countdown
         * when the policy changed so a new value takes effect immediately. */
        cpe_maint_config_t cfg;
        cpe_maint_config_get(&cfg);
        if (!config_equal(&cfg, &ctx.cfg)) {
            ctx.cfg = cfg;
            schedule_from_config(&ctx, now);
            ESP_LOGI(TAG, "Maintenance schedule updated: ECM every %" PRIu32 " min, "
                     "reboot every %" PRIu32 " min",
                     cfg.ecm_period_min, cfg.reboot_period_min);
        }

        cpe_status_t st;
        cpe_status_get(&st);

        maint_track_health(&ctx, &st, now);
        maint_check_ecm(&ctx, &st, now);

        /* A reboot covers a due ECM rebuild as well, so it wins. */
        if (ctx.next_reboot_us != 0 && now >= ctx.next_reboot_us) {
            maint_reboot(&ctx, now);
        } else if (ctx.next_ecm_us != 0 && now >= ctx.next_ecm_us) {
            maint_ecm_rebuild(&ctx, &st, now);
        }

        publish(&ctx);
    }
}

/* The maintenance policy is not part of the security configuration, but a
 * factory reset must return every user visible setting to its default. Both
 * reset paths (web console and the physical button) run this hook. */
static void factory_reset_hook(void)
{
    (void)cpe_maint_config_reset();
}

esp_err_t cpe_maintenance_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* Load the persisted policy once (falls back to the Kconfig defaults) and
     * publish it before the task starts. */
    cpe_maint_config_t cfg;
    config_load(&cfg);
    portENTER_CRITICAL(&s_mux);
    s_cfg = cfg;
    s_cfg_ready = true;
    portEXIT_CRITICAL(&s_mux);

    if (xTaskCreate(maintenance_task, "cpe_maint",
                    MAINT_TASK_STACK, NULL, MAINT_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the maintenance task");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;

    esp_err_t hook_err = cpe_system_register_factory_reset_hook(factory_reset_hook,
                                                               "maintenance");
    if (hook_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register the factory-reset hook: %s",
                 esp_err_to_name(hook_err));
    }

    if (cfg.ecm_period_min == 0 && cfg.reboot_period_min == 0) {
        ESP_LOGI(TAG, "Periodic maintenance is disabled (both periods are 0), "
                 "it can be enabled from the web console");
    }
    return ESP_OK;
}
