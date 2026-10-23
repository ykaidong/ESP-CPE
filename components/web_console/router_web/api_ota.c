/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Firmware update endpoints.
 */

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "cpe_ota.h"
#include "cpe_wan.h"
#include "http_internal.h"

static const char *TAG = "cpe_web";

#define OTA_CHUNK_SIZE              4096
/* Maximum time between two successfully received chunks (not the total upload
 * duration): a slow but progressing client is never aborted. The upload runs on
 * its own task, so a stuck client no longer blocks the console; 15 s is plenty
 * for a stalled TCP window to recover. */
#define OTA_UPLOAD_IDLE_TIMEOUT_US  (15LL * 1000 * 1000)
#define OTA_DRAIN_TIMEOUT_US        (5LL * 1000 * 1000)

/* Stack and priority of the one-shot upload task (same order of magnitude as
 * the other console helper tasks). */
#define OTA_UPLOAD_TASK_STACK       4096
#define OTA_UPLOAD_TASK_PRIO        5

/*
 * The OTA upload is a binary body (not JSON), so precheck_post() cannot be
 * used: it rejects anything larger than CPE_WEB_MAX_JSON_BODY. Only the origin
 * check and the presence of a body are enforced here; the size limit is applied
 * by cpe_ota_begin() against the inactive OTA partition.
 */
static esp_err_t precheck_ota_post(httpd_req_t *req)
{
    if (req->content_len == 0) {
        return cpe_web_send_error(req, "empty_body");
    }
    if (!cpe_web_request_origin_is_same_origin(req)) {
        ESP_LOGW(TAG, "Rejecting cross-origin firmware upload");
        return cpe_web_send_error(req, "cross_origin_rejected");
    }
    return ESP_OK;
}

esp_err_t api_ota_status_get_handler(httpd_req_t *req)
{
    cpe_ota_info_t info;
    if (cpe_ota_get_info(&info) != ESP_OK) {
        return cpe_web_send_error(req, "ota_info_failed");
    }

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 512);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"version\":");
    cpe_web_sbuf_append_json_string(&sb, info.running_version);
    cpe_web_sbuf_append(&sb, ",\"partition\":");
    cpe_web_sbuf_append_json_string(&sb, info.running_partition);
    cpe_web_sbuf_append(&sb, ",\"next_partition\":");
    cpe_web_sbuf_append_json_string(&sb, info.next_partition);
    cpe_web_sbuf_appendf(&sb, ",\"max_image_size\":%u,\"pending_verify\":%s,\"upload_in_progress\":%s}",
                         (unsigned)info.max_image_size,
                         info.pending_verify ? "true" : "false",
                         info.upload_in_progress ? "true" : "false");
    return cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);
}

/*
 * Discard the unread part of the upload body before answering with an error.
 *
 * If the server closes the socket while the browser is still sending, the
 * client gets a TCP reset and its XHR reports a network error ("upload
 * interrupted") instead of the real 4xx response. Draining first lets the
 * response reach the page. The drain is time bounded so an authenticated
 * client cannot keep the (single) HTTP server task busy forever, and it is only
 * used after the session/CSRF checks have passed.
 */
static void ota_drain_unread_body(httpd_req_t *req, size_t unread_len)
{
    char scratch[256];
    int64_t start_us = esp_timer_get_time();
    while (unread_len > 0) {
        size_t to_read = (unread_len > sizeof(scratch)) ? sizeof(scratch) : unread_len;
        int received = httpd_req_recv(req, scratch, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (esp_timer_get_time() - start_us > OTA_DRAIN_TIMEOUT_US) {
                ESP_LOGW(TAG, "Stopped draining %u unread upload bytes",
                         (unsigned)unread_len);
                break;
            }
            continue;
        }
        if (received <= 0) {
            ESP_LOGW(TAG, "Client aborted while the upload error was being returned");
            break;
        }
        unread_len -= (size_t)received;
    }
}

