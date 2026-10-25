/*
 * SPDX-FileCopyrightText: 2026 Yang Kaidong
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Single WS2812 (addressable RGB) status LED driven from RMT.
 *
 * Pattern priority, highest first:
 *   factory reset pending        blue blink
 *   WAN ready                    green solid
 *   WAN link up, not ready       red solid
 *   no WAN link                  red blink
 *
 * "WAN ready" is the link manager's view (link up and an IP acquired,
 * cpe_status.got_ip) plus, when AT telemetry is available, a SIM card that is
 * registered on the network. The modem's ECM DHCP server can hand out an IP
 * with no SIM inserted, so got_ip alone is not proof of a usable WAN.
 */

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "cpe_status.h"
#include "status_led.h"

static const char *TAG = "status_led";

#if CONFIG_CPE_STATUS_LED_GPIO >= 0

#define LED_GPIO        CONFIG_CPE_STATUS_LED_GPIO
#define LED_BRIGHTNESS  CONFIG_CPE_STATUS_LED_BRIGHTNESS
#define LED_BLINK_MS    CONFIG_CPE_STATUS_LED_BLINK_MS

/* Poll period: short enough for a crisp blink edge, long enough that the LED
 * is only written when the rendered colour actually changes. */
#define LED_TICK_MS     50

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
static bool s_inited;
static volatile bool s_factory_reset;

typedef enum {
    LED_OFF = 0,
    LED_RED_BLINK,
    LED_RED_SOLID,
    LED_GREEN_SOLID,
    LED_BLUE_BLINK,
} led_mode_t;

static uint8_t scale(uint8_t value)
{
    return (uint8_t)((unsigned)value * LED_BRIGHTNESS / 255U);
}

static esp_err_t led_send(uint8_t r, uint8_t g, uint8_t b)
{
    /* WS2812 receives GRB, most significant bit first. */
    uint8_t grb[3] = { scale(g), scale(r), scale(b) };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_chan, s_encoder, grb, sizeof(grb), &tx);
    if (err != ESP_OK) {
        return err;
    }
    return rmt_tx_wait_all_done(s_chan, 100);
}

/* AT telemetry is authoritative when it has been read: a SIM card must be
 * present and the modem registered. Without AT (modem exposes no usable AT
 * interface, or CPE_AT_ENABLE is off) keep the transport view. */
static bool network_ready(const cpe_status_t *st)
{
    if (!st->sim_valid) {
        return true;
    }
    return st->sim_inserted && st->modem_registered;
}

static led_mode_t mode_from_status(const cpe_status_t *st)
{
    if (s_factory_reset) {
        return LED_BLUE_BLINK;
    }
    if (st->got_ip && network_ready(st)) {
        return LED_GREEN_SOLID;
    }
    if (st->link_up) {
        return LED_RED_SOLID;
    }
    return LED_RED_BLINK;
}

static bool blink_phase_on(void)
{
    return ((esp_timer_get_time() / 1000) / LED_BLINK_MS) % 2 == 0;
}

static void mode_color(led_mode_t mode, bool on, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = 0;
    *g = 0;
    *b = 0;
    switch (mode) {
    case LED_RED_BLINK:
        if (on) {
            *r = 255;
        }
        break;
    case LED_RED_SOLID:
        *r = 255;
        break;
    case LED_GREEN_SOLID:
        *g = 255;
        break;
    case LED_BLUE_BLINK:
        if (on) {
            *b = 255;
        }
        break;
    case LED_OFF:
    default:
        break;
    }
}

static void status_led_task(void *arg)
{
    (void)arg;
    led_mode_t last_mode = LED_OFF;
    bool last_on = false;
    bool error_logged = false;

    for (;;) {
        cpe_status_t st;
        cpe_status_get(&st);
        led_mode_t mode = mode_from_status(&st);
        bool on = (mode == LED_RED_SOLID || mode == LED_GREEN_SOLID) ? true : blink_phase_on();

        if (mode != last_mode || on != last_on) {
            uint8_t r;
            uint8_t g;
            uint8_t b;
            mode_color(mode, on, &r, &g, &b);
            if (led_send(r, g, b) != ESP_OK) {
                if (!error_logged) {
                    ESP_LOGE(TAG, "Failed to write the WS2812 on GPIO%d", LED_GPIO);
                    error_logged = true;
                }
            } else {
                error_logged = false;
            }
            last_mode = mode;
            last_on = on;
        }

        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

esp_err_t status_led_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = 10 * 1000 * 1000, /* 0.1 us per tick */
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT channel for GPIO%d: %s",
                 LED_GPIO, esp_err_to_name(err));
        return err;
    }

    /* WS2812B: T0H 0.3 us / T0L 0.9 us, T1H 0.9 us / T1L 0.3 us.
     * A frame ends with the line held low until the next transmission; the
     * 50 ms tick is far longer than the >50 us reset pulse. */
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&enc_cfg, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT encoder: %s", esp_err_to_name(err));
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return err;
    }

    err = rmt_enable(s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RMT channel: %s", esp_err_to_name(err));
        rmt_del_encoder(s_encoder);
        s_encoder = NULL;
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return err;
    }

    s_inited = true;
    (void)led_send(0, 0, 0);
    ESP_LOGI(TAG, "WS2812 status LED on GPIO%d", LED_GPIO);
    return ESP_OK;
}

esp_err_t status_led_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(status_led_task, "status_led", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the status LED task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void status_led_factory_reset(void)
{
    s_factory_reset = true;
}

#else /* CONFIG_CPE_STATUS_LED_GPIO < 0 */

esp_err_t status_led_init(void)
{
    return ESP_OK;
}

esp_err_t status_led_start(void)
{
    return ESP_OK;
}

void status_led_factory_reset(void)
{
}

#endif /* CONFIG_CPE_STATUS_LED_GPIO */
