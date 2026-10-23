/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reset button helper (active low to GND by default, GPIO0 on the reference
 * board).
 *
 * Two behaviours are tied to the same physical button:
 *
 *  1. Double-click it within CPE_CREDENTIAL_PRINT_WINDOW_MS after boot: print
 *     the SoftAP/admin credentials to the UART. This deliberately requires a
 *     physical action, so the credentials are only exposed to someone who has
 *     the device in hand. The AP password is always printable; the admin
 *     password is only printable while it still equals the AP password (a
 *     changed admin password is stored as a hash only).
 *     (Holding the button at boot is NOT used: GPIO0 is the ESP32-S3 boot
 *     strapping pin, so a button held across reset selects UART download mode
 *     and the application never starts.)
 *  2. Keep holding the button for CPE_FACTORY_RESET_HOLD_MS after boot:
 *     restore factory settings and reboot.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "cpe_config.h"
#include "cpe_system.h"
#include "cpe_wan.h"
#include "reset_button.h"
#include "status_led.h"

static const char *TAG = "reset_button";

#if CONFIG_CPE_RESET_BUTTON_GPIO >= 0

#define BUTTON_GPIO         CONFIG_CPE_RESET_BUTTON_GPIO
#define BUTTON_ACTIVE_LEVEL CONFIG_CPE_RESET_BUTTON_ACTIVE_LEVEL

/* How often to complain while a factory reset is waiting for the button to be
 * released (the wait must not be open ended). */
#define RESET_RELEASE_REMINDER_MS   5000

static bool s_gpio_ready;
static bool s_boot_printed;
static int64_t s_boot_print_deadline_us;

static bool button_is_pressed(void)
{
    return gpio_get_level(BUTTON_GPIO) == BUTTON_ACTIVE_LEVEL;
}

esp_err_t reset_button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (BUTTON_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (BUTTON_ACTIVE_LEVEL == 0) ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure reset button GPIO%d: %s",
                 BUTTON_GPIO, esp_err_to_name(err));
        return err;
    }

    s_boot_print_deadline_us = esp_timer_get_time() +
                               (int64_t)CONFIG_CPE_CREDENTIAL_PRINT_WINDOW_MS * 1000LL;
    s_gpio_ready = true;
    ESP_LOGI(TAG, "Reset button on GPIO%d: double-click within %d ms after boot "
             "to print credentials; hold %d ms for factory reset",
             BUTTON_GPIO, CONFIG_CPE_CREDENTIAL_PRINT_WINDOW_MS,
             CONFIG_CPE_FACTORY_RESET_HOLD_MS);
    return ESP_OK;
}

static void print_credentials(void)
{
    ESP_LOGI(TAG, "Reset button double-clicked: printing credentials");
    cpe_config_print_credentials("reset button double-click");
}

static void reset_button_reboot(void)
{
    ESP_LOGI(TAG, "Rebooting; double-click within %d ms after boot to print "
             "the credentials", CONFIG_CPE_CREDENTIAL_PRINT_WINDOW_MS);
    if (cpe_wan_reboot_deferred(true, "reset button") != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(300));
        cpe_wan_reboot(true);
    }
}

static void reset_button_task(void *arg)
{
    (void)arg;
    bool pressed = false;
    bool reset_triggered = false;
    /* Factory reset stored, reboot waiting for the button to be released. */
    bool reboot_pending = false;
    int64_t press_start_us = 0;
    int64_t last_press_us = 0;
    int64_t pending_warn_us = 0;

    for (;;) {
        bool now = button_is_pressed();
        int64_t now_us = esp_timer_get_time();

        if (now && !pressed) {
            /* Debounced press edge. */
            pressed = true;
            press_start_us = now_us;

            if (!s_boot_printed && now_us < s_boot_print_deadline_us &&
                    last_press_us != 0 &&
                    (now_us - last_press_us) <=
                    (int64_t)CONFIG_CPE_DOUBLE_CLICK_MS * 1000LL) {
                s_boot_printed = true;
                print_credentials();
            }
            last_press_us = now_us;

            ESP_LOGI(TAG, "Reset button pressed (hold %d ms for factory reset)",
                     CONFIG_CPE_FACTORY_RESET_HOLD_MS);
        } else if (!now) {
            pressed = false;
            if (reboot_pending) {
                /* Released: GPIO0 is high again, so the reboot cannot fall into
                 * the ROM download mode. */
                reboot_pending = false;
                reset_button_reboot();
            }
        }

        if (reboot_pending &&
                (now_us - pending_warn_us) >= (int64_t)RESET_RELEASE_REMINDER_MS * 1000LL) {
            pending_warn_us = now_us;
            ESP_LOGE(TAG, "Button still held: the factory settings are stored but "
                     "the reboot is postponed, release the button to complete it");
        }

        if (pressed && !reset_triggered &&
                (now_us - press_start_us) >=
                (int64_t)CONFIG_CPE_FACTORY_RESET_HOLD_MS * 1000LL) {
            reset_triggered = true;
            status_led_factory_reset();
            ESP_LOGI(TAG, "Reset button held for %d ms, restoring factory settings",
                     CONFIG_CPE_FACTORY_RESET_HOLD_MS);
            /* Same call as the web console: configuration, station nicknames,
             * the maintenance policy and the recovery budget are all cleared
             * there, so both paths leave the same state behind. */
            esp_err_t err = cpe_system_factory_reset();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Factory reset failed: %s; rebooting anyway",
                         esp_err_to_name(err));
            }
            /* GPIO0 must be released before the reboot, otherwise the ROM enters
             * UART download mode. The wait happens in the main loop instead of
             * blocking here: a stuck button then only postpones the reboot
             * (and says so every RESET_RELEASE_REMINDER_MS) instead of leaving
             * the device half reset forever. */
            reboot_pending = true;
            pending_warn_us = now_us;
            ESP_LOGI(TAG, "Release the button to complete the factory reset");
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t reset_button_start(void)
{
    if (!s_gpio_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t created = xTaskCreate(reset_button_task, "reset_btn", 3072, NULL, 3, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create reset button task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else /* CONFIG_CPE_RESET_BUTTON_GPIO < 0 */

esp_err_t reset_button_init(void)
{
    return ESP_OK;
}

esp_err_t reset_button_start(void)
{
    return ESP_OK;
}

#endif /* CONFIG_CPE_RESET_BUTTON_GPIO */
