/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_check.h"
#include "usb/usb_helpers.h"
#include "iot_usbh_descriptor.h"
#include "usb/usb_types_ch9.h"
#include "iot_usbh_cdc_type.h"

static const char *TAG = "cdc_descriptor";

#define USB_SUBCLASS_NULL          0x00
#define USB_SUBCLASS_COMMON        0x02
#define USB_PROTOCOL_NULL          0x00
#define USB_DEVICE_PROTOCOL_IAD    0x01

/* Maximum number of candidate data interfaces we track (USB allows 255 interfaces per config,
 * a CDC function realistically has a handful). Extra candidates are ignored. */
#define CDC_MAX_DATA_INTF_CANDIDATES 8

typedef struct {
    uint8_t bFunctionLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
} cdc_cs_intf_header_t;

/**
 * @brief Minimal layout of the Union Functional Descriptor.
 *
 * The descriptor has a variable number of subordinate interface fields, so we only rely on
 * the fields that are guaranteed once bFunctionLength is at least 5.
 */
typedef struct {
    uint8_t bFunctionLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bControlInterface;
    uint8_t bSubordinateInterface0;
} cdc_union_func_desc_min_t;

/**
 * @brief Searches interface by index and verifies its CDC-compliance
 *
 * @param[in] device_desc Pointer to Device descriptor
 * @param[in] config_desc Pointer to Configuration descriptor
 * @param[in] intf_idx    bInterfaceNumber of the required interface
 * @return true  The required interface is CDC compliant
 * @return false The required interface is NOT CDC compliant
 */
static bool cdc_parse_is_cdc_compliant(const usb_device_desc_t *device_desc, const usb_config_desc_t *config_desc, uint8_t intf_idx)
{
    int desc_offset = 0;

    if (device_desc->bDeviceClass == USB_CLASS_PER_INTERFACE || device_desc->bDeviceClass == USB_CLASS_COMM) {
        const usb_intf_desc_t *intf_desc = usb_parse_interface_descriptor(config_desc, intf_idx, 0, NULL);
        if (intf_desc && intf_desc->bInterfaceClass == USB_CLASS_COMM) {
            // 1. This is a Communication Device Class: Class defined in Interface descriptor
            return true;
        }
    }

    if (((device_desc->bDeviceClass == USB_CLASS_MISC) && (device_desc->bDeviceSubClass == USB_SUBCLASS_COMMON) &&
            (device_desc->bDeviceProtocol == USB_DEVICE_PROTOCOL_IAD)) ||
            ((device_desc->bDeviceClass == USB_CLASS_PER_INTERFACE) && (device_desc->bDeviceSubClass == USB_SUBCLASS_NULL) &&
             (device_desc->bDeviceProtocol == USB_PROTOCOL_NULL))) {
        const usb_standard_desc_t *this_desc = (const usb_standard_desc_t *)config_desc;
        while ((this_desc = usb_parse_next_descriptor_of_type(this_desc, config_desc->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION, &desc_offset))) {
            const usb_iad_desc_t *iad_desc = (const usb_iad_desc_t *)this_desc;
            // Interface numbers are only required to be unique, they are not necessarily
            // contiguous. Do not assume "control interface + 1 == data interface", instead check
            // whether the interface belongs to the association. Also accept associations with
            // more than 2 interfaces (e.g. ECM + ACM composite functions).
            if ((iad_desc->bFirstInterface <= intf_idx) &&
                    (intf_idx < (int)(iad_desc->bFirstInterface + iad_desc->bInterfaceCount)) &&
                    (iad_desc->bFunctionClass == USB_COMMUNICATIONS_DEVICE_CLASS)) {
                // 2. This is a composite device, that uses Interface Association Descriptor
                return true;
            }
        };
    }
    return false;
}

/**
 * @brief Find the Union Functional Descriptor of a CDC control interface.
 *
 * Only descriptors that directly follow the control interface descriptor and belong to it
 * (i.e. before the next INTERFACE descriptor) are considered.
 */
