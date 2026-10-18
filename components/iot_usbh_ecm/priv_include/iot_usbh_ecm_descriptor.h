/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "usb/usb_host.h"
#include "iot_usbh_cdc_type.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Ethernet Networking Functional Descriptor of a CDC ECM function.
 *
 * The layout is shared with the CDC component (`usb_cdc_ethernet_networking_func_desc_t`),
 * there is no need to duplicate it here.
 */
typedef usb_cdc_ethernet_networking_func_desc_t usb_ecm_function_desc_t;

/**
 * @brief Check for the presence of an ECM control interface.
 *
 * This function scans the configuration descriptor for an interface with class
 * USB_COMMUNICATIONS_INTERFACE_CLASS, subclass
 * USB_COMMUNICATIONS_INTERFACE_SUBCLASS_ETHERNET_NETWORKING_CONTROL_MODEL and protocol 0x00.
 *
 * @param device_desc Pointer to the USB device descriptor (unused, reserved for future use).
 * @param config_desc Pointer to the USB configuration descriptor.
 * @param itf_num Pointer to an integer where the control interface number will be stored if found.
 *
 * @return
 *     - ESP_OK: ECM control interface found
 *     - ESP_ERR_INVALID_ARG: Invalid argument
 *     - ESP_ERR_NOT_FOUND: ECM control interface not found
 */
esp_err_t usbh_ecm_interface_check(const usb_device_desc_t *device_desc, const usb_config_desc_t *config_desc, int *itf_num);

/**
 * @brief Get the Ethernet Networking Functional Descriptor of the ECM function.
 *
 * Only the class-specific descriptors that belong to the given ECM control interface are
 * searched, so a composite device (e.g. ECM + ACM) cannot return the descriptor of another
 * function. The descriptor length is validated before the pointer is returned.
 *
 * @param[in] device_desc Pointer to USB device descriptor (unused, reserved for future use)
 * @param[in] config_desc Pointer to USB configuration descriptor
 * @param[in] control_itf_num Interface number of the ECM control interface
 * @param[out] ret_desc Pointer to store the found ECM function descriptor
 *
 * @return
 *      - ESP_OK: ECM function descriptor found and stored successfully
 *      - ESP_ERR_INVALID_ARG: Invalid argument
 *      - ESP_ERR_NOT_FOUND: ECM function descriptor not found
 */
esp_err_t usbh_get_ecm_function_desc(const usb_device_desc_t *device_desc, const usb_config_desc_t *config_desc,
                                     uint8_t control_itf_num, const usb_ecm_function_desc_t **ret_desc);

#ifdef __cplusplus
}
#endif
