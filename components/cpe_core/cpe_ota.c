/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Streaming OTA update helper (see cpe_ota.h).
 *
 * Only one upload can be in progress at a time: the upload task streams the
 * body chunk by chunk and any second request is rejected with
 * CPE_OTA_ERR_BUSY, so two clients can never write the same partition.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"

#include "cpe_ota.h"

static const char *TAG = "cpe_ota";

/* Upload state. begin/write/end/abort form one state machine and must run in
 * the same task (the HTTP server task); cpe_ota_get_info() is the only entry
 * point that may be called from another task. The flag it reports is published
 * under a critical section so the status handler never observes a torn state
 * transition. */
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_upload_in_progress;
static esp_ota_handle_t s_update_handle;
static const esp_partition_t *s_update_partition;
static size_t s_written;

static const char *ota_image_state_str(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending_verify";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default:
        return "undefined";
    }
}

esp_err_t cpe_ota_get_info(cpe_ota_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));

    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc != NULL) {
        snprintf(info->running_version, sizeof(info->running_version), "%s", app_desc->version);
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        snprintf(info->running_partition, sizeof(info->running_partition), "%s", running->label);
        info->partition_size = (size_t)running->size;
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
            info->pending_verify = (state == ESP_OTA_IMG_PENDING_VERIFY);
        }
    }

    /* Report the partition that will boot next only when it differs from the
     * running one (i.e. an OTA selected another slot). */
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (boot != NULL && running != NULL && boot->address != running->address) {
        snprintf(info->next_partition, sizeof(info->next_partition), "%s", boot->label);
    }

    const esp_partition_t *next_update = esp_ota_get_next_update_partition(NULL);
    info->max_image_size = (next_update != NULL) ? (size_t)next_update->size : 0;
    portENTER_CRITICAL(&s_state_lock);
    info->upload_in_progress = s_upload_in_progress;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

/* Clears the whole OTA bookkeeping in one critical section. The four fields are
 * only ever meaningful together, so every exit path goes through here: leaving
 * one behind would make the module look idle while it is not. */
static void ota_reset_state(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_update_handle = 0;
    s_update_partition = NULL;
    s_upload_in_progress = false;
    s_written = 0;
    portEXIT_CRITICAL(&s_state_lock);
}

esp_err_t cpe_ota_begin(size_t expected_size)
{
    /* Test and set in one critical section: the slot is reserved before the
     * (slow) partition work starts. esp_ota_begin() also refuses a second
     * session, but only once it gets there, so two callers could previously
     * both pass the busy check and race into it. */
    portENTER_CRITICAL(&s_state_lock);
    bool busy = s_upload_in_progress;
    if (!busy) {
        s_upload_in_progress = true;
        /* No write may use the handle of a previous session while this one is
         * still being set up. */
        s_update_handle = 0;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (busy) {
        return CPE_OTA_ERR_BUSY;
    }

    esp_err_t err;
    esp_ota_handle_t handle = 0;
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        ESP_LOGE(TAG, "No inactive OTA partition found");
        err = ESP_ERR_NOT_FOUND;
        goto fail;
    }
    if (expected_size > 0 && expected_size > (size_t)partition->size) {
        ESP_LOGE(TAG, "Image too large: %u bytes > partition %s (%u bytes)",
                 (unsigned)expected_size, partition->label, (unsigned)partition->size);
        err = ESP_ERR_INVALID_SIZE;
        goto fail;
    }

    err = esp_ota_begin(partition, expected_size > 0 ? expected_size : OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            /* The running image is still PENDING_VERIFY and must be validated
             * first (see cpe_ota_mark_running_valid()). */
            err = CPE_OTA_ERR_PENDING_VERIFY;
        } else if (err == ESP_ERR_OTA_ALREADY_IN_PROGRESS) {
            err = CPE_OTA_ERR_BUSY;
        }
        goto fail;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_update_handle = handle;
    s_update_partition = partition;
    s_written = 0;
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "OTA started: target partition %s (%u bytes)", partition->label,
             (unsigned)partition->size);
    return ESP_OK;

fail:
    /* Give the slot back: nothing was started, so a retry must be allowed. */
    ota_reset_state();
    return err;
}

esp_err_t cpe_ota_write(const void *data, size_t len)
{
    portENTER_CRITICAL(&s_state_lock);
    bool active = s_upload_in_progress;
    esp_ota_handle_t handle = s_update_handle;
    portEXIT_CRITICAL(&s_state_lock);
    if (!active || data == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_ota_write(handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            /* esp_ota_write() validates the image magic of the first chunk. */
            return CPE_OTA_ERR_INVALID_IMAGE;
        }
        return err;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_written += len;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t cpe_ota_end(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool active = s_upload_in_progress;
    esp_ota_handle_t handle = s_update_handle;
    const esp_partition_t *partition = s_update_partition;
    /* Read it now: the size is logged below, after the state is already gone.
     * Nothing writes to it in between, the upload is finished. */
    size_t written = s_written;
    portEXIT_CRITICAL(&s_state_lock);
    if (!active || partition == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s (image rejected)", esp_err_to_name(err));
        ota_reset_state();
        return CPE_OTA_ERR_INVALID_IMAGE;
    }

    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        ota_reset_state();
        return err;
    }

    esp_app_desc_t new_desc;
    if (esp_ota_get_partition_description(partition, &new_desc) == ESP_OK) {
        ESP_LOGI(TAG, "OTA image validated: version %s on %s (%u bytes), boot partition switched",
                 new_desc.version, partition->label, (unsigned)written);
    } else {
        ESP_LOGI(TAG, "OTA image validated on %s (%u bytes), boot partition switched",
                 partition->label, (unsigned)written);
    }

    ota_reset_state();
    return ESP_OK;
}

void cpe_ota_abort(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool active = s_upload_in_progress;
    esp_ota_handle_t handle = s_update_handle;
    portEXIT_CRITICAL(&s_state_lock);
    if (!active) {
        return;
    }
    esp_err_t err = esp_ota_abort(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_abort failed: %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "OTA aborted, current image unchanged");
    ota_reset_state();
}

/* Only cpe_ota_mark_running_valid() needs it, and it logs the image state on
 * every call, so it stays inside this file rather than being a public query. */
static bool ota_running_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return false;
    }
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "Running image is PENDING_VERIFY (%s)", ota_image_state_str(state));
        return true;
    }
    ESP_LOGI(TAG, "Running image state: %s", ota_image_state_str(state));
    return false;
}

esp_err_t cpe_ota_mark_running_valid(void)
{
    if (!ota_running_pending_verify()) {
        return ESP_OK;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA image validated and marked valid (rollback cancelled)");
    } else {
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed: %s", esp_err_to_name(err));
    }
    return err;
}
