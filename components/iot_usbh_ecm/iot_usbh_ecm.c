/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "iot_usbh_cdc.h"
#include "usb/usb_helpers.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "iot_usbh_ecm.h"
#include "iot_eth_types.h"
#include "iot_eth_interface.h"
#include "iot_usbh_ecm_descriptor.h"

static const char *TAG = "iot_usbh_ecm";

#define ECM_DEV_CONNECTED (1 << 0)
#define ECM_STOP (1 << 4)
#define ECM_STOP_DONE (1 << 5)

#define ECM_EVENT_ALL (ECM_DEV_CONNECTED | ECM_STOP | ECM_STOP_DONE)

// Define the CDC ECM interface request codes, see CDC ECM Subclass 1.2
#define ECM_SET_ETHERNET_PACKET_FILTER 0x43
#define PACKET_TYPE_MULTICAST     BIT(4)
#define PACKET_TYPE_BROADCAST     BIT(3)
#define PACKET_TYPE_DIRECTED      BIT(2)
#define PACKET_TYPE_ALL_MULTICAST BIT(1)
#define PACKET_TYPE_PROMISCUOUS   BIT(0)

#define LANG_ID_ENGLISH 0x409

#define ECM_MAC_STR_MAX_LEN         64      /*!< GET_DESCRIPTOR(string) buffer size */
#define ECM_MAC_STR_MIN_DESC_LEN    26      /*!< bLength of a string descriptor holding 12 hex chars */
#define ECM_MAX_SEGMENT_SIZE        16384   /*!< Upper bound for the wMaxSegmentSize reported by the device */
#define ECM_MIN_SEGMENT_SIZE        512     /*!< Lower bound for the transfer buffer size */
#define ECM_DEFAULT_SEGMENT_SIZE    1514    /*!< Fallback when the device reports wMaxSegmentSize == 0 */
#define ECM_CONFIG_RETRY_COUNT      3       /*!< Number of interface configuration attempts per connection */
#define ECM_CONFIG_RETRY_DELAY_MS   500     /*!< Delay between two interface configuration attempts */
#define ECM_STOP_TIMEOUT_MS         10000   /*!< Max time to wait for the ECM task to stop */

typedef struct {
    iot_eth_driver_t base;
    iot_eth_mediator_t *mediator;
    iot_usbh_ecm_config_t config;
    EventGroupHandle_t event_group;
    TaskHandle_t task_h;
    SemaphoreHandle_t lock;         /*!< Protects the shared state below */
    uint8_t cdc_dev_addr;
    usb_device_handle_t cdc_dev_hdl;
    usbh_cdc_port_handle_t cdc_port;
    bool dev_connect_status;
    bool deiniting;
    bool mac_valid;
    uint8_t eth_mac_str_index;
    uint8_t eth_mac_addr[6];
    uint16_t itf_num;
    void *user_data;

    int last_connected;             /*!< -1: unknown, 0: disconnected, 1: connected */
    uint32_t last_up_speed;
    uint32_t last_down_speed;

    bool intf_configured;           /*!< Set once _ecm_config_intf() has finished */
    bool has_pending_link;          /*!< A link state change arrived before the interface was configured */
    bool pending_link_up;           /*!< Deferred link state to report after configuration completes */
    bool link_reported_up;          /*!< Last link state reported to iot_eth */
    /* Notifications can be dispatched by the CDC client task between
     * usbh_cdc_port_open() and the assignment of ecm->cdc_port below. Cache the
     * first NETWORK_CONNECTION notification received in that window (review
     * P1-3), otherwise it is lost and the auto-link-up heuristic may report a
     * bogus LINK_UP. */
    bool port_opening;
    bool pending_notif_valid;
    int pending_notif_connected;
    int config_attempts;
    TickType_t config_next_try_tick;
    TickType_t connected_tick;
} usbh_ecm_t;

#define ECM_LOCK(ecm)       xSemaphoreTake((ecm)->lock, portMAX_DELAY)
#define ECM_UNLOCK(ecm)     xSemaphoreGive((ecm)->lock)

/**
 * @brief Parse the MAC address string descriptor.
 *
 * The descriptor is a USB string descriptor: bLength, bDescriptorType(0x03) followed by UTF-16LE
 * characters. Both the "001122334455" (12 hex chars) and "00:11:22:33:44:55" (17 chars) layouts
 * are accepted; the separators are ignored.
 *
 * @param[in] raw Raw descriptor bytes
 * @param[in] len Number of bytes actually received
 * @param[out] mac Parsed MAC address
 * @return true on success
 */
static bool _parse_mac_descriptor(const uint8_t *raw, size_t len, uint8_t mac[6])
{
    if (raw == NULL || len < 2) {
        return false;
    }
    const size_t b_length = raw[0];
    if (raw[1] != 0x03 || b_length < ECM_MAC_STR_MIN_DESC_LEN || b_length > len || b_length > ECM_MAC_STR_MAX_LEN) {
        return false;
    }

    int byte_count = 0;
    uint8_t nibble = 0;
    bool have_nibble = false;
    // Each UTF-16LE character is stored in 2 bytes; only the low byte holds the ASCII character.
    // Two hex characters (nibbles) make one MAC byte, so stop after 6 bytes (12 characters).
    for (size_t i = 2; i + 1 < b_length && byte_count < 6; i += 2) {
        char c = (char)raw[i];
        int value;
        if (c >= '0' && c <= '9') {
            value = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            value = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            value = c - 'A' + 10;
        } else {
            continue;   // Skip ':' and any other separator
        }
        if (!have_nibble) {
            nibble = (uint8_t)value;
            have_nibble = true;
        } else {
            mac[byte_count++] = (uint8_t)((nibble << 4) | value);
            have_nibble = false;
        }
    }
    return (byte_count == 6) && !have_nibble;
}