static const cdc_union_func_desc_min_t *cdc_find_union_desc(const usb_config_desc_t *config_desc, const usb_intf_desc_t *control_intf, int control_offset)
{
    int desc_offset = control_offset;
    const usb_standard_desc_t *this_desc = (const usb_standard_desc_t *)control_intf;

    while ((this_desc = usb_parse_next_descriptor(this_desc, config_desc->wTotalLength, &desc_offset))) {
        if (this_desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            // Next interface reached, the union descriptor must be before it
            break;
        }
        if (this_desc->bDescriptorType != USB_CS_DESCRIPTOR_TYPE_INTERFACE ||
                this_desc->bLength < sizeof(cdc_cs_intf_header_t)) {
            continue;
        }
        const cdc_cs_intf_header_t *cs_desc = (const cdc_cs_intf_header_t *)this_desc;
        if (cs_desc->bDescriptorSubtype != USB_CS_DESCRIPTOR_SUBTYPE_UNION_FUNCTION) {
            continue;
        }
        if (this_desc->bLength < sizeof(cdc_union_func_desc_min_t)) {
            ESP_LOGW(TAG, "Union functional descriptor too short (%d)", this_desc->bLength);
            continue;
        }
        return (const cdc_union_func_desc_min_t *)this_desc;
    }
    return NULL;
}

/**
 * @brief Collect the candidate data interfaces of a CDC control interface.
 *
 * Preference order:
 *   1. The subordinate interfaces of the Union Functional Descriptor (spec compliant).
 *   2. Any interface with bInterfaceClass == USB_DATA_INTERFACE_CLASS (fallback for devices
 *      that do not provide a Union descriptor).
 */
static int cdc_collect_data_intf_candidates(const usb_config_desc_t *config_desc, const usb_intf_desc_t *control_intf,
                                            int control_offset, uint8_t *candidates, int max_candidates)
{
    int num_candidates = 0;
    const cdc_union_func_desc_min_t *union_desc = cdc_find_union_desc(config_desc, control_intf, control_offset);

    if (union_desc) {
        // bControlInterface is at offset 3, the subordinate interfaces start at offset 4
        // (bSubordinateInterface0). Use offsetof() so the first subordinate is not skipped, and
        // clamp the end to bFunctionLength, the descriptor's bLength and the config buffer.
        const size_t first = offsetof(cdc_union_func_desc_min_t, bSubordinateInterface0);
        size_t limit = union_desc->bFunctionLength;
        const size_t b_length = ((const usb_standard_desc_t *)union_desc)->bLength;
        if (limit > b_length) {
            limit = b_length;
        }
        if (limit > config_desc->wTotalLength) {
            limit = config_desc->wTotalLength;
        }
        for (size_t i = first; i < limit && num_candidates < max_candidates; i++) {
            uint8_t subordinate = ((const uint8_t *)union_desc)[i];
            if (subordinate != control_intf->bInterfaceNumber) {
                candidates[num_candidates++] = subordinate;
            }
        }
        if (num_candidates > 0) {
            return num_candidates;
        }
    }

    // Fallback: scan the configuration for data class interfaces
    int desc_offset = 0;
    const usb_standard_desc_t *this_desc = (const usb_standard_desc_t *)config_desc;
    while ((this_desc = usb_parse_next_descriptor_of_type(this_desc, config_desc->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &desc_offset))) {
        if (this_desc->bLength < sizeof(usb_intf_desc_t)) {
            continue;
        }
        const usb_intf_desc_t *intf_desc = (const usb_intf_desc_t *)this_desc;
        if (intf_desc->bAlternateSetting != 0) {
            continue;
        }
        if (intf_desc->bInterfaceClass != USB_DATA_INTERFACE_CLASS) {
            continue;
        }
        if (intf_desc->bInterfaceNumber == control_intf->bInterfaceNumber) {
            continue;
        }
        bool already_added = false;
        for (int i = 0; i < num_candidates; i++) {
            if (candidates[i] == intf_desc->bInterfaceNumber) {
                already_added = true;
                break;
            }
        }
        if (!already_added) {
            candidates[num_candidates++] = intf_desc->bInterfaceNumber;
            if (num_candidates >= max_candidates) {
                break;
            }
        }
    }
    return num_candidates;
}

/**
 * @brief Find the data interface (and its active alternate setting endpoints).
 *
 * The USB specification does not require the data interface number to be the control interface
 * number + 1, so the data interface is located with the CDC Union functional descriptor (or, as
 * a fallback, by scanning the configuration for a data class interface).
 */
