/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 4G link manager: keeps the WAN alive with bounded recovery:
 * L1 rebuild ECM driver -> L2 root port power cycle -> L3 controlled reboot
 * (NVS boot counter, cleared once healthy).
 * "Link up but no IP" (no SIM / PDP not active) never reboots: the device stays
 * in fault mode with AP/Web, and its ECM rebuilds are bounded by
 * CONFIG_CPE_IP_MAX_ECM_REBUILDS, after which only the DHCP client is restarted,
 * which still picks the link up when the SIM/PDP shows up later.
 * "No modem enumerated" never reboots either: it power-cycles the root port at
 * most CONFIG_CPE_USB_ENUM_RESET_ATTEMPTS times per round and then retries,
 * because a modem may be plugged in at any time.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_wifi.h"
#include "cpe_at.h"
#include "cpe_core.h"

#include "cpe_wan.h"
#include "wan_internal.h"

static const char *TAG = "cpe_link";

/* Back-off after a USB host or ECM driver install failure, and the pause after
 * a scheduled rebuild before the driver is created again. */
#define LM_INIT_RETRY_MS        5000
#define LM_REBUILD_SETTLE_MS    500

/* Set by cpe_wan_request_ecm_rebuild() (any task) and consumed by the link
 * manager on its next tick. The ECM handles are not locked, so a scheduled
 * rebuild must never run in the caller's context. */
static volatile bool s_ecm_rebuild_req;

/* Heap watermark helper: free heap alone is not enough to spot a leak or
 * fragmentation, so also log the historical minimum and the largest internal
 * block. Only called on link transitions and once at boot. */