/**
 * @brief Report a link stage change to iot_eth, only when the state actually changed.
 */
static void _ecm_report_link(usbh_ecm_t *ecm, bool up)
{
    if (ecm == NULL || ecm->lock == NULL || ecm->mediator == NULL) {
        return;
    }
    ECM_LOCK(ecm);
    bool changed = (ecm->link_reported_up != up);
    ecm->link_reported_up = up;
    ECM_UNLOCK(ecm);
    if (!changed) {
        return;
    }
    iot_eth_link_t link = up ? IOT_ETH_LINK_UP : IOT_ETH_LINK_DOWN;
    ecm->mediator->on_stage_changed(ecm->mediator, IOT_ETH_STAGE_LINK, &link);
}

/**
 * @brief Clamp the device reported wMaxSegmentSize into a sane transfer buffer size.
 */
static size_t _ecm_transfer_buffer_size(uint16_t w_max_segment_size)
{
    uint32_t size = w_max_segment_size;
    if (size == 0) {
        size = ECM_DEFAULT_SEGMENT_SIZE;
    }
    if (size > ECM_MAX_SEGMENT_SIZE) {
        size = ECM_MAX_SEGMENT_SIZE;
    }
    if (size < ECM_MIN_SEGMENT_SIZE) {
        size = ECM_MIN_SEGMENT_SIZE;
    }
    return (size_t)usb_round_up_to_mps((int)size, 512);
}

/**
 * @brief True while the cached port handle is still the active, connected one.
 *
 * _ecm_config_intf() performs several control transfers; the device can
 * disappear in between and the CDC driver then releases the port. Re-check
 * before every request instead of trusting the handle cached at entry
 * (review P2-14).
 */
static bool _ecm_port_is_current(usbh_ecm_t *ecm, usbh_cdc_port_handle_t port)
{
    ECM_LOCK(ecm);
    bool ok = !ecm->deiniting && ecm->dev_connect_status && (ecm->cdc_port == port);
    ECM_UNLOCK(ecm);
    return ok;
}