static esp_err_t cdc_find_data_interface(const usb_config_desc_t *config_desc, const usb_intf_desc_t *control_intf,
                                         int control_offset, usbh_cdc_parsed_info_t *info_ret)
{
    uint8_t candidates[CDC_MAX_DATA_INTF_CANDIDATES];
    int num_candidates = cdc_collect_data_intf_candidates(config_desc, control_intf, control_offset,
                                                          candidates, CDC_MAX_DATA_INTF_CANDIDATES);
    if (num_candidates == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    for (int cand = 0; cand < num_candidates; cand++) {
        int num_of_alternate = usb_parse_interface_number_of_alternate(config_desc, candidates[cand]);
        if (num_of_alternate < 0) {
            continue;
        }
        for (int alt = 0; alt <= num_of_alternate; alt++) {
            int intf_offset = 0;
            const usb_intf_desc_t *data_intf = usb_parse_interface_descriptor(config_desc, candidates[cand], alt, &intf_offset);
            if (data_intf == NULL) {
                continue;
            }
            const usb_ep_desc_t *in_ep = NULL;
            const usb_ep_desc_t *out_ep = NULL;
            for (int i = 0; i < data_intf->bNumEndpoints; i++) {
                int ep_offset = intf_offset;
                const usb_ep_desc_t *this_ep = usb_parse_endpoint_descriptor_by_index(data_intf, i, config_desc->wTotalLength, &ep_offset);
                if (this_ep == NULL) {
                    continue;
                }
                if (USB_EP_DESC_GET_XFERTYPE(this_ep) != USB_TRANSFER_TYPE_BULK) {
                    continue;
                }
                if (USB_EP_DESC_GET_EP_DIR(this_ep)) {
                    in_ep = this_ep;
                } else {
                    out_ep = this_ep;
                }
            }
            if (in_ep && out_ep) {
                info_ret->data_intf = data_intf;
                info_ret->in_ep = in_ep;
                info_ret->out_ep = out_ep;
                ESP_LOGI(TAG, "Found data interface %d (alternate setting %d)", data_intf->bInterfaceNumber, data_intf->bAlternateSetting);
                return ESP_OK;
            }
        }
        ESP_LOGD(TAG, "Interface %d has no alternate setting with BULK IN and OUT endpoints", candidates[cand]);
    }
    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief Parse CDC interface descriptor and return interface information.
 *        this function not only parse the standard CDC interface descriptor, but also parse the vendor specific CDC interface descriptor.
 *        If the interface is a CDC Notification interface, return both Notification and Data interfaces.
 *        If the interface is a CDC Data interface, return only the Data interface.
 *
 * There are several types of CDC interface:
 *   1. Standard CDC-> [
 *                      IAD Descriptor,
 *                      notification interface: [interrupt endpoint],
 *                      Some class specific descriptor,
 *                      data interface: [in bulk endpoint, out bulk endpoint]
 *                      ]
 *   2. Vendor specific CDC type1 -> [
 *                                      specific interface: [
 *                                          Some class specific descriptor,
 *                                          interrupt endpoint, // for notification
 *                                          in bulk endpoint,
 *                                          out bulk endpoint
 *                                       ]
 *                                    ]
 *   3. Vendor specific CDC type2 -> [
 *                                      specific interface: [
 *                                          in bulk endpoint,
 *                                          out bulk endpoint
 *                                       ]
 *                                    ]
 *
 * @param device_desc Pointer to USB device descriptor
 * @param config_desc Pointer to USB configuration descriptor
 * @param intf_number bInterfaceNumber of the control interface to parse
 * @param info_ret Pointer to store the parsed interface information
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t cdc_parse_interface_descriptor(const usb_device_desc_t *device_desc, const usb_config_desc_t *config_desc, uint8_t intf_number, usbh_cdc_parsed_info_t *info_ret)
{
    ESP_RETURN_ON_FALSE(device_desc != NULL && config_desc != NULL && info_ret != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    memset(info_ret, 0, sizeof(usbh_cdc_parsed_info_t));

    int intf_offset = 0;
    const usb_intf_desc_t *first_intf_desc = usb_parse_interface_descriptor(config_desc, intf_number, 0, &intf_offset);
    ESP_RETURN_ON_FALSE(first_intf_desc, ESP_ERR_NOT_FOUND, TAG, "Required interface no %d was not found.", intf_number);

    if (first_intf_desc->bInterfaceClass == USB_CLASS_VENDOR_SPEC) {
        if (3 == first_intf_desc->bNumEndpoints) {
            // if there are 3 endpoints, check if the first one is a CDC Header descriptor. It is a relatively strict inspection
            int desc_offset = intf_offset;
            const usb_standard_desc_t *this_desc = (const usb_standard_desc_t *)first_intf_desc;
            this_desc = usb_parse_next_descriptor_of_type(this_desc, config_desc->wTotalLength, USB_CS_DESCRIPTOR_TYPE_INTERFACE, &desc_offset);
            if (this_desc && this_desc->bLength >= sizeof(usb_cdc_header_func_desc_t)) {
                const usb_cdc_header_func_desc_t *cdc_header = (const usb_cdc_header_func_desc_t *)this_desc;
                ESP_RETURN_ON_FALSE(cdc_header->bDescriptorSubtype == USB_CS_DESCRIPTOR_SUBTYPE_HEADER, ESP_ERR_NOT_FOUND, TAG,
                                    "interface no %d is not a supported CDC interface.", intf_number);
                ESP_RETURN_ON_FALSE(cdc_header->bcdCDC == 0x0110, ESP_ERR_NOT_FOUND, TAG, "interface no %d is not a supported CDC interface.", intf_number);
            }
        }

        for (int i = 0; i < first_intf_desc->bNumEndpoints; i++) {
            int ep_offset = intf_offset;
            const usb_ep_desc_t *this_ep = usb_parse_endpoint_descriptor_by_index(first_intf_desc, i, config_desc->wTotalLength, &ep_offset);
            if (this_ep == NULL) {
                continue;
            }
            if (USB_EP_DESC_GET_XFERTYPE(this_ep) == USB_TRANSFER_TYPE_INTR) {
                info_ret->notif_intf = first_intf_desc;
                info_ret->notif_ep = this_ep;
            } else if (USB_EP_DESC_GET_XFERTYPE(this_ep) == USB_TRANSFER_TYPE_BULK) {
                info_ret->data_intf = first_intf_desc;
                if (USB_EP_DESC_GET_EP_DIR(this_ep)) {
                    info_ret->in_ep = this_ep;
                } else {
                    info_ret->out_ep = this_ep;
                }
            }
        }
    } else {
        const bool cdc_compliant = cdc_parse_is_cdc_compliant(device_desc, config_desc, intf_number);
        if (cdc_compliant) {
            info_ret->notif_intf = first_intf_desc; // We make sure that intf_desc is set for CDC compliant devices that use EP0 as notification element
            // Find the notification (interrupt IN) endpoint of the control interface. Some devices
            // have a control interface without any interrupt endpoint, which is allowed.
            for (int i = 0; i < first_intf_desc->bNumEndpoints; i++) {
                int ep_offset = intf_offset;
                const usb_ep_desc_t *this_ep = usb_parse_endpoint_descriptor_by_index(first_intf_desc, i, config_desc->wTotalLength, &ep_offset);
                if (this_ep == NULL) {
                    continue;
                }
                if (USB_EP_DESC_GET_XFERTYPE(this_ep) == USB_TRANSFER_TYPE_INTR &&
                        USB_EP_DESC_GET_EP_DIR(this_ep)) {
                    info_ret->notif_ep = this_ep;
                    break;
                }
            }
            if (info_ret->notif_ep == NULL) {
                ESP_LOGD(TAG, "No notification endpoint found for interface %d", intf_number);
            }

            esp_err_t err = cdc_find_data_interface(config_desc, first_intf_desc, intf_offset, info_ret);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not find the CDC data interface of control interface %d", intf_number);
                return err;
            }
        } else {
            ESP_LOGE(TAG, "Interface %d is not CDC compliant", intf_number);
            return ESP_ERR_NOT_FOUND;
        }
    }

    if (info_ret->notif_ep) {
        ESP_LOGI(TAG, "Found NOTIF endpoint: %d", USB_EP_DESC_GET_EP_NUM(info_ret->notif_ep));
    }
    if (info_ret->out_ep) {
        ESP_LOGI(TAG, "Found OUT endpoint: %d", USB_EP_DESC_GET_EP_NUM(info_ret->out_ep));
    }
    if (info_ret->in_ep) {
        ESP_LOGI(TAG, "Found IN endpoint: %d", USB_EP_DESC_GET_EP_NUM(info_ret->in_ep));
    }
    // If we did not find IN and OUT data endpoints, the device cannot be used
    return (info_ret->in_ep && info_ret->out_ep && info_ret->data_intf) ? ESP_OK : ESP_ERR_NOT_FOUND;
}
