/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host stub: the request/server types the tested sources declare or pass around.
 * Nothing from them is dereferenced on the host.
 */

#pragma once

/* The real header pulls the event types in; the web console relies on that. */
#include "esp_event.h"

typedef struct httpd_req {
    int unused;
} httpd_req_t;

typedef void *httpd_handle_t;
typedef int httpd_method_t;
typedef struct httpd_uri httpd_uri_t;