static esp_err_t _ecm_config_intf(usbh_ecm_t *ecm)
{
    esp_err_t ret = ESP_OK;

    usbh_cdc_port_handle_t cdc_port;
    uint8_t mac_str_index;
    uint16_t ctrl_itf_num;
    ECM_LOCK(ecm);
    cdc_port = ecm->cdc_port;
    mac_str_index = ecm->eth_mac_str_index;
    ctrl_itf_num = ecm->itf_num;
    bool stopping = ecm->deiniting;
    ECM_UNLOCK(ecm);

    if (stopping) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cdc_port == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mac_str_index == 0) {
        ESP_LOGW(TAG, "The ECM function reports no MAC address string (iMACAddress == 0)");
    } else {
        uint8_t str[ECM_MAC_STR_MAX_LEN] = {0};
        size_t actual_len = 0;
        uint8_t bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
        uint16_t wValue = (USB_W_VALUE_DT_STRING << 8) | (mac_str_index & 0xFF);
        ret = usbh_cdc_send_custom_request_ex(cdc_port, bmRequestType, USB_B_REQUEST_GET_DESCRIPTOR,
                                              wValue, LANG_ID_ENGLISH, ECM_MAC_STR_MAX_LEN, str, &actual_len);
        if (ret != ESP_OK) {
            /* A missing MAC string must not abort the whole ECM configuration;
             * the fallback below provides a usable address. */
            ESP_LOGW(TAG, "Failed to read the MAC address string descriptor: %s", esp_err_to_name(ret));
        } else {
            uint8_t mac[6] = {0};
            if (!_parse_mac_descriptor(str, actual_len, mac)) {
                ESP_LOGW(TAG, "Failed to parse the MAC address descriptor (len %u), using fallback MAC",
                         (unsigned)actual_len);
                ESP_LOG_BUFFER_HEXDUMP(TAG, str,
                                       (actual_len <= ECM_MAC_STR_MAX_LEN) ? actual_len : ECM_MAC_STR_MAX_LEN,
                                       ESP_LOG_WARN);
            } else {
                ECM_LOCK(ecm);
                memcpy(ecm->eth_mac_addr, mac, sizeof(mac));
                ecm->mac_valid = true;
                ECM_UNLOCK(ecm);
                ESP_LOGI(TAG, "Parsed MAC address: %02X:%02X:%02X:%02X:%02X:%02X",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            }
        }
    }

    /* Devices without a MAC address string would leave the WAN netif with an
     * all-zero hwaddr (DHCP/ARP then fail). Fall back to a locally
     * administered unicast address instead of failing the link (review P2-10). */
    ECM_LOCK(ecm);
    bool need_fallback_mac = !ecm->mac_valid;
    ECM_UNLOCK(ecm);
    if (need_fallback_mac) {
        uint8_t fallback[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };
        uint32_t rnd = esp_random();
        for (int i = 2; i < 6; i++) {
            fallback[i] = (uint8_t)(rnd >> (8 * (5 - i)));
        }
        /* A non-zero, unicast, locally administered address is guaranteed by
         * the 0x02 first byte plus a non-zero suffix. */
        fallback[5] |= 0x01;
        ECM_LOCK(ecm);
        memcpy(ecm->eth_mac_addr, fallback, sizeof(fallback));
        ecm->mac_valid = true;
        ECM_UNLOCK(ecm);
        ESP_LOGW(TAG, "Using a locally administered fallback MAC address: %02X:%02X:%02X:%02X:%02X:%02X",
                 fallback[0], fallback[1], fallback[2], fallback[3], fallback[4], fallback[5]);
    }

    ECM_LOCK(ecm);
    stopping = ecm->deiniting;
    ECM_UNLOCK(ecm);
    if (stopping) {
        return ESP_ERR_INVALID_STATE;
    }

    // Setting ETHERNET packet filter to enable network interface
    if (!_ecm_port_is_current(ecm, cdc_port)) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Setting ETHERNET packet filter");
    uint8_t bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    uint16_t wValue = PACKET_TYPE_BROADCAST | PACKET_TYPE_DIRECTED | PACKET_TYPE_ALL_MULTICAST; // 0x0E
    ret = usbh_cdc_send_custom_request(cdc_port, bmRequestType, ECM_SET_ETHERNET_PACKET_FILTER, wValue, ctrl_itf_num, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set ETHERNET packet filter: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set the active alternate setting of the data interface. Do not guess "control interface + 1":
    // the CDC data interface number/alternate setting come from the CDC descriptor parser.
    if (!_ecm_port_is_current(ecm, cdc_port)) {
        return ESP_ERR_INVALID_STATE;
    }
    const usb_intf_desc_t *data_intf = NULL;
    ret = usbh_cdc_port_get_intf_desc(cdc_port, NULL, &data_intf);
    if (ret != ESP_OK || data_intf == NULL) {
        ESP_LOGE(TAG, "Failed to get the CDC data interface descriptor: %s", esp_err_to_name(ret));
        return (ret == ESP_OK) ? ESP_ERR_INVALID_STATE : ret;
    }
    ESP_LOGI(TAG, "Setting interface %d alternate setting %d", data_intf->bInterfaceNumber, data_intf->bAlternateSetting);
    bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    ret = usbh_cdc_send_custom_request(cdc_port, bmRequestType, USB_B_REQUEST_SET_INTERFACE,
                                       data_intf->bAlternateSetting, data_intf->bInterfaceNumber, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set the data interface alternate setting: %s", esp_err_to_name(ret));
        return ret;
    }

    // The data interface is active now: start the BULK IN polling. The port was opened with
    // USBH_CDC_FLAGS_DEFER_DATA_POLL for this purpose.
    if (!_ecm_port_is_current(ecm, cdc_port)) {
        return ESP_ERR_INVALID_STATE;
    }
    ret = usbh_cdc_port_start_data(cdc_port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start the BULK IN data polling: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

/**
 * @brief Try to configure the ECM interface once and update the link state.
 */
static void _ecm_config_attempt(usbh_ecm_t *ecm)
{
    bool allowed;
    ECM_LOCK(ecm);
    ecm->config_attempts++;
    ecm->config_next_try_tick = xTaskGetTickCount() + pdMS_TO_TICKS(ECM_CONFIG_RETRY_DELAY_MS);
    allowed = (ecm->config_attempts <= ECM_CONFIG_RETRY_COUNT) && ecm->dev_connect_status && !ecm->deiniting;
    ECM_UNLOCK(ecm);
    if (!allowed) {
        return;
    }

    esp_err_t ret = _ecm_config_intf(ecm);
    if (ret == ESP_OK) {
        bool pending = false;
        bool pending_up = false;
        ECM_LOCK(ecm);
        ecm->intf_configured = true;
        ecm->connected_tick = xTaskGetTickCount();
        if (ecm->has_pending_link) {
            pending = true;
            pending_up = ecm->pending_link_up;
            ecm->has_pending_link = false;
        }
        ECM_UNLOCK(ecm);
        ESP_LOGI(TAG, "USB ECM interface configured");
        if (pending) {
            // Report the link state that was received before configuration completed
            ESP_LOGI(TAG, "Reporting deferred link state: %s", pending_up ? "up" : "down");
            _ecm_report_link(ecm, pending_up);
        }
    } else {
        bool give_up;
        ECM_LOCK(ecm);
        ecm->intf_configured = false;
        give_up = (ecm->config_attempts >= ECM_CONFIG_RETRY_COUNT);
        ECM_UNLOCK(ecm);
        ESP_LOGE(TAG, "Failed to configure the ECM interface (attempt %d): %s",
                 (int)ecm->config_attempts, esp_err_to_name(ret));
        if (give_up) {
            ESP_LOGE(TAG, "Giving up configuring the ECM interface, reporting link down");
        }
        // Never report a fake link-up when the data path is not configured.
        _ecm_report_link(ecm, false);
    }
}

static esp_err_t usbh_ecm_transmit(iot_eth_driver_t *h, uint8_t *buffer, size_t buflen)
{
    usbh_ecm_t *ecm = __containerof(h, usbh_ecm_t, base);
    usbh_cdc_port_handle_t cdc_port;
    ECM_LOCK(ecm);
    cdc_port = ecm->cdc_port;
    bool ready = (cdc_port != NULL) && ecm->dev_connect_status && !ecm->deiniting;
    ECM_UNLOCK(ecm);
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = usbh_cdc_write_bytes(cdc_port, buffer, buflen, pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to transmit packet: %s", esp_err_to_name(ret));
    }
    return ret;
}

static void _usbh_ecm_recv_data_cb(usbh_cdc_port_handle_t cdc_port_handle, void *arg)
{
    usbh_ecm_t *ecm = (usbh_ecm_t *)arg;
    if (ecm == NULL || ecm->lock == NULL) {
        return;
    }
    size_t rx_length = 0;
    if (usbh_cdc_get_rx_buffer_size(cdc_port_handle, &rx_length) != ESP_OK || rx_length == 0) {
        return;
    }
    uint8_t *buf = malloc(rx_length);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for the RX packet", (unsigned)rx_length);
        return;
    }
    esp_err_t ret = usbh_cdc_read_bytes(cdc_port_handle, buf, &rx_length, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read ECM data from CDC: %s", esp_err_to_name(ret));
        free(buf);
        return;
    }
    if (rx_length == 0) {
        free(buf);
        return;
    }
    ESP_LOGD(TAG, "USB ECM received data: %u", (unsigned)rx_length);

    ECM_LOCK(ecm);
    bool ready = !ecm->deiniting && (ecm->cdc_port == cdc_port_handle);
    iot_eth_mediator_t *mediator = ecm->mediator;
    ECM_UNLOCK(ecm);
    if (!ready || mediator == NULL) {
        free(buf);
        return;
    }

    if (mediator->stack_input_info) {
        // USB ECM currently has no extra RX frame metadata to forward.
        mediator->stack_input_info(mediator, buf, rx_length, NULL);
    } else if (mediator->stack_input) {
        mediator->stack_input(mediator, buf, rx_length);
    } else {
        ESP_LOGE(TAG, "No stack input callback registered, drop RX packet");
        free(buf);
    }
}

static void _usbh_ecm_notif_cb(usbh_cdc_port_handle_t cdc_port_handle, iot_cdc_notification_t *notif, uint32_t length, void *arg)
{
    usbh_ecm_t *ecm = (usbh_ecm_t *)arg;
    if (ecm == NULL || ecm->lock == NULL || notif == NULL) {
        return;
    }
    const uint32_t header_len = offsetof(iot_cdc_notification_t, Data);
    if (length < header_len) {
        ESP_LOGW(TAG, "Notification too short: %u bytes", (unsigned)length);
        return;
    }
    // The device may not report more payload than what was actually received
    if ((uint32_t)notif->wLength > (length - header_len)) {
        ESP_LOGW(TAG, "Malformed notification: wLength %u, received %u", notif->wLength, (unsigned)(length - header_len));
        return;
    }

    // Ignore notifications that belong to a port which is not (yet) the active one. The only
    // exception is the NETWORK_CONNECTION notification that can race with the port opening in
    // _usbh_ecm_dev_event_cb(): cache it and apply it once the handle is registered.
    ECM_LOCK(ecm);
    bool active_port = (ecm->cdc_port == cdc_port_handle) && !ecm->deiniting;
    bool opening = ecm->port_opening && !ecm->deiniting;
    if (!active_port && opening && (notif->bNotificationCode == USB_CDC_NOTIFY_NETWORK_CONNECTION)) {
        ecm->pending_notif_valid = true;
        ecm->pending_notif_connected = notif->wValue ? 1 : 0;
        ECM_UNLOCK(ecm);
        ESP_LOGD(TAG, "Cached a notification received while the port was still opening");
        return;
    }
    ECM_UNLOCK(ecm);
    if (!active_port) {
        return;
    }

    switch (notif->bNotificationCode) {
    case USB_CDC_NOTIFY_NETWORK_CONNECTION: {
        int connected = notif->wValue ? 1 : 0;
        bool configured, changed;
        ECM_LOCK(ecm);
        changed = (ecm->last_connected != connected);
        ecm->last_connected = connected;
        configured = ecm->intf_configured;
        if (!configured) {
            // Defer the link state until _ecm_config_intf() completes, otherwise the netif glue
            // would query the MAC address before it is available.
            ecm->has_pending_link = true;
            ecm->pending_link_up = connected;
        }
        ECM_UNLOCK(ecm);
        if (!changed) {
            // Avoid printing the message too frequently
            return;
        }
        ESP_LOGI(TAG, "Notify - network connection changed: %s", connected ? "Connected" : "Disconnected");
        if (configured) {
            _ecm_report_link(ecm, connected);
        }
        break;
    }
    case USB_CDC_NOTIFY_SPEED_CHANGE: {
        if ((uint32_t)notif->wLength < (2 * sizeof(uint32_t)) ||
                length < (header_len + 2 * sizeof(uint32_t))) {
            ESP_LOGW(TAG, "Malformed SPEED_CHANGE notification (wLength %u, received %u)",
                     notif->wLength, (unsigned)length);
            break;
        }
        uint32_t speeds[2];
        memcpy(speeds, notif->Data, sizeof(speeds));
        bool changed;
        ECM_LOCK(ecm);
        changed = (ecm->last_up_speed != speeds[0]) || (ecm->last_down_speed != speeds[1]);
        ecm->last_up_speed = speeds[0];
        ecm->last_down_speed = speeds[1];
        ECM_UNLOCK(ecm);
        if (changed) {
            ESP_LOGI(TAG, "Notify - link speeds: %lu kbps up, %lu kbps down",
                     (unsigned long)(speeds[0] / 1000), (unsigned long)(speeds[1] / 1000));
        }
        break;
    }
    default:
        ESP_LOGW(TAG, "unexpected notification 0x%02x!", notif->bNotificationCode);
        return;
    }
}

static void _usbh_ecm_dev_event_cb(usbh_cdc_device_event_t event, usbh_cdc_device_event_data_t *event_data, void *user_ctx)
{
    usbh_ecm_t *ecm = (usbh_ecm_t *)user_ctx;
    if (ecm == NULL || ecm->lock == NULL || event_data == NULL) {
        return;
    }

    switch (event) {
    case CDC_HOST_DEVICE_EVENT_CONNECTED: {
        ECM_LOCK(ecm);
        bool busy = (ecm->cdc_port != NULL) || ecm->deiniting;
        ECM_UNLOCK(ecm);
        if (busy) {
            ESP_LOGD(TAG, "ECM port already opened or driver is stopping, ignore this event");
            return;
        }

        const usb_config_desc_t *config_desc = event_data->new_dev.active_config_desc;
        const usb_device_desc_t *device_desc = event_data->new_dev.device_desc;
        if (config_desc == NULL || device_desc == NULL) {
            ESP_LOGE(TAG, "CONNECTED event without descriptors");
            return;
        }

        int itf_num = -1;
        esp_err_t ret = usbh_ecm_interface_check(device_desc, config_desc, &itf_num);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "No ECM interface found for device VID: %04X, PID: %04X, ignore it", device_desc->idVendor, device_desc->idProduct);
            return;
        }
        const usb_ecm_function_desc_t *ecm_func_desc = NULL;
        ret = usbh_get_ecm_function_desc(device_desc, config_desc, (uint8_t)itf_num, &ecm_func_desc);
        if (ret != ESP_OK || ecm_func_desc == NULL) {
            ESP_LOGW(TAG, "No Ethernet Networking functional descriptor found for VID: %04X, PID: %04X, ignore it",
                     device_desc->idVendor, device_desc->idProduct);
            return;
        }
        ESP_LOGI(TAG, "ECM interface found: VID: %04X, PID: %04X, IFNUM: %d", device_desc->idVendor, device_desc->idProduct, itf_num);

        size_t transfer_buffer_size = _ecm_transfer_buffer_size(ecm_func_desc->wMaxSegmentSize);
        ESP_LOGI(TAG, "ECM data interface transfer buffer size: %u bytes (wMaxSegmentSize %u)",
                 (unsigned)transfer_buffer_size, ecm_func_desc->wMaxSegmentSize);

        usbh_cdc_port_config_t cdc_port_config = {
            .dev_addr = event_data->new_dev.dev_addr,
            .itf_num = (uint8_t)itf_num,
            .in_transfer_buffer_size = transfer_buffer_size,
            .out_transfer_buffer_size = transfer_buffer_size,
            .cbs = {
                .notif_cb_ex = _usbh_ecm_notif_cb,
                .recv_data = _usbh_ecm_recv_data_cb,
                .user_data = ecm,
            },
            // Defer starting the BULK IN data polling until _ecm_config_intf() has set the
            // data interface alternate setting. Otherwise polling runs against an inactive
            // interface and floods "IN Transfer failed" until the retry budget is exhausted.
            .flags = USBH_CDC_FLAGS_DEFER_DATA_POLL
#ifdef CONFIG_ECM_DISABLE_CDC_NOTIFICATION
                     | USBH_CDC_FLAGS_DISABLE_NOTIFICATION
#endif
            ,
        };
        usbh_cdc_port_handle_t cdc_port = NULL;
        ECM_LOCK(ecm);
        ecm->port_opening = true;
        ecm->pending_notif_valid = false;
        ECM_UNLOCK(ecm);
        ret = usbh_cdc_port_open(&cdc_port_config, &cdc_port);
        if (ret != ESP_OK) {
            ECM_LOCK(ecm);
            ecm->port_opening = false;
            ECM_UNLOCK(ecm);
            ESP_LOGE(TAG, "Failed to open the CDC port of interface %d: %s", itf_num, esp_err_to_name(ret));
            return;
        }
        /* The port may have disappeared while it was being opened; do not
         * publish a stale handle (review P1-3 second half). */
        usbh_cdc_port_status_t port_status = USBH_CDC_PORT_STATUS_CLOSED;
        if (usbh_cdc_port_get_status(cdc_port, &port_status) != ESP_OK ||
                port_status == USBH_CDC_PORT_STATUS_CLOSED) {
            ESP_LOGW(TAG, "CDC port disappeared during opening, dropping it");
            (void)usbh_cdc_port_close(cdc_port);
            ECM_LOCK(ecm);
            ecm->port_opening = false;
            ECM_UNLOCK(ecm);
            return;
        }
        usb_device_handle_t cdc_dev_hdl = NULL;
        (void)usbh_cdc_get_dev_handle(cdc_port, &cdc_dev_hdl);

        bool pending_notif = false;
        int pending_notif_connected = 0;
        ECM_LOCK(ecm);
        ecm->cdc_port = cdc_port;
        ecm->cdc_dev_hdl = cdc_dev_hdl;
        ecm->cdc_dev_addr = event_data->new_dev.dev_addr;
        ecm->dev_connect_status = true;
        ecm->mac_valid = false;
        memset(ecm->eth_mac_addr, 0, sizeof(ecm->eth_mac_addr));
        ecm->eth_mac_str_index = ecm_func_desc->iMACAddress;
        ecm->itf_num = (uint16_t)itf_num;
        ecm->last_connected = -1;
        ecm->last_up_speed = -1;
        ecm->last_down_speed = -1;
        ecm->intf_configured = false;
        ecm->has_pending_link = false;
        ecm->pending_link_up = false;
        ecm->link_reported_up = false;
        ecm->config_attempts = 0;
        ecm->config_next_try_tick = 0;
        ecm->connected_tick = xTaskGetTickCount();
        ecm->port_opening = false;
        pending_notif = ecm->pending_notif_valid;
        pending_notif_connected = ecm->pending_notif_connected;
        ecm->pending_notif_valid = false;
        if (pending_notif) {
            /* Apply the notification that raced with the port opening: defer
             * the link report until the interface configuration completes. */
            ecm->last_connected = pending_notif_connected;
            ecm->has_pending_link = true;
            ecm->pending_link_up = (pending_notif_connected != 0);
        }
        ECM_UNLOCK(ecm);

        xEventGroupSetBits(ecm->event_group, ECM_DEV_CONNECTED);

        xEventGroupSetBits(ecm->event_group, ECM_DEV_CONNECTED);
    } break;

    case CDC_HOST_DEVICE_EVENT_DISCONNECTED: {
        bool matched;
        ECM_LOCK(ecm);
        matched = (ecm->cdc_dev_hdl != NULL && ecm->cdc_dev_hdl == event_data->dev_gone.dev_hdl) ||
                  ((ecm->cdc_dev_addr != 0) && (ecm->cdc_dev_addr == event_data->dev_gone.dev_addr));
        if (matched) {
            // The CDC driver closes the port itself after this callback returns. Forget the
            // handle here so no API is called on it anymore.
            ecm->cdc_port = NULL;
            ecm->cdc_dev_hdl = NULL;
            ecm->cdc_dev_addr = 0;
            ecm->dev_connect_status = false;
            ecm->intf_configured = false;
            ecm->has_pending_link = false;
            ecm->pending_link_up = false;
            ecm->mac_valid = false;
            ecm->last_connected = -1;
            memset(ecm->eth_mac_addr, 0, sizeof(ecm->eth_mac_addr));
        }
        ECM_UNLOCK(ecm);
        if (matched) {
            // Report link down unconditionally (the peer is gone)
            ECM_LOCK(ecm);
            ecm->link_reported_up = false;
            ECM_UNLOCK(ecm);
            if (ecm->mediator) {
                iot_eth_link_t link = IOT_ETH_LINK_DOWN;
                ecm->mediator->on_stage_changed(ecm->mediator, IOT_ETH_STAGE_LINK, &link);
            }
        }
    } break;

    default:
        break;
    }
}