/*
 * Receive the image and write it to the inactive OTA partition.
 *
 * Runs on the upload task created by api_ota_upload_post_handler() with an
 * async copy of the request: esp_http_server serves every socket from a single
 * task, so doing this inside the URI handler would block the whole management
 * console for as long as the upload takes (and a slow or stuck client could
 * keep it blocked indefinitely).
 *
 * The caller owns the request copy and must call
 * httpd_req_async_handler_complete() after this returns.
 */
static esp_err_t ota_upload_run(httpd_req_t *req)
{
    uint8_t *buf = malloc(OTA_CHUNK_SIZE);
    if (buf == NULL) {
        ota_drain_unread_body(req, req->content_len);
        return cpe_web_send_error(req, "out_of_memory");
    }

    /* Read the first chunk before touching flash: it carries the image magic
     * byte, so a wrong file (HTML page, wrong binary, ...) is rejected without
     * erasing the inactive OTA partition. */
    size_t remaining = req->content_len;
    int64_t last_rx_us = esp_timer_get_time();
    int first_len = 0;
    for (;;) {
        first_len = httpd_req_recv(req, (char *)buf,
                                   remaining > OTA_CHUNK_SIZE ? OTA_CHUNK_SIZE : remaining);
        if (first_len == HTTPD_SOCK_ERR_TIMEOUT) {
            if (esp_timer_get_time() - last_rx_us > OTA_UPLOAD_IDLE_TIMEOUT_US) {
                /* Drain before answering, like every other error path: the
                 * browser is still sending and would otherwise report a
                 * network error instead of the real 408. */
                ota_drain_unread_body(req, remaining);
                free(buf);
                return cpe_web_send_error(req, "upload_timeout");
            }
            continue;
        }
        break;
    }
    if (first_len <= 0) {
        free(buf);
        return cpe_web_send_error(req, "upload_interrupted");
    }
    last_rx_us = esp_timer_get_time();
    if (buf[0] != 0xE9) { /* ESP_IMAGE_HEADER_MAGIC */
        ESP_LOGW(TAG, "Rejected upload: not an ESP32 firmware image (magic 0x%02x)", buf[0]);
        free(buf);
        ota_drain_unread_body(req, (size_t)req->content_len - (size_t)first_len);
        return cpe_web_send_error(req, "invalid_firmware_image");
    }

    esp_err_t err = cpe_ota_begin(req->content_len);
    if (err == ESP_ERR_INVALID_SIZE) {
        free(buf);
        ota_drain_unread_body(req, (size_t)req->content_len - (size_t)first_len);
        return cpe_web_send_error(req, "image_too_large");
    }
    if (err == CPE_OTA_ERR_PENDING_VERIFY) {
        /* The running image was installed by OTA and has not been validated
         * yet (cpe_ota validates it CPE_OTA_VALIDATE_DELAY_MS after boot). */
        free(buf);
        ota_drain_unread_body(req, (size_t)req->content_len - (size_t)first_len);
        return cpe_web_send_error(req, "pending_verify");
    }
    if (err == CPE_OTA_ERR_BUSY) {
        free(buf);
        ota_drain_unread_body(req, (size_t)req->content_len - (size_t)first_len);
        return cpe_web_send_error(req, "ota_busy");
    }
    if (err != ESP_OK) {
        free(buf);
        ota_drain_unread_body(req, (size_t)req->content_len - (size_t)first_len);
        return cpe_web_send_error(req, "ota_begin_failed");
    }

    esp_err_t upload_err = ESP_OK;
    while (first_len > 0) {
        /* Account the bytes already read from the socket before writing them,
         * so remaining stays "still unread" on every error path below. */
        remaining -= (size_t)first_len;
        upload_err = cpe_ota_write(buf, (size_t)first_len);
        if (upload_err != ESP_OK) {
            break;
        }
        if (remaining == 0) {
            break;
        }
        int received = httpd_req_recv(req, (char *)buf,
                                      remaining > OTA_CHUNK_SIZE ? OTA_CHUNK_SIZE : remaining);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (esp_timer_get_time() - last_rx_us > OTA_UPLOAD_IDLE_TIMEOUT_US) {
                upload_err = ESP_ERR_TIMEOUT;
                break;
            }
            continue;
        }
        if (received <= 0) {
            upload_err = ESP_FAIL;
            break;
        }
        last_rx_us = esp_timer_get_time();
        first_len = received;
    }
    free(buf);

    if (upload_err != ESP_OK) {
        cpe_ota_abort();
        ESP_LOGE(TAG, "Firmware upload failed: %s", esp_err_to_name(upload_err));
        if (remaining > 0) {
            ota_drain_unread_body(req, remaining);
        }
        if (upload_err == CPE_OTA_ERR_INVALID_IMAGE) {
            return cpe_web_send_error(req, "invalid_firmware_image");
        }
        if (upload_err == ESP_ERR_TIMEOUT) {
            return cpe_web_send_error(req, "upload_timeout");
        }
        if (upload_err == ESP_FAIL) {
            /* httpd_req_recv() <= 0: the client went away mid-upload. */
            return cpe_web_send_error(req, "upload_interrupted");
        }
        /* A real write/finalization error, not a bad image or a dropped link. */
        return cpe_web_send_error(req, "ota_write_failed");
    }

    err = cpe_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Firmware image rejected or boot partition switch failed: %s",
                 esp_err_to_name(err));
        if (err == CPE_OTA_ERR_INVALID_IMAGE) {
            return cpe_web_send_error(req, "invalid_firmware_image");
        }
        return cpe_web_send_error(req, "ota_finalize_failed");
    }

    cpe_web_sbuf_t sb;
    cpe_web_sbuf_init(&sb, 96);
    cpe_web_sbuf_append(&sb, "{\"status\":\"ok\",\"message\":\"ota_ready\",\"reboot\":true}");
    err = cpe_web_send_sbuf_json(req, HTTPD_200, &sb, false);

    /* The response is queued; the shared helper waits for it to leave the
     * socket and then boots the new image. */
    ESP_LOGI(TAG, "Firmware update stored, restarting into the new image");
    if (cpe_wan_reboot_deferred(true, "firmware update") != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(800));
        cpe_wan_reboot(true);
    }
    return err;
}