void cpe_wan_log_heap(const char *reason)
{
    ESP_LOGI(TAG, "heap (%s): free=%u min=%u largest_internal=%u",
             reason,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

/* Sleep in 500 ms slices, returning early if the 4G link comes up (e.g. the
 * user plugs the modem back in while the link manager is backing off). */
static void backoff_sleep_ms(uint32_t ms)
{
    uint32_t waited = 0;
    while (waited < ms) {
        cpe_status_t st;
        cpe_status_get(&st);
        if (st.link_up) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }
}

typedef enum {
    LM_PHASE_BOOT = 0,      /* USB host / ECM driver bring-up */
    LM_PHASE_USB_WAIT,      /* no link yet */
    LM_PHASE_LINK_WAIT,     /* link is up, no IP yet */
    LM_PHASE_ONLINE,        /* link up and IP acquired */
} lm_phase_t;

/* LM_TICK_ACTION means a recovery action already consumed the delay (the old
 * loop's "continue"), so the task must not add its usual 500 ms tick. */
typedef enum {
    LM_TICK_IDLE = 0,
    LM_TICK_ACTION,
} lm_tick_result_t;

typedef struct {
    lm_phase_t phase;
    /* USB and IP recovery are counted separately: a modem that stays linked
     * without an IP (no SIM / PDP inactive) must not push the USB fault path
     * towards a reboot once the link later drops. */
    uint32_t usb_attempts;
    uint32_t ip_attempts;
    uint32_t enum_reset_attempts;
    uint32_t dhcp_restarts;
    /* ECM rebuilds of the current "link up but no IP" episode: the expensive
     * step (it resets the modem USB interface) is bounded separately from the
     * cheap DHCP-client restarts. */
    uint32_t ecm_rebuilds;
    int64_t wait_start_us;
    int64_t online_start_us;
    bool was_online;
    bool last_link_up;
    bool last_got_ip;
} lm_ctx_t;

static const char *lm_phase_name(lm_phase_t phase)
{
    switch (phase) {
    case LM_PHASE_BOOT:
        return "boot";
    case LM_PHASE_USB_WAIT:
        return "usb_wait";
    case LM_PHASE_LINK_WAIT:
        return "link_wait";
    case LM_PHASE_ONLINE:
        return "online";
    default:
        return "unknown";
    }
}

static void lm_enter_phase(lm_ctx_t *ctx, lm_phase_t phase)
{
    if (ctx->phase != phase) {
        ESP_LOGD(TAG, "Link manager phase: %s -> %s",
                 lm_phase_name(ctx->phase), lm_phase_name(phase));
        ctx->phase = phase;
    }
}

/* Every ECM teardown drops the link, so the wait window measured on it is stale
 * from that moment on: the two always go together. */
static void lm_teardown_ecm(lm_ctx_t *ctx)
{
    /* A failed uninstall leaves the driver instance behind; wan_ecm_bring_up()
     * finishes it on the next tick. Say so, otherwise a rebuild that never
     * really happened looks like a successful one in the log. */
    if (wan_ecm_teardown() != ESP_OK) {
        ESP_LOGW(TAG, "ECM teardown incomplete, the driver is uninstalled on the next attempt");
    }
    ctx->wait_start_us = 0;
}

/* Back to the BOOT phase and give the modem / CDC stack a moment before the
 * next tick creates the driver again. */
static void lm_backoff_to_boot(lm_ctx_t *ctx, uint32_t ms)
{
    lm_enter_phase(ctx, LM_PHASE_BOOT);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* USB host + CDC driver (installed once), then the ECM driver and netif. */
static bool lm_ensure_ready(lm_ctx_t *ctx)
{
    if (wan_usb_cdc_install() != ESP_OK) {
        cpe_status_set_state(CPE_STATE_FAULT, "usb host init failed, retrying");
        cpe_status_set_error("usb host init failed");
        lm_backoff_to_boot(ctx, LM_INIT_RETRY_MS);
        return false;
    }

    if (wan_ecm_bring_up() != ESP_OK) {
        cpe_status_set_state(CPE_STATE_FAULT, "ecm driver init failed, retrying");
        cpe_status_set_error("ecm driver init failed");
        lm_backoff_to_boot(ctx, LM_INIT_RETRY_MS);
        return false;
    }
    return true;
}

static void lm_service(lm_ctx_t *ctx, const cpe_status_t *st, int64_t now)
{
    if (st->link_up != ctx->last_link_up) {
        ctx->wait_start_us = now;
        ctx->last_link_up = st->link_up;
        if (st->link_up) {
            /* A fresh link starts a fresh recovery budget: the USB fault is
             * over and the DHCP client gets a clean set of retries. The ECM
             * rebuild budget is deliberately NOT reset here: a rebuild tears
             * the driver down, which drops this very link, so resetting it on
             * the next rising edge would hand out a new budget after every
             * rebuild and the limit would never be reached. It is cleared when
             * an IP actually arrives (below), on entering ONLINE and by a
             * scheduled rebuild. */
            ctx->usb_attempts = 0;
            ctx->ip_attempts = 0;
        }
        cpe_wan_log_heap(st->link_up ? "link up" : "link down");
    }
    if (st->got_ip != ctx->last_got_ip) {
        ctx->wait_start_us = now;
        ctx->last_got_ip = st->got_ip;
        if (st->got_ip) {
            ctx->ip_attempts = 0;
            ctx->dhcp_restarts = 0;
            ctx->ecm_rebuilds = 0;
            wan_link_online();
        } else {
            wan_link_offline();
        }
        cpe_wan_log_heap(st->got_ip ? "got ip" : "lost ip");
    }
}

static lm_tick_result_t lm_online(lm_ctx_t *ctx, const cpe_status_t *st, int64_t now)
{
    lm_enter_phase(ctx, LM_PHASE_ONLINE);

    /* Healthy. Clear the persistent boot/recovery counter once stable. */
    if (!ctx->was_online) {
        ctx->was_online = true;
        ctx->usb_attempts = 0;
        ctx->ip_attempts = 0;
        ctx->enum_reset_attempts = 0;
        ctx->ecm_rebuilds = 0;
        ctx->online_start_us = now;
        cpe_status_set_recovery_attempts(0);
        cpe_status_set_recovery_stage(CPE_RECOVERY_NONE);
        cpe_status_set_state(CPE_STATE_ONLINE, "online");
        ESP_LOGI(TAG, "CPE online (WAN IP %s)", st->ip);
    }
    if (ctx->online_start_us != 0 &&
            (now - ctx->online_start_us) > (int64_t)CONFIG_CPE_HEALTHY_CLEAR_SECONDS * 1000000LL) {
        uint32_t boot_count = 0;
        if (cpe_boot_count_get(&boot_count) == ESP_OK && boot_count != 0) {
            cpe_boot_count_clear();
            /* Keep the reported status in sync: otherwise the web UI keeps
             * showing the boot count captured at start-up. */
            cpe_status_set_boot_count(0);
            ESP_LOGI(TAG, "Healthy for %d s, boot counter cleared", CONFIG_CPE_HEALTHY_CLEAR_SECONDS);
        }
        ctx->online_start_us = 0;
    }
    return LM_TICK_IDLE;
}

/* Link is up but the modem has not obtained an IP: usually no SIM / PDP not
 * activated. Keep the ECM/USB session and retry only the DHCP client (a
 * teardown resets the modem USB interface, which is exactly what to avoid). */
static lm_tick_result_t lm_recover_dhcp(lm_ctx_t *ctx, int64_t now)
{
    ctx->dhcp_restarts++;
    cpe_status_set_error("waiting for the modem DHCP server");
    cpe_status_set_state(CPE_STATE_LINK_WAIT, "waiting for IP (DHCP retry)");
    wan_ecm_dhcp_restart();
    ctx->wait_start_us = now;
    return LM_TICK_ACTION;
}

static lm_tick_result_t lm_rebuild_ecm(lm_ctx_t *ctx)
{
    ctx->dhcp_restarts = 0;
    ctx->ecm_rebuilds++;
    cpe_status_set_error("link up but no IP (SIM/PDP problem?)");
    cpe_status_set_state(CPE_STATE_FAULT, "no IP from modem (SIM/PDP?), retrying");
    ESP_LOGW(TAG, "DHCP client retries exhausted, rebuilding the ECM driver "
             "(rebuild %"PRIu32")", ctx->ecm_rebuilds);
    lm_teardown_ecm(ctx);
    lm_backoff_to_boot(ctx, CONFIG_CPE_RECOVERY_RETRY_MS);
    return LM_TICK_ACTION;
}

static lm_tick_result_t lm_wait_ip(lm_ctx_t *ctx, int64_t now)
{
    lm_enter_phase(ctx, LM_PHASE_LINK_WAIT);
    if (ctx->wait_start_us == 0) {
        ctx->wait_start_us = now;
    }
    if ((now - ctx->wait_start_us) <= (int64_t)CONFIG_CPE_IP_TIMEOUT_MS * 1000LL) {
        cpe_status_set_state(CPE_STATE_LINK_WAIT, "waiting for IP");
        return LM_TICK_IDLE;
    }

    ctx->ip_attempts++;
    cpe_status_set_recovery_attempts(ctx->ip_attempts);
    cpe_status_set_recovery_stage(CPE_RECOVERY_IP);
    ESP_LOGW(TAG, "Link up but no IP after %d ms (attempt %"PRIu32")",
             CONFIG_CPE_IP_TIMEOUT_MS, ctx->ip_attempts);

    if (CONFIG_CPE_IP_DHCP_RESTARTS > 0 &&
            ctx->dhcp_restarts < CONFIG_CPE_IP_DHCP_RESTARTS) {
        return lm_recover_dhcp(ctx, now);
    }
    if (CONFIG_CPE_IP_MAX_ECM_REBUILDS > 0 &&
            ctx->ecm_rebuilds >= CONFIG_CPE_IP_MAX_ECM_REBUILDS) {
        /* Bounded escalation: every ECM rebuild resets the modem USB interface
         * and never fixes a missing SIM or an inactive PDP context, so stop
         * tearing the driver down after the configured number of attempts.
         * Recovery does not stop, it degrades to the cheap step: the DHCP
         * client keeps retrying, which still picks the link up when the SIM or
         * the PDP context shows up later, and the SoftAP/Web console stays
         * available the whole time. */
        ESP_LOGW(TAG, "ECM rebuilds exhausted, retrying the DHCP client only "
                 "(AP/Web stay available)");
        cpe_status_set_error("no IP from modem (SIM/PDP problem?)");
        cpe_status_set_state(CPE_STATE_FAULT,
                             "no IP from modem (SIM/PDP?), DHCP retries only");
        ctx->dhcp_restarts = 0;
        return lm_recover_dhcp(ctx, now);
    }
    return lm_rebuild_ecm(ctx);
}

/* Nothing enumerated: the modem is absent OR its USB stack is stale (typical
 * when the ESP32 resets while an externally powered module keeps its power ->
 * CHECK_SHORT_DEV_DESC FAILED). ECM rebuilds cannot fix this, so try the reset
 * mechanisms right away. */
static bool lm_try_reenumerate(lm_ctx_t *ctx)
{
    if (ctx->enum_reset_attempts >= CONFIG_CPE_USB_ENUM_RESET_ATTEMPTS) {
        return false;
    }
    if (!wan_usb_modem_reset()) {
        return false;
    }
    ctx->enum_reset_attempts++;
    cpe_status_set_state(CPE_STATE_BOOT, "re-enumerating the USB modem");
    ctx->wait_start_us = 0;
    lm_enter_phase(ctx, LM_PHASE_BOOT);
    return true;
}

static lm_tick_result_t lm_wait_for_modem(lm_ctx_t *ctx)
{
    ESP_LOGW(TAG, "No USB modem detected, staying in fault mode (AP/Web available)");
    cpe_status_set_state(CPE_STATE_FAULT, "no USB modem, AP/Web still available");
    backoff_sleep_ms(CONFIG_CPE_RECOVERY_RETRY_MS);
    /* Retry the whole reset cycle later (a modem may be plugged in at any
     * time). */
    ctx->enum_reset_attempts = 0;
    ctx->wait_start_us = 0;
    return LM_TICK_ACTION;
}

/* Reboot the device after the standard pre-restart cleanup (see cpe_wan.h). */
void cpe_wan_reboot(bool reset_modem_first)
{
    if (reset_modem_first) {
        /* Reset the modem first so it re-enumerates cleanly after the ESP
         * boots; best effort, an AT port that is already wedged does not block
         * the restart. */
        esp_err_t modem_err = cpe_at_modem_reset(CPE_AT_MODEM_RESET_TIMEOUT_MS);
        if (modem_err != ESP_OK) {
            ESP_LOGW(TAG, "Modem reset did not succeed (%s)", esp_err_to_name(modem_err));
        }
    }
    app_wifi_deauth_all_stations();
    esp_restart();
}

/* One-shot task behind cpe_wan_reboot_deferred(): wait for the grace period,
 * then run the cleanup/restart outside the caller's task context. */
#define REBOOT_TASK_STACK       3072
#define REBOOT_TASK_PRIO        5
#define REBOOT_REASON_LEN       40

typedef struct {
    bool reset_modem_first;
    char reason[REBOOT_REASON_LEN];
} reboot_job_t;

/* Several sources (web console, maintenance, reset button) may ask for a
 * restart within the grace period: only the first request creates a task. */
static bool s_reboot_pending;

static void reboot_task(void *arg)
{
    reboot_job_t *job = arg;
    bool reset_modem_first = job->reset_modem_first;
    ESP_LOGW(TAG, "Restarting (%s)", job->reason);
    free(job);

    vTaskDelay(pdMS_TO_TICKS(CPE_WAN_REBOOT_GRACE_MS));
    cpe_wan_reboot(reset_modem_first);
    vTaskDelete(NULL);
}

esp_err_t cpe_wan_reboot_deferred(bool reset_modem_first, const char *reason)
{
    /* A restart that is already queued wins: reporting ESP_OK keeps the second
     * caller from falling back to an inline restart. */
    if (__atomic_exchange_n(&s_reboot_pending, true, __ATOMIC_ACQ_REL)) {
        return ESP_OK;
    }

    reboot_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        __atomic_store_n(&s_reboot_pending, false, __ATOMIC_RELEASE);
        return ESP_ERR_NO_MEM;
    }
    job->reset_modem_first = reset_modem_first;
    if (reason != NULL) {
        snprintf(job->reason, sizeof(job->reason), "%s", reason);
    }
    if (xTaskCreate(reboot_task, "cpe_reboot", REBOOT_TASK_STACK, job,
                    REBOOT_TASK_PRIO, NULL) != pdPASS) {
        free(job);
        __atomic_store_n(&s_reboot_pending, false, __ATOMIC_RELEASE);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* The modem is on the bus but the ECM link never came up: controlled reboot is
 * the last resort, bounded by the persistent boot counter. */
static void lm_controlled_restart(uint32_t boot_count)
{
    ESP_LOGE(TAG, "ECM link stuck, controlled reboot %"PRIu32"/%d",
             boot_count, CONFIG_CPE_MAX_REBOOTS);
    cpe_status_set_state(CPE_STATE_FAULT, "restarting to recover ECM link");
    vTaskDelay(pdMS_TO_TICKS(1000));
    cpe_wan_reboot(true);
}

static lm_tick_result_t lm_escalate_usb_fault(lm_ctx_t *ctx)
{
    if (ctx->usb_attempts >= CONFIG_CPE_MAX_RECOVERY_ATTEMPTS) {
        ctx->usb_attempts = 0;
        cpe_status_set_recovery_attempts(0);

        uint32_t boot_count = 0;
        (void)cpe_boot_count_get(&boot_count);
        if (CONFIG_CPE_MAX_REBOOTS > 0 && boot_count < CONFIG_CPE_MAX_REBOOTS) {
            lm_controlled_restart(boot_count);
        }
        ESP_LOGE(TAG, "ECM link stuck, keeping AP/Web alive (fault mode)");
        cpe_status_set_state(CPE_STATE_FAULT, "ECM link stuck, AP/Web still available");
        backoff_sleep_ms(CONFIG_CPE_RECOVERY_RETRY_MS);
    } else {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return LM_TICK_ACTION;
}

/* No USB link at all: module missing, enumeration failed or the module
 * dropped off the bus. */
static lm_tick_result_t lm_wait_usb(lm_ctx_t *ctx, int64_t now)
{
    lm_enter_phase(ctx, LM_PHASE_USB_WAIT);
    ctx->dhcp_restarts = 0;
    if (ctx->wait_start_us == 0) {
        ctx->wait_start_us = now;
    }
    if ((now - ctx->wait_start_us) <= (int64_t)CONFIG_CPE_LINK_TIMEOUT_MS * 1000LL) {
        cpe_status_set_state(CPE_STATE_USB_WAIT, "waiting for USB modem");
        return LM_TICK_IDLE;
    }

    ctx->usb_attempts++;
    cpe_status_set_recovery_attempts(ctx->usb_attempts);
    cpe_status_set_recovery_stage(CPE_RECOVERY_USB);
    cpe_status_set_error("no USB link from 4G modem");
    ESP_LOGW(TAG, "No USB link after %d ms (attempt %"PRIu32")",
             CONFIG_CPE_LINK_TIMEOUT_MS, ctx->usb_attempts);
    cpe_status_set_state(CPE_STATE_FAULT, "waiting for USB modem");
    lm_teardown_ecm(ctx);

    if (!wan_usb_device_present()) {
        if (lm_try_reenumerate(ctx)) {
            return LM_TICK_ACTION;
        }
        return lm_wait_for_modem(ctx);
    }
    return lm_escalate_usb_fault(ctx);
}

/* Periodic maintenance requested by cpe_maintenance: rebuild the ECM interface
 * once, without resetting the modem and without spending the recovery budget.
 * The netif/DHCP/link events bring the WAN back on their own; if the modem does
 * not re-link within CPE_LINK_TIMEOUT_MS the normal recovery policy takes over
 * from a clean slate. */
static lm_tick_result_t lm_scheduled_rebuild(lm_ctx_t *ctx)
{
    ESP_LOGW(TAG, "Scheduled ECM rebuild: recreating the WAN interface");

    ctx->usb_attempts = 0;
    ctx->ip_attempts = 0;
    ctx->enum_reset_attempts = 0;
    ctx->dhcp_restarts = 0;
    ctx->ecm_rebuilds = 0;
    ctx->was_online = false;
    cpe_status_set_recovery_attempts(0);
    cpe_status_set_recovery_stage(CPE_RECOVERY_NONE);

    lm_teardown_ecm(ctx);
    /* Set the state after the teardown cleared the link flags, so the web
     * console does not briefly show "online" with a dead interface. */
    cpe_status_set_state(CPE_STATE_BOOT, "scheduled ECM rebuild");
    /* Leave the modem/CDC state alone for a moment before the next tick
     * creates the driver again. */
    lm_backoff_to_boot(ctx, LM_REBUILD_SETTLE_MS);
    return LM_TICK_ACTION;
}

static lm_tick_result_t lm_tick(lm_ctx_t *ctx)
{
    if (__atomic_exchange_n(&s_ecm_rebuild_req, false, __ATOMIC_ACQ_REL)) {
        return lm_scheduled_rebuild(ctx);
    }

    if (!lm_ensure_ready(ctx)) {
        return LM_TICK_ACTION;
    }

    cpe_status_t st;
    cpe_status_get(&st);
    int64_t now = esp_timer_get_time();

    lm_service(ctx, &st, now);

    if (st.link_up && st.got_ip) {
        return lm_online(ctx, &st, now);
    }

    ctx->was_online = false;
    if (st.link_up) {
        return lm_wait_ip(ctx, now);
    }
    return lm_wait_usb(ctx, now);
}

static void link_manager_task(void *arg)
{
    (void)arg;
    lm_ctx_t ctx = { .phase = LM_PHASE_BOOT };

    cpe_status_set_state(CPE_STATE_BOOT, "starting usb modem support");

    for (;;) {
        if (lm_tick(&ctx) == LM_TICK_IDLE) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

esp_err_t wan_link_manager_start(void)
{
    BaseType_t created = xTaskCreate(link_manager_task, "cpe_link", 4096, NULL, 6, NULL);
    return created == pdPASS ? ESP_OK : ESP_FAIL;
}

void cpe_wan_request_ecm_rebuild(void)
{
    __atomic_store_n(&s_ecm_rebuild_req, true, __ATOMIC_RELEASE);
}