static void _usbh_ecm_task(void *arg)
{
    usbh_ecm_t *ecm = (usbh_ecm_t *)arg;
    while (1) {
        uint32_t events = xEventGroupWaitBits(ecm->event_group, ECM_EVENT_ALL, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        if (events & ECM_STOP) {
            ESP_LOGI(TAG, "USB ECM stop");
            break;
        }
        if (events & ECM_DEV_CONNECTED) {
            ECM_LOCK(ecm);
            ecm->config_attempts = 0;
            ecm->config_next_try_tick = 0;
            ecm->connected_tick = xTaskGetTickCount();
            ECM_UNLOCK(ecm);
            _ecm_config_attempt(ecm);
        } else {
            // Retry the interface configuration after a transient failure
            bool retry = false;
            ECM_LOCK(ecm);
            if (ecm->dev_connect_status && !ecm->intf_configured && !ecm->deiniting &&
                    ecm->config_attempts > 0 && ecm->config_attempts < ECM_CONFIG_RETRY_COUNT &&
                    (int32_t)(xTaskGetTickCount() - ecm->config_next_try_tick) >= 0) {
                retry = true;
            }
            ECM_UNLOCK(ecm);
            if (retry) {
                _ecm_config_attempt(ecm);
            }
        }
#ifdef CONFIG_ECM_AUTO_LINK_UP_AFTER_NO_NOTIFICATION
        // Only allow the auto link-up when the interface was configured successfully and the
        // device is still connected; never fake a link-up on a broken data path.
        bool auto_link_up = false;
        ECM_LOCK(ecm);
        if (ecm->dev_connect_status && ecm->intf_configured && !ecm->deiniting &&
                ecm->last_connected == -1 &&
                (xTaskGetTickCount() - ecm->connected_tick) >= pdMS_TO_TICKS(CONFIG_ECM_AUTO_LINK_UP_WAIT_TIME_MS)) {
            ecm->last_connected = 1;
            auto_link_up = true;
        }
        ECM_UNLOCK(ecm);
        if (auto_link_up) {
            ESP_LOGW(TAG, "ECM network connection not notified after %dms of connection, assume link is up", CONFIG_ECM_AUTO_LINK_UP_WAIT_TIME_MS);
            _ecm_report_link(ecm, true);
        }
#endif

        /* The CDC driver stops polling after repeated transfer errors. Without
         * this watchdog the WAN data path dies silently while the link still
         * looks "up" (review P2-7). */
        ECM_LOCK(ecm);
        usbh_cdc_port_handle_t poll_port = ecm->cdc_port;
        bool polling_expected = ecm->intf_configured && !ecm->deiniting;
        ECM_UNLOCK(ecm);
        if (poll_port != NULL && polling_expected) {
            usbh_cdc_port_status_t port_status = USBH_CDC_PORT_STATUS_CLOSED;
            if (usbh_cdc_port_get_status(poll_port, &port_status) == ESP_OK &&
                    port_status == USBH_CDC_PORT_STATUS_ERROR) {
                ESP_LOGW(TAG, "CDC data polling stopped after transfer errors, restarting it");
                if (usbh_cdc_port_start_data(poll_port) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to restart the CDC data polling");
                }
            }
        }
    }
    ESP_LOGI(TAG, "USB ECM task exit");
    xEventGroupSetBits(ecm->event_group, ECM_STOP_DONE);
    vTaskDelete(NULL);
}

static esp_err_t usbh_ecm_set_mediator(iot_eth_driver_t *driver, iot_eth_mediator_t *mediator)
{
    usbh_ecm_t *ecm = __containerof(driver, usbh_ecm_t, base);
    if (ecm->lock) {
        ECM_LOCK(ecm);
        ecm->mediator = mediator;
        ECM_UNLOCK(ecm);
    } else {
        ecm->mediator = mediator;
    }
    return ESP_OK;
}

static esp_err_t usbh_ecm_init(iot_eth_driver_t *handle)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    usbh_ecm_t *ecm = __containerof(handle, usbh_ecm_t, base);

    ecm->event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(ecm->event_group != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create the event group");

    ecm->lock = xSemaphoreCreateMutex();
    if (ecm->lock == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto err;
    }
    ecm->deiniting = false;

    // The registration also replays the devices that are already connected, so the event group
    // and the lock must be ready before this call.
    ret = usbh_cdc_register_dev_event_cb(ecm->config.match_id_list, _usbh_ecm_dev_event_cb, ecm);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register the CDC device event callback: %s", esp_err_to_name(ret));
        goto err;
    }

    BaseType_t task_created = xTaskCreate(_usbh_ecm_task, "_usbh_ecm_task", 4096, ecm, 5, &ecm->task_h);
    if (task_created != pdPASS) {
        ret = ESP_FAIL;
        goto err_unregister;
    }

    ecm->mediator->on_stage_changed(ecm->mediator, IOT_ETH_STAGE_LL_INIT, NULL);
    ESP_LOGI(TAG, "USB ECM network interface init success");
    return ESP_OK;

err_unregister:
    usbh_cdc_unregister_dev_event_cb(_usbh_ecm_dev_event_cb);
    if (ecm->cdc_port) {
        usbh_cdc_port_close(ecm->cdc_port);
        ecm->cdc_port = NULL;
    }
err:
    if (ecm->task_h) {
        vTaskDelete(ecm->task_h);
        ecm->task_h = NULL;
    }
    if (ecm->lock) {
        vSemaphoreDelete(ecm->lock);
        ecm->lock = NULL;
    }
    if (ecm->event_group) {
        vEventGroupDelete(ecm->event_group);
        ecm->event_group = NULL;
    }
    return ret;
}

