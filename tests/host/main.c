/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Entry point of the host unit tests. One suite per source under test; each
 * suite only covers logic that has no hardware or RTOS dependency.
 */

#include <stdio.h>

#include "test_harness.h"

void test_json_util(void);
void test_station_names(void);
void test_config_validate(void);

int main(void)
{
    printf("ESP-CPE host unit tests (pure logic, ESP-IDF stubbed out)\n");
    ht_run_suite("json_util: buffer, escaping, depth guard, field getters", test_json_util);
    ht_run_suite("station_names: MAC parsing, nickname whitelist", test_station_names);
    ht_run_suite("cpe_config: configuration validation", test_config_validate);
    return ht_report();
}
