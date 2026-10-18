/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "iot_usbh_cdc_type.h"
#include "iot_usbh_ecm_descriptor.h"

static const char *TAG = "usbh_ecm_desc";

#define ECM_FUNC_DESC_MIN_LEN  13  /*!< bLength of the Ethernet Networking Functional Descriptor */

typedef struct {
    uint8_t bFunctionLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
} cdc_cs_intf_header_t;

esp_err_t usbh_ecm_interface_check(const usb_device_desc_t *device_desc, const usb_config_desc_t *config_desc, int *itf_num)
{
    (void)device_desc;
    ESP_RETURN_ON_FALSE(config_desc != NULL && itf_num != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");

    size_t total_length = config_desc->wTotalLength;
    int desc_offset = 0;

    // Get first Interface descriptor
    const usb_standard_desc_t *current_desc = (const usb_standard_desc_t *)config_desc;
    while ((current_desc = usb_parse_next_descriptor_of_type(current_desc, total_length, USB_B_DESCRIPTOR_TYPE_INTERFACE, &desc_offset))) {
        if (current_desc->bLength < sizeof(usb_intf_desc_t)) {
            continue;
        }
        const usb_intf_desc_t *intf_desc = (const usb_intf_desc_t *)current_desc;
        if ((intf_desc->bInterfaceClass == USB_COMMUNICATIONS_INTERFACE_CLASS) &&
                (intf_desc->bInterfaceSubClass == USB_COMMUNICATIONS_INTERFACE_SUBCLASS_ETHERNET_NETWORKING_CONTROL_MODEL) &&
                (intf_desc->bInterfaceProtocol == USB_COMMUNICATIONS_INTERFACE_PROTOCOL_NONE)) {

            *itf_num = intf_desc->bInterfaceNumber;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t usbh_get_ecm_function_desc(const usb_device_desc_t *device_desc, const usb_config_desc_t *config_desc,
                                     uint8_t control_itf_num, const usb_ecm_function_desc_t **ret_desc)
{
    (void)device_desc;
    ESP_RETURN_ON_FALSE(config_desc != NULL && ret_desc != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    *ret_desc = NULL;

    // Locate the ECM control interface first; the functional descriptors of the function must
    // directly follow it and belong to it.
    int intf_offset = 0;
    const usb_intf_desc_t *control_intf = usb_parse_interface_descriptor(config_desc, control_itf_num, 0, &intf_offset);
    ESP_RETURN_ON_FALSE(control_intf != NULL, ESP_ERR_NOT_FOUND, TAG, "ECM control interface %d not found", control_itf_num);

    int desc_offset = intf_offset;
    const usb_standard_desc_t *current_desc = (const usb_standard_desc_t *)control_intf;
    while ((current_desc = usb_parse_next_descriptor(current_desc, config_desc->wTotalLength, &desc_offset))) {
        if (current_desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            // Reached the next interface: the function descriptors of this interface are over
            break;
        }
        if (current_desc->bDescriptorType != USB_CS_DESCRIPTOR_TYPE_INTERFACE ||
                current_desc->bLength < sizeof(cdc_cs_intf_header_t)) {
            continue;
        }
        const cdc_cs_intf_header_t *cs_desc = (const cdc_cs_intf_header_t *)current_desc;
        if (cs_desc->bDescriptorSubtype != USB_CS_DESCRIPTOR_SUBTYPE_ETHERNET_NETWORKING) {
            continue;
        }
        if (current_desc->bLength < ECM_FUNC_DESC_MIN_LEN || cs_desc->bFunctionLength < ECM_FUNC_DESC_MIN_LEN) {
            ESP_LOGW(TAG, "Ethernet Networking descriptor too short (%d/%d)", current_desc->bLength, cs_desc->bFunctionLength);
            continue;
        }
        *ret_desc = (const usb_ecm_function_desc_t *)current_desc;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