esp_err_t usbh_ecm_deinit(iot_eth_driver_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    usbh_ecm_t *ecm = __containerof(handle, usbh_ecm_t, base);
    if (ecm->lock == NULL) {
        // Already deinitialized (or the init failed before creating the lock)
        return ESP_OK;
    }

    ECM_LOCK(ecm);
    bool already_deiniting = ecm->deiniting;
    ecm->deiniting = true;
    ECM_UNLOCK(ecm);
    if (already_deiniting) {
        return ESP_OK;
    }

    // 1) Stop the ECM task first: it may be inside _ecm_config_intf() and must not touch the
    //    port anymore.
    if (ecm->event_group && ecm->task_h) {
        xEventGroupSetBits(ecm->event_group, ECM_STOP);
        EventBits_t bits = xEventGroupWaitBits(ecm->event_group, ECM_STOP_DONE, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(ECM_STOP_TIMEOUT_MS));
        if ((bits & ECM_STOP_DONE) == 0) {
            // Do NOT free the object while the task may still be running: leave it allocated
            // (and the port open) so a retry of the teardown can be attempted safely.
            ESP_LOGE(TAG, "Timed out waiting for the ECM task to stop, aborting the deinit");
            ECM_LOCK(ecm);
            ecm->deiniting = false;
            ECM_UNLOCK(ecm);
            return ESP_ERR_TIMEOUT;
        }
        ecm->task_h = NULL;
    }

    // 2) Unregister the device event callback (waits for any in-flight dispatch) before closing
    //    the port, so no new port is opened during the teardown.
    esp_err_t ret = usbh_cdc_unregister_dev_event_cb(_usbh_ecm_dev_event_cb);
    if (ret == ESP_ERR_TIMEOUT) {
        // A callback may still be executing with this object: do not free it.
        ESP_LOGE(TAG, "The device event callbacks did not drain, aborting the deinit");
        ECM_LOCK(ecm);
        ecm->deiniting = false;
        ECM_UNLOCK(ecm);
        return ret;
    }
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to unregister the CDC device event callback: %s", esp_err_to_name(ret));
    }

    // 3) Close the CDC port. usbh_cdc_port_close() clears the port callbacks and waits for a
    //    running callback to return, so after this call no callback can reference this object.
    usbh_cdc_port_handle_t cdc_port;
    ECM_LOCK(ecm);
    cdc_port = ecm->cdc_port;
    ecm->cdc_port = NULL;
    ECM_UNLOCK(ecm);
    if (cdc_port != NULL) {
        esp_err_t err = usbh_cdc_port_close(cdc_port);
        if (err == ESP_ERR_TIMEOUT) {
            // A port callback is still using this object: do not free it.
            ESP_LOGE(TAG, "The CDC port callback did not drain, aborting the deinit");
            ECM_LOCK(ecm);
            ecm->deiniting = false;
            ECM_UNLOCK(ecm);
            return err;
        }
        if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
            ESP_LOGE(TAG, "Failed to close the CDC port: %s", esp_err_to_name(err));
        }
    }

    // 4) Now the object is no longer referenced by the driver: release it.
    ECM_LOCK(ecm);
    ecm->mediator = NULL;
    ECM_UNLOCK(ecm);
    vEventGroupDelete(ecm->event_group);
    ecm->event_group = NULL;
    vSemaphoreDelete(ecm->lock);
    ecm->lock = NULL;
    free(ecm);
    return ESP_OK;
}

