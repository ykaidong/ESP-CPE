/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Streaming OTA update helper for the CPE management console.
 *
 * The image is written to the inactive OTA partition through esp_ota_*; the
 * boot partition is only switched after the whole image was received and
 * validated by esp_ota_end(). Nothing is changed when the upload is aborted or
 * interrupted, so a failed update cannot brick the device: the bootloader keeps
 * booting the current (valid) partition.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OTA specific error codes (user error range).
 *
 * The esp_ota_* errors are translated inside cpe_ota.c on purpose: callers
 * (e.g. the web component) must not need app_update headers, and the HTTP layer
 * maps these codes to status codes / JSON error names. */
#define CPE_OTA_ERR_BASE            (0x40000)
#define CPE_OTA_ERR_BUSY            (CPE_OTA_ERR_BASE + 1) /*!< Another upload is in progress */
#define CPE_OTA_ERR_PENDING_VERIFY  (CPE_OTA_ERR_BASE + 2) /*!< Running image not validated yet */
#define CPE_OTA_ERR_INVALID_IMAGE   (CPE_OTA_ERR_BASE + 3) /*!< Not a valid firmware image */

/** Information about the running image and a possible pending update. */
typedef struct {
    char running_version[32];   /*!< Version string of the running app */
    char running_partition[17]; /*!< Label of the running OTA partition ("ota_0"/"ota_1") */
    char next_partition[17];    /*!< Partition that will boot next, "" when it is the same */
    size_t partition_size;      /*!< Size in bytes of the running app partition */
    bool pending_verify;        /*!< Running image still has to be validated (rollback armed) */
    bool upload_in_progress;    /*!< A firmware upload is currently being written */
    size_t max_image_size;      /*!< Size of the inactive OTA partition (upload limit) */
} cpe_ota_info_t;

/**
 * @brief Fill @p info with the running image / pending update description.
 */
esp_err_t cpe_ota_get_info(cpe_ota_info_t *info);

/**
 * @brief Start a streaming update into the inactive OTA partition.
 *
 * @note begin/write/end/abort form one state machine and must be called from
 *       the same task (the HTTP server task). cpe_ota_get_info() is the only
 *       function safe to call from another task.
 *
 * @param[in] expected_size Image size announced by the uploader (0 = unknown).
 *                          When non zero it is validated against the partition
 *                          size before anything is written.
 */
esp_err_t cpe_ota_begin(size_t expected_size);

/** @brief Append a chunk of the new image. */
esp_err_t cpe_ota_write(const void *data, size_t len);

/**
 * @brief Finish the update, validate the image and select it for the next boot.
 *
 * The caller is responsible for rebooting the device afterwards.
 */
esp_err_t cpe_ota_end(void);

/** @brief Abort an in-progress update and leave the current image untouched. */
void cpe_ota_abort(void);

/**
 * @brief Mark the running image as valid, cancelling a pending rollback.
 *
 * Safe to call on a normal boot: it is a no-op unless the image is
 * PENDING_VERIFY (i.e. it was installed by OTA and not validated yet).
 * Returns ESP_ERR_NOT_SUPPORTED when CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is
 * disabled (nothing to validate in that case).
 */
esp_err_t cpe_ota_mark_running_valid(void);

#ifdef __cplusplus
}
#endif
