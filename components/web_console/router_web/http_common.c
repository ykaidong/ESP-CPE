/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Low-level helpers shared by every module of the management web server.
 */

#include "esp_log.h"

#include "http_internal.h"

static const char *TAG = "cpe_web";

/* Overwrite sensitive buffers so credentials do not linger on the stack. */
void cpe_web_secure_wipe(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) {
        *p++ = 0;
    }
}

esp_err_t cpe_web_register_uris(httpd_handle_t server, const httpd_uri_t *uris,
                                size_t count)
{
    for (size_t i = 0; i < count; i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register handler %s: %s",
                     uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