esp_err_t usbh_ecm_get_addr(iot_eth_driver_t *handle, uint8_t *mac_addr)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(mac_addr != NULL, ESP_ERR_INVALID_ARG, TAG, "mac_addr is NULL");
    usbh_ecm_t *ecm = __containerof(handle, usbh_ecm_t, base);
    ESP_RETURN_ON_FALSE(ecm->lock != NULL, ESP_ERR_INVALID_STATE, TAG, "ECM driver is not initialized");
    ECM_LOCK(ecm);
    bool valid = ecm->dev_connect_status && ecm->mac_valid;
    uint8_t mac[6];
    memcpy(mac, ecm->eth_mac_addr, sizeof(mac));
    ECM_UNLOCK(ecm);
    ESP_RETURN_ON_FALSE(valid, ESP_ERR_INVALID_STATE, TAG, "ECM MAC address not available");
    memcpy(mac_addr, mac, sizeof(mac));
    return ESP_OK;
}

esp_err_t iot_eth_new_usb_ecm(const iot_usbh_ecm_config_t *config, iot_eth_driver_t **ret_handle)
{
    ESP_LOGI(TAG, "IOT USBH ECM Version: %d.%d.%d", IOT_USBH_ECM_VER_MAJOR, IOT_USBH_ECM_VER_MINOR, IOT_USBH_ECM_VER_PATCH);
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(ret_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "ret_handle is NULL");
    ESP_RETURN_ON_FALSE(config->match_id_list != NULL, ESP_ERR_INVALID_ARG, TAG, "match_id_list is NULL");

    usbh_ecm_t *ecm = calloc(1, sizeof(usbh_ecm_t));
    ESP_RETURN_ON_FALSE(ecm != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for usbh_ecm_t");

    memcpy(&ecm->config, config, sizeof(iot_usbh_ecm_config_t));
    ecm->base.name = "usb_ecm";
    ecm->base.init = usbh_ecm_init;
    ecm->base.set_mediator = usbh_ecm_set_mediator;
    ecm->base.deinit = usbh_ecm_deinit;
    ecm->base.start = NULL;
    ecm->base.stop = NULL;
    ecm->base.transmit = usbh_ecm_transmit;
    ecm->base.get_addr = usbh_ecm_get_addr;

    *ret_handle = (iot_eth_driver_t *)&ecm->base;
    return ESP_OK;
}

usbh_cdc_port_handle_t usb_ecm_get_cdc_port_handle(const iot_eth_driver_t *ecm_drv)
{
    ESP_RETURN_ON_FALSE(ecm_drv != NULL, NULL, TAG, "ecm is NULL");
    usbh_ecm_t *ecm = __containerof(ecm_drv, usbh_ecm_t, base);
    if (ecm->lock == NULL) {
        return NULL;
    }
    ECM_LOCK(ecm);
    usbh_cdc_port_handle_t port = ecm->cdc_port;
    ECM_UNLOCK(ecm);
    return port;
}