/* One-shot task that owns the upload: it keeps the single HTTP server task free
 * while the body is received and written to flash. */
static void ota_upload_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    (void)ota_upload_run(req);

    /* On success ota_upload_run() has already answered and restarted the
     * device, so this only runs on the error paths: give the socket back to the
     * server so it can be reused. */
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

esp_err_t api_ota_upload_post_handler(httpd_req_t *req)
{
    esp_err_t pre = precheck_ota_post(req);
    if (pre != ESP_OK) {
        return pre;
    }
    char session[CPE_WEB_SESSION_BUF_LEN] = { 0 };
    esp_err_t err = cpe_web_require_session_mutable(req, session);
    if (err != ESP_OK) {
        return err;
    }

    cpe_ota_info_t info;
    if (cpe_ota_get_info(&info) != ESP_OK) {
        ota_drain_unread_body(req, req->content_len);
        return cpe_web_send_error(req, "ota_info_failed");
    }
    if (info.upload_in_progress) {
        ota_drain_unread_body(req, req->content_len);
        return cpe_web_send_error(req, "ota_busy");
    }
    if (info.max_image_size > 0 && req->content_len > info.max_image_size) {
        ESP_LOGW(TAG, "Rejecting firmware upload of %u bytes (partition is %u bytes)",
                 (unsigned)req->content_len, (unsigned)info.max_image_size);
        ota_drain_unread_body(req, req->content_len);
        return cpe_web_send_error(req, "image_too_large");
    }

    /* Everything below (reading the body, writing flash, rebooting) must not run
     * in the HTTP server task: hand an async copy of the request to a dedicated
     * task and return, so the console keeps serving the other sockets. */
    httpd_req_t *async_req = NULL;
    err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start the asynchronous upload: %s", esp_err_to_name(err));
        return cpe_web_send_error(req, "out_of_memory");
    }
    if (xTaskCreate(ota_upload_task, "ota_upload", OTA_UPLOAD_TASK_STACK, async_req,
                    OTA_UPLOAD_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the upload task");
        httpd_req_async_handler_complete(async_req);
        return cpe_web_send_error(req, "internal_error");
    }
    return ESP_OK;
}


