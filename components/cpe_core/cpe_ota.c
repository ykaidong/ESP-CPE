/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Streaming OTA update helper (see cpe_ota.h).
 *
 * Only one upload can be in progress at a time: the HTTP handler streams the
 * body chunk by chunk and any second request is rejected with
 * ESP_ERR_INVALID_STATE, so two clients can never write the same partition.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"

#include "cpe_ota.h"

static const char *TAG = "cpe_ota";

/* Upload state. A single HTTP server task handles the requests, but the state
 * is also read from the status handler, so keep the updates short and atomic
 * from the scheduler's point of view. */
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
    info->upload_in_progress = s_upload_in_progress;
    return ESP_OK;
}

esp_err_t cpe_ota_begin(size_t expected_size)
{
    if (s_upload_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        ESP_LOGE(TAG, "No inactive OTA partition found");
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_size > 0 && expected_size > (size_t)partition->size) {
        ESP_LOGE(TAG, "Image too large: %u bytes > partition %s (%u bytes)",
                 (unsigned)expected_size, partition->label, (unsigned)partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(partition, expected_size > 0 ? expected_size : OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    s_update_handle = handle;
    s_update_partition = partition;
    s_written = 0;
    s_upload_in_progress = true;
    ESP_LOGI(TAG, "OTA started: target partition %s (%u bytes)", partition->label,
             (unsigned)partition->size);
    return ESP_OK;
}

esp_err_t cpe_ota_write(const void *data, size_t len)
{
    if (!s_upload_in_progress || data == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_ota_write(s_update_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        return err;
    }
    s_written += len;
    return ESP_OK;
}

esp_err_t cpe_ota_end(void)
{
    if (!s_upload_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ota_end(s_update_handle);
    s_update_handle = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s (image rejected)", esp_err_to_name(err));
        s_upload_in_progress = false;
        s_update_partition = NULL;
        return err;
    }

    err = esp_ota_set_boot_partition(s_update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        s_upload_in_progress = false;
        s_update_partition = NULL;
        return err;
    }

    esp_app_desc_t new_desc;
    if (esp_ota_get_partition_description(s_update_partition, &new_desc) == ESP_OK) {
        ESP_LOGW(TAG, "OTA image validated: version %s on %s (%u bytes), boot partition switched",
                 new_desc.version, s_update_partition->label, (unsigned)s_written);
    } else {
        ESP_LOGW(TAG, "OTA image validated on %s (%u bytes), boot partition switched",
                 s_update_partition->label, (unsigned)s_written);
    }

    s_upload_in_progress = false;
    s_update_partition = NULL;
    s_written = 0;
    return ESP_OK;
}

void cpe_ota_abort(void)
{
    if (!s_upload_in_progress) {
        return;
    }
    esp_err_t err = esp_ota_abort(s_update_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_abort failed: %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "OTA aborted, current image unchanged");
    s_update_handle = 0;
    s_update_partition = NULL;
    s_upload_in_progress = false;
    s_written = 0;
}

bool cpe_ota_running_pending_verify(void)
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
    if (!cpe_ota_running_pending_verify()) {
        return ESP_OK;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "OTA image validated and marked valid (rollback cancelled)");
    } else {
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed: %s", esp_err_to_name(err));
    }
    return err;
}
