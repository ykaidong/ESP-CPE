/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CPE_SESSION_TOKEN_LEN   33  /* 32 hex chars + NUL */
#define CPE_CSRF_TOKEN_LEN      33

typedef enum {
    CPE_AUTH_OK = 0,
    CPE_AUTH_ERR_CREDENTIALS,   /*!< Wrong user name or password */
    CPE_AUTH_ERR_LOCKED,        /*!< Too many failures, temporarily locked */
    CPE_AUTH_ERR_NO_MEM,        /*!< No free session slot */
    CPE_AUTH_ERR_INVALID_ARG,   /*!< Malformed argument */
} cpe_auth_result_t;

esp_err_t cpe_auth_init(void);

/**
 * @brief Verify credentials and create a new session.
 *
 * @param[out] session  Session token, at least CPE_SESSION_TOKEN_LEN bytes.
 * @param[out] csrf     CSRF token, at least CPE_CSRF_TOKEN_LEN bytes.
 * @param[out] must_change  True when the admin password must be changed first.
 */
cpe_auth_result_t cpe_auth_login(const char *username, const char *password,
                                 char *session, size_t session_len,
                                 char *csrf, size_t csrf_len,
                                 bool *must_change);

/** Validate a session token (refreshes the idle timer). */
bool cpe_auth_session_valid(const char *session);

/** Validate the CSRF token bound to a session. */
bool cpe_auth_csrf_valid(const char *session, const char *csrf);

void cpe_auth_logout(const char *session);

/** Invalidate every session (after password change or factory reset). */
void cpe_auth_logout_all(void);

/**
 * @brief Change the admin password.
 *
 * The old password must be correct and the new one must pass validation.
 * All sessions are invalidated on success.
 */
esp_err_t cpe_auth_change_password(const char *old_password, const char *new_password);

bool cpe_auth_must_change(void);

/** Remaining lockout time in seconds (0 when not locked). */
uint32_t cpe_auth_lockout_remaining_s(void);

#ifdef __cplusplus
}
#endif
