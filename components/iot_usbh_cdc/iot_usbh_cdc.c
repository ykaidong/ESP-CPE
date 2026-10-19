/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <inttypes.h>
#include <sys/queue.h>
#include "esp_err.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/event_groups.h"
#include "esp_private/usb_phy.h"
#include "iot_usbh_cdc.h"
#include "usb/usb_host.h"
#include "iot_usbh_descriptor.h"

static const char *TAG = "USBH_CDC";

// CDC spinlock, protects the linked lists and the flags of the port/device objects
static portMUX_TYPE cdc_lock =  portMUX_INITIALIZER_UNLOCKED;
#define CDC_ENTER_CRITICAL()    portENTER_CRITICAL(&cdc_lock)
#define CDC_EXIT_CRITICAL()     portEXIT_CRITICAL(&cdc_lock)

// CDC-ACM events
#define CDC_TEARDOWN            BIT0
#define CDC_TEARDOWN_COMPLETE   BIT1

#define TIMEOUT_USB_RINGBUF_MS  200                      /*! Timeout for ring buffer operate */
#define CDC_CTRL_TIMEOUT_MS     5000                     /*! Timeout for control transfer */
#define CDC_CLOSE_MUTEX_TIMEOUT_MS  5000                 /*! Max time to wait for the TX mutex when closing a port */
#define CDC_UNINSTALL_TIMEOUT_MS    5000                 /*! Max time to wait for the ports to be released */
#define CDC_TEARDOWN_TIMEOUT_MS     5000                 /*! Max time to wait for the client task to stop */
#define CDC_DEV_EVENT_DRAIN_MS      5000                 /*! Max time to wait for the device event callbacks */
#define CDC_MAX_DEV_ADDR            128                  /*! USB addresses are 7 bits wide (1..127) */

typedef struct dev_event_cb_s {
    usbh_cdc_device_event_callback_t cb;
    void *user_data;
    const usb_device_match_id_t *match_id_list;
    bool removed;
    uint32_t delivered_addr_bits[CDC_MAX_DEV_ADDR / 32];  /*! Addresses already delivered to this callback */
    SLIST_ENTRY(dev_event_cb_s) list_entry;
} dev_event_cb_t;

/**
 * @brief Context handed to the internal USB Host library task.
 *
 * It is owned by the task, so the task can free it before notifying the installer: the installer
 * may free the driver object when the notification times out, and the task must not touch any
 * driver memory after notifying.
 */
typedef struct {
    TaskHandle_t install_task;
    bool (*enum_filter_cb)(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue);
} usb_lib_task_ctx_t;

typedef struct {
    usb_host_client_handle_t cdc_client_hdl;             /*!< USB Host handle reused for all CDC-ACM devices in the system */
    EventGroupHandle_t event_group;
    SemaphoreHandle_t open_close_mutex;                  /*!< Serializes port open/close and device open/close */
    SemaphoreHandle_t dev_event_mutex;                   /*!< Protects dev_event_cb_list, free_pending_list and dispatch_active */
    SemaphoreHandle_t dispatch_idle_sem;                 /*!< Given when no device event callback is being dispatched */
    SLIST_HEAD(list_cb, dev_event_cb_s) dev_event_cb_list;
    SLIST_HEAD(list_dev_port, usbh_cdc_port_s) cdc_devices_port_list;   /*!< List of opened ports */
    SLIST_HEAD(list_free_port, usbh_cdc_port_s) free_pending_list;      /*!< Ports waiting for the deferred release */
    SLIST_HEAD(list_dev, usbh_cdc_dev_s) cdc_devices_list;   /*!< List of opened devices */
    bool dispatch_stopped;                               /*!< No more device event callbacks are dispatched */
    int dispatch_active;                                 /*!< Number of device event dispatching in progress */
} usbh_cdc_obj_t;

typedef struct usbh_cdc_dev_s {
    usb_device_handle_t dev_hdl;                  // USB device handle
    uint8_t dev_addr;                             // Device address
    struct {
        usb_transfer_t *xfer;
        SemaphoreHandle_t mux;                    // Serializes the control transfers of this device
        SemaphoreHandle_t done;                   // Given by control_xfer_cb on completion
        volatile bool pending;                    // A control transfer is in flight (protected by cdc_lock)
    } ctrl;

    SLIST_ENTRY(usbh_cdc_dev_s) list_entry;
} usbh_cdc_dev_t;

/**
 * define the USB CDC port structure
 */
typedef struct usbh_cdc_port_s {
    usbh_cdc_dev_t *cdc_dev; // pointer to the cdc device
    struct {
        usb_transfer_t *xfer;         // Notification transfer
        const usb_intf_desc_t *intf_desc;
    } notif;
    struct {
        usb_transfer_t *out_xfer;          // OUT data transfer
        SemaphoreHandle_t out_xfer_free_sem;
        SemaphoreHandle_t out_mux;
        usb_transfer_t *in_xfer;           // IN data transfer
        uint16_t in_mps;                   // IN endpoint Maximum Packet Size
        const usb_intf_desc_t *intf_desc;  // Pointer to data interface descriptor
    } data;

    int itf_num;                            /*!< Interface number of this CDC port */
    usbh_cdc_port_event_callbacks_t cbs; /*!< Callbacks for the CDC port */
    RingbufHandle_t in_ringbuf_handle;     /*!< in ringbuffer handle of corresponding interface */
    size_t in_ringbuf_size;
    RingbufHandle_t out_ringbuf_handle;    /*!< out ringbuffer handle of corresponding interface */
    size_t out_ringbuf_size;
    SLIST_ENTRY(usbh_cdc_port_s) list_entry;
    bool to_close;                       /*!< Flag to indicate if the port is to be closed */
    bool pending_free;                   /*!< The port is on the free_pending_list */
    bool interfaces_released;            /*!< The claimed USB interfaces were released */
    bool data_started;                   /*!< Flag to indicate if the BULK IN data polling has been started */
    bool rx_error;                       /*!< IN polling stopped after the retry budget was exhausted */
    bool notif_error;                    /*!< Notification polling stopped after the retry budget was exhausted */
    int xfer_inflight;                   /*!< Number of submitted URBs not yet retired (protected by cdc_lock) */
    int in_retry_count;
    int notif_retry_count;
    int free_retries;                    /*!< Number of deferred free attempts that failed */
    SemaphoreHandle_t cb_mutex;          /*!< Serializes the user callbacks with usbh_cdc_port_close() */
} usbh_cdc_port_t;

static usbh_cdc_obj_t *p_usbh_cdc_obj = NULL;
/* Set by usb_lib_task() when that task installed the USB Host library itself.
 * The driver-install error path uses it to roll the library back so a retry
 * starts from a clean state. The driver is a singleton, so a plain flag is
 * enough. */
static volatile bool s_usb_host_installed_by_driver = false;

static void _cdc_transfers_free(usbh_cdc_port_t *cdc_port);
static esp_err_t _cdc_dev_ctrl_allocate(usbh_cdc_dev_t *cdc_dev);
static esp_err_t _cdc_dev_ctrl_free(usbh_cdc_dev_t *cdc_dev);
static esp_err_t _cdc_tx_xfer_submit(usb_transfer_t *out_xfer);
static esp_err_t _cdc_submit_transfer(usbh_cdc_port_t *cdc_port, usb_transfer_t *xfer);

/*--------------------------------- CDC Buffer Handle Code [RINGBUF_TYPE_BYTEBUF] --------------------------------------*/
static size_t _get_ringbuf_len(RingbufHandle_t ringbuf_hdl)
{
    size_t uxItemsWaiting = 0;
    vRingbufferGetInfo(ringbuf_hdl, NULL, NULL, NULL, NULL, &uxItemsWaiting);
    return uxItemsWaiting;
}

static esp_err_t _ringbuf_pop(RingbufHandle_t ringbuf_hdl, uint8_t *buf, size_t req_bytes, size_t *read_bytes, TickType_t ticks_to_wait)
{
    uint8_t *buf_rcv = xRingbufferReceiveUpTo(ringbuf_hdl, read_bytes, ticks_to_wait, req_bytes);
    if (!buf_rcv) {
        return ESP_FAIL;
    }

    memcpy(buf, buf_rcv, *read_bytes);
    vRingbufferReturnItem(ringbuf_hdl, (void *)(buf_rcv));

    size_t read_bytes2 = 0;
    if (*read_bytes < req_bytes) {
        buf_rcv = xRingbufferReceiveUpTo(ringbuf_hdl, &read_bytes2, 0, req_bytes - *read_bytes);
        if (buf_rcv) {
            memcpy(buf + *read_bytes, buf_rcv, read_bytes2);
            *read_bytes += read_bytes2;
            vRingbufferReturnItem(ringbuf_hdl, (void *)(buf_rcv));
        }
    }

    return ESP_OK;
}

static esp_err_t _ringbuf_push(RingbufHandle_t ringbuf_hdl, const uint8_t *buf, size_t write_bytes, TickType_t ticks_to_wait)
{
    int res = xRingbufferSend(ringbuf_hdl, buf, write_bytes, ticks_to_wait);
    if (res != pdTRUE) {
        ESP_LOGW(TAG, "The ringbuffer is full, the data has been lost");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void _ringbuf_flush(RingbufHandle_t ringbuf_hdl)
{
    if (ringbuf_hdl == NULL) {
        return;
    }
    // The ring buffer may wrap/contain several segments, so drain it until it is empty.
    while (1) {
        size_t read_bytes = 0;
        size_t uxItemsWaiting = _get_ringbuf_len(ringbuf_hdl);
        if (uxItemsWaiting == 0) {
            break;
        }
        uint8_t *buf_rcv = xRingbufferReceiveUpTo(ringbuf_hdl, &read_bytes, 0, uxItemsWaiting);
        if (!buf_rcv) {
            break;
        }
        vRingbufferReturnItem(ringbuf_hdl, (void *)(buf_rcv));
        if (read_bytes == 0) {
            break;
        }
    }
}

static void usb_lib_task(void *arg)
{
    usb_lib_task_ctx_t *ctx = (usb_lib_task_ctx_t *)arg;
    // Own the context: copy what is needed and free it before notifying the installer, because
    // the installer may free the driver object when the notification times out.
    TaskHandle_t install_task = ctx->install_task;
    bool (*enum_filter_cb)(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue) = ctx->enum_filter_cb;
    free(ctx);

    esp_err_t ret = ESP_OK;
    usb_host_lib_info_t info;
    esp_err_t res = usb_host_lib_info(&info);
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "USB Host library installed by others, devices: %d, clients: %d", info.num_devices, info.num_clients);
    } else if (res == ESP_ERR_INVALID_STATE) {
        // Install USB Host driver. Should only be called once in entire application
        const usb_host_config_t host_config = {
            .skip_phy_setup = false,
            .intr_flags = ESP_INTR_FLAG_LEVEL1,
            .enum_filter_cb = enum_filter_cb,
        };
        ret = usb_host_install(&host_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install USB Host library: %s", esp_err_to_name(ret));
        } else {
            s_usb_host_installed_by_driver = true;
        }
    } else {
        ESP_LOGE(TAG, "usb_host_lib_info failed: %s", esp_err_to_name(res));
        ret = res;
    }

    // Signalize the usbh_cdc_driver_install(): 1 == success, 0 == failure
    xTaskNotify(install_task, (ret == ESP_OK) ? 1 : 0, eSetValueWithOverwrite);
    if (ret != ESP_OK) {
        /* Do not self-delete: the installer owns this task and deletes it while
         * rolling back the partial install. Self-deletion would leave the
         * installer with a dangling task handle. */
        vTaskSuspend(NULL);
        return;
    }

    bool has_clients = true;
    bool has_devices = false;
    while (has_clients) {
        uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_lib_handle_events failed: %s", esp_err_to_name(err));
            break;
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "Get FLAGS_NO_CLIENTS");
            if (ESP_OK == usb_host_device_free_all()) {
                ESP_LOGI(TAG, "All devices marked as free, no need to wait FLAGS_ALL_FREE event");
                has_clients = false;
            } else {
                ESP_LOGI(TAG, "Wait for the FLAGS_ALL_FREE");
                has_devices = true;
            }
        }
        if (has_devices && event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "Get FLAGS_ALL_FREE");
            has_clients = false;
        }
    }
    ESP_LOGI(TAG, "No more clients and devices, uninstall USB Host library");

    // Clean up USB Host
    vTaskDelay(100); // Short delay to allow clients clean-up
    esp_err_t err = usb_host_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_uninstall failed: %s", esp_err_to_name(err));
    }
    ESP_LOGD(TAG, "USB Host library is uninstalled");
    vTaskDelete(NULL);
}

/*-------------------------------------------- Internal list / flag helpers -------------------------------------------*/
static bool is_port_opened(usbh_cdc_port_t *cdc_port)
{
    if (p_usbh_cdc_obj == NULL || cdc_port == NULL) {
        return false;
    }
    bool found = false;
    CDC_ENTER_CRITICAL();
    if (!cdc_port->to_close) {
        usbh_cdc_port_t *tmp_cdc_port = NULL;
        SLIST_FOREACH(tmp_cdc_port, &p_usbh_cdc_obj->cdc_devices_port_list, list_entry) {
            if (tmp_cdc_port == cdc_port) {
                found = true;
                break;
            }
        }
    }
    CDC_EXIT_CRITICAL();
    return found;
}

static usbh_cdc_dev_t *get_cdc_dev_by_addr(uint8_t dev_addr)
{
    usbh_cdc_dev_t *current_dev = NULL;
    if (p_usbh_cdc_obj == NULL) {
        return NULL;
    }
    CDC_ENTER_CRITICAL();
    SLIST_FOREACH(current_dev, &p_usbh_cdc_obj->cdc_devices_list, list_entry) {
        if (current_dev->dev_addr == dev_addr) {
            break;
        }
    }
    CDC_EXIT_CRITICAL();
    return current_dev;
}

static uint8_t get_cdc_dev_addr_by_hdl(usb_device_handle_t dev_hdl)
{
    uint8_t dev_addr = 0;
    if (p_usbh_cdc_obj == NULL) {
        return 0;
    }
    CDC_ENTER_CRITICAL();
    usbh_cdc_dev_t *current_dev = NULL;
    SLIST_FOREACH(current_dev, &p_usbh_cdc_obj->cdc_devices_list, list_entry) {
        if (current_dev->dev_hdl == dev_hdl) {
            dev_addr = current_dev->dev_addr;
            break;
        }
    }
    CDC_EXIT_CRITICAL();
    return dev_addr;
}

/**
 * @brief Number of ports (open + pending release) that still reference a device
 *
 * @note The caller must hold open_close_mutex, which serializes this with port open/close.
 */
static size_t get_cdc_port_number_on_device(usbh_cdc_dev_t *cdc_dev)
{
    size_t num = 0;
    if (p_usbh_cdc_obj == NULL || cdc_dev == NULL) {
        return 0;
    }
    CDC_ENTER_CRITICAL();
    usbh_cdc_port_t *current_port = NULL;
    SLIST_FOREACH(current_port, &p_usbh_cdc_obj->cdc_devices_port_list, list_entry) {
        if (current_port->cdc_dev == cdc_dev) {
            num++;
        }
    }
    CDC_EXIT_CRITICAL();
    xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
    SLIST_FOREACH(current_port, &p_usbh_cdc_obj->free_pending_list, list_entry) {
        if (current_port->cdc_dev == cdc_dev) {
            num++;
        }
    }
    xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
    return num;
}

static bool _cdc_pending_free_empty(void)
{
    if (p_usbh_cdc_obj == NULL) {
        return true;
    }
    bool empty;
    xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
    empty = SLIST_EMPTY(&p_usbh_cdc_obj->free_pending_list);
    xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
    return empty;
}

static bool _cdc_devices_empty(void)
{
    if (p_usbh_cdc_obj == NULL) {
        return true;
    }
    bool empty;
    CDC_ENTER_CRITICAL();
    empty = SLIST_EMPTY(&p_usbh_cdc_obj->cdc_devices_list);
    CDC_EXIT_CRITICAL();
    return empty;
}

/*------------------------------------------ Deferred port release -----------------------------------------------------*/
/**
 * @brief Release the claimed interfaces and, if it is the last port, the device itself.
 *
 * @return true when the port resources can be destroyed, false when the release has to be retried.
 */
static bool _cdc_port_release_interfaces(usbh_cdc_port_t *cdc_port)
{
    if (cdc_port->interfaces_released) {
        return true;
    }
    usb_device_handle_t dev_hdl = cdc_port->cdc_dev ? cdc_port->cdc_dev->dev_hdl : NULL;
    if (dev_hdl == NULL) {
        cdc_port->interfaces_released = true;
        return true;
    }

    esp_err_t err = ESP_OK;
    if (cdc_port->data.intf_desc) {
        esp_err_t ret = usb_host_interface_release(p_usbh_cdc_obj->cdc_client_hdl, dev_hdl,
                                                   cdc_port->data.intf_desc->bInterfaceNumber);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to release data interface %d: %s",
                     cdc_port->data.intf_desc->bInterfaceNumber, esp_err_to_name(ret));
            err = ret;
        }
    }
    if (cdc_port->notif.intf_desc && cdc_port->notif.intf_desc != cdc_port->data.intf_desc) {
        esp_err_t ret = usb_host_interface_release(p_usbh_cdc_obj->cdc_client_hdl, dev_hdl,
                                                   cdc_port->notif.intf_desc->bInterfaceNumber);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to release notification interface %d: %s",
                     cdc_port->notif.intf_desc->bInterfaceNumber, esp_err_to_name(ret));
            err = ret;
        }
    }
    if (err != ESP_OK) {
        return false;
    }
    cdc_port->interfaces_released = true;
    return true;
}

/**
 * @brief Destroy the port resources and close the device when it is no longer used.
 *
 * @note Runs in the USB client task context only, with open_close_mutex held. Holding the mutex
 *       prevents a concurrent usbh_cdc_port_open() from claiming the interfaces that are being
 *       released (or from reusing the device object that is being freed).
 */
static void _cdc_port_destroy(usbh_cdc_port_t *cdc_port)
{
    usbh_cdc_dev_t *cdc_dev = cdc_port->cdc_dev;

    _cdc_transfers_free(cdc_port);

    if (cdc_port->in_ringbuf_handle) {
        vRingbufferDelete(cdc_port->in_ringbuf_handle);
        cdc_port->in_ringbuf_handle = NULL;
    }
    if (cdc_port->out_ringbuf_handle) {
        vRingbufferDelete(cdc_port->out_ringbuf_handle);
        cdc_port->out_ringbuf_handle = NULL;
    }
    if (cdc_port->data.out_xfer_free_sem) {
        vSemaphoreDelete(cdc_port->data.out_xfer_free_sem);
        cdc_port->data.out_xfer_free_sem = NULL;
    }
    if (cdc_port->data.out_mux) {
        vSemaphoreDelete(cdc_port->data.out_mux);
        cdc_port->data.out_mux = NULL;
    }
    if (cdc_port->cb_mutex) {
        vSemaphoreDelete(cdc_port->cb_mutex);
        cdc_port->cb_mutex = NULL;
    }
    free(cdc_port);

    if (cdc_dev == NULL || p_usbh_cdc_obj == NULL) {
        return;
    }

    // The device is closed when the last port that references it has been released.
    if (get_cdc_port_number_on_device(cdc_dev) != 0) {
        return;
    }
    esp_err_t err = _cdc_dev_ctrl_free(cdc_dev);
    if (err != ESP_OK) {
        // Keep the device object alive (and in the list) to avoid freeing a transfer that is
        // still in use. The driver reports a failure on uninstall in this case.
        ESP_LOGE(TAG, "Failed to free the control transfer of device %d: %s", cdc_dev->dev_addr, esp_err_to_name(err));
        return;
    }
    usb_device_handle_t dev_hdl = cdc_dev->dev_hdl;
    if (dev_hdl != NULL) {
        err = usb_host_device_close(p_usbh_cdc_obj->cdc_client_hdl, dev_hdl);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to close device %d: %s", cdc_dev->dev_addr, esp_err_to_name(err));
        }
    }
    CDC_ENTER_CRITICAL();
    SLIST_REMOVE(&p_usbh_cdc_obj->cdc_devices_list, cdc_dev, usbh_cdc_dev_s, list_entry);
    CDC_EXIT_CRITICAL();
    free(cdc_dev);
}

/**
 * @brief Release the ports that have no more in-flight transfer.
 *
 * Called by the USB client task after usb_host_client_handle_events() returned, i.e. after all the
 * completed transfer callbacks have been serviced. This is the second phase of the port close:
 * the port is only freed here, never from usbh_cdc_port_close()/a transfer callback.
 */
static void _cdc_process_free_pending(void)
{
    while (p_usbh_cdc_obj != NULL) {
        usbh_cdc_port_t *cdc_port = NULL;
        xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
        usbh_cdc_port_t *current = NULL;
        SLIST_FOREACH(current, &p_usbh_cdc_obj->free_pending_list, list_entry) {
            if (current->xfer_inflight == 0) {
                cdc_port = current;
                break;
            }
        }
        if (cdc_port == NULL) {
            xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
            return;
        }
        SLIST_REMOVE(&p_usbh_cdc_obj->free_pending_list, cdc_port, usbh_cdc_port_s, list_entry);
        cdc_port->pending_free = false;
        xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);

        xSemaphoreTake(p_usbh_cdc_obj->open_close_mutex, portMAX_DELAY);
        bool released = _cdc_port_release_interfaces(cdc_port);
        if (released) {
            _cdc_port_destroy(cdc_port);
            xSemaphoreGive(p_usbh_cdc_obj->open_close_mutex);
            continue;
        }
        xSemaphoreGive(p_usbh_cdc_obj->open_close_mutex);

        // The interface still has in-flight URBs or a pending endpoint event; try again on the
        // next iteration of the client task.
        cdc_port->free_retries++;
        if (cdc_port->free_retries > 1000) {
            ESP_LOGE(TAG, "Giving up releasing the interfaces of port %p", cdc_port);
        }
        xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
        cdc_port->pending_free = true;
        SLIST_INSERT_HEAD(&p_usbh_cdc_obj->free_pending_list, cdc_port, list_entry);
        xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
        if (cdc_port->free_retries <= 1000) {
            usb_host_client_unblock(p_usbh_cdc_obj->cdc_client_hdl);
        }
        return;
    }
}

/*----------------------------------------- Device event callback dispatch --------------------------------------------*/
static inline bool _dev_bit_test(const dev_event_cb_t *cb, uint8_t addr)
{
    if (addr >= CDC_MAX_DEV_ADDR) {
        return false;
    }
    return (cb->delivered_addr_bits[addr / 32] & (1U << (addr % 32))) != 0;
}

static inline void _dev_bit_set(dev_event_cb_t *cb, uint8_t addr)
{
    if (addr < CDC_MAX_DEV_ADDR) {
        cb->delivered_addr_bits[addr / 32] |= (1U << (addr % 32));
    }
}

static void _dev_bit_clear(dev_event_cb_t *cb, uint8_t addr)
{
    if (addr < CDC_MAX_DEV_ADDR) {
        cb->delivered_addr_bits[addr / 32] &= ~(1U << (addr % 32));
    }
}

static bool _cdc_dispatch_begin(void)
{
    if (p_usbh_cdc_obj == NULL) {
        return false;
    }
    xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
    if (p_usbh_cdc_obj->dispatch_stopped) {
        xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
        return false;
    }
    p_usbh_cdc_obj->dispatch_active++;
    xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
    return true;
}

static void _cdc_dispatch_end(void)
{
    if (p_usbh_cdc_obj == NULL) {
        return;
    }
    xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
    if (p_usbh_cdc_obj->dispatch_active > 0) {
        p_usbh_cdc_obj->dispatch_active--;
    }
    if (p_usbh_cdc_obj->dispatch_active == 0) {
        xSemaphoreGive(p_usbh_cdc_obj->dispatch_idle_sem);
    }
    xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
}

/**
 * @brief Take a snapshot of the registered callbacks.
 *
 * The dispatch counter is incremented before the snapshot is taken, so a concurrently
 * unregistering task will not free any node until _cdc_dispatch_end() is called.
 */
static dev_event_cb_t **_cdc_dev_event_snapshot(int *count_out)
{
    *count_out = 0;
    if (p_usbh_cdc_obj == NULL) {
        return NULL;
    }
    xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
    if (p_usbh_cdc_obj->dispatch_stopped) {
        xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
        return NULL;
    }
    int count = 0;
    dev_event_cb_t *cb = NULL;
    SLIST_FOREACH(cb, &p_usbh_cdc_obj->dev_event_cb_list, list_entry) {
        count++;
    }
    if (count == 0) {
        xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
        return NULL;
    }
    dev_event_cb_t **snapshot = (dev_event_cb_t **)malloc(count * sizeof(dev_event_cb_t *));
    if (snapshot == NULL) {
        ESP_LOGE(TAG, "Failed to allocate the device event callback snapshot");
        xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
        return NULL;
    }
    int i = 0;
    SLIST_FOREACH(cb, &p_usbh_cdc_obj->dev_event_cb_list, list_entry) {
        snapshot[i++] = cb;
    }
    p_usbh_cdc_obj->dispatch_active++;
    xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
    *count_out = count;
    return snapshot;
}

static void _cdc_dispatch_connected(uint8_t dev_addr, const usb_device_desc_t *device_desc,
                                    const usb_config_desc_t *config_desc)
{
    int count = 0;
    dev_event_cb_t **snapshot = _cdc_dev_event_snapshot(&count);
    if (snapshot == NULL) {
        return;
    }
    for (int i = 0; i < count; i++) {
        dev_event_cb_t *cb = snapshot[i];
        int matched_intf_num = -1;
        bool deliver = false;
        xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
        if (!cb->removed && !_dev_bit_test(cb, dev_addr) &&
                usbh_match_id_from_list(device_desc, config_desc, cb->match_id_list, &matched_intf_num) == 1) {
            _dev_bit_set(cb, dev_addr);
            deliver = true;
        }
        xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
        if (deliver && cb->cb) {
            usbh_cdc_device_event_data_t evt_data = {
                .new_dev.dev_addr = dev_addr,
                .new_dev.matched_intf_num = matched_intf_num,
                .new_dev.device_desc = device_desc,
                .new_dev.active_config_desc = config_desc,
            };
            cb->cb(CDC_HOST_DEVICE_EVENT_CONNECTED, &evt_data, cb->user_data);
        }
    }
    free(snapshot);
    _cdc_dispatch_end();
}

static void _cdc_dispatch_disconnected(uint8_t dev_addr, usb_device_handle_t dev_hdl)
{
    int count = 0;
    dev_event_cb_t **snapshot = _cdc_dev_event_snapshot(&count);
    if (snapshot == NULL) {
        return;
    }
    for (int i = 0; i < count; i++) {
        dev_event_cb_t *cb = snapshot[i];
        bool deliver = false;
        xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
        if (!cb->removed) {
            _dev_bit_clear(cb, dev_addr);
            deliver = true;
        }
        xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
        if (deliver && cb->cb) {
            usbh_cdc_device_event_data_t evt_data = {
                .dev_gone.dev_addr = dev_addr,
                .dev_gone.dev_hdl = dev_hdl,
            };
            cb->cb(CDC_HOST_DEVICE_EVENT_DISCONNECTED, &evt_data, cb->user_data);
        }
    }
    free(snapshot);
    _cdc_dispatch_end();
}

/**
 * @brief Copy of the descriptors handed to the device event callbacks.
 *
 * The descriptors cached by the USB Host are only valid while the device is opened. The event
 * callbacks may open/close CDC ports and even trigger a device release, so the descriptors are
 * copied first and the copy stays valid for the whole callback dispatch.
 */
typedef struct {
    usb_device_desc_t *device_desc;
    usb_config_desc_t *config_desc;
    usb_speed_t speed;
    bool speed_valid;
} cdc_desc_copy_t;

static const char *_cdc_speed_str(usb_speed_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:
        return "low (1.5 Mbit/s)";
    case USB_SPEED_FULL:
        return "full (12 Mbit/s)";
    case USB_SPEED_HIGH:
        return "high (480 Mbit/s)";
    default:
        return "unknown";
    }
}

static void _cdc_desc_copy_release(cdc_desc_copy_t *copy)
{
    if (copy == NULL) {
        return;
    }
    free(copy->device_desc);
    free(copy->config_desc);
    copy->device_desc = NULL;
    copy->config_desc = NULL;
}

/**
 * @brief Load and copy the device + active configuration descriptors of a device.
 *
 * The driver's own device handle is reused when the device is already opened; otherwise the
 * device is opened, the descriptors are copied and the device is closed again before the
 * callbacks run (a callback may claim interfaces, which would make a later close fail).
 */
static esp_err_t _cdc_load_descriptors(uint8_t dev_addr, cdc_desc_copy_t *copy)
{
    if (p_usbh_cdc_obj == NULL || copy == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    copy->device_desc = NULL;
    copy->config_desc = NULL;
    copy->speed = USB_SPEED_FULL;
    copy->speed_valid = false;

    usb_device_handle_t dev_hdl = NULL;
    const usb_device_desc_t *device_desc = NULL;
    const usb_config_desc_t *config_desc = NULL;
    bool opened_here = false;
    esp_err_t ret = ESP_OK;

    // Serialize with usbh_cdc_port_open(), which may open the same device concurrently
    xSemaphoreTake(p_usbh_cdc_obj->open_close_mutex, portMAX_DELAY);
    usbh_cdc_dev_t *cdc_dev = get_cdc_dev_by_addr(dev_addr);
    if (cdc_dev != NULL && cdc_dev->dev_hdl != NULL) {
        dev_hdl = cdc_dev->dev_hdl;
    } else {
        ret = usb_host_device_open(p_usbh_cdc_obj->cdc_client_hdl, dev_addr, &dev_hdl);
        if (ret != ESP_OK) {
            xSemaphoreGive(p_usbh_cdc_obj->open_close_mutex);
            return ret;
        }
        opened_here = true;
    }
    usb_device_info_t dev_info;
    if (usb_host_device_info(dev_hdl, &dev_info) == ESP_OK) {
        copy->speed = dev_info.speed;
        copy->speed_valid = true;
    }
    ret = usb_host_get_device_descriptor(dev_hdl, &device_desc);
    if (ret == ESP_OK) {
        ret = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
    }
    if (ret == ESP_OK) {
        if (device_desc == NULL || config_desc == NULL) {
            ret = ESP_ERR_INVALID_STATE;
        } else if (config_desc->wTotalLength < sizeof(usb_config_desc_t) || config_desc->wTotalLength > 8192) {
            ESP_LOGE(TAG, "Invalid configuration descriptor length %d", config_desc->wTotalLength);
            ret = ESP_ERR_INVALID_SIZE;
        }
    }
    if (opened_here) {
        esp_err_t err = usb_host_device_close(p_usbh_cdc_obj->cdc_client_hdl, dev_hdl);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to close device %d after loading its descriptors: %s", dev_addr, esp_err_to_name(err));
        }
    }
    xSemaphoreGive(p_usbh_cdc_obj->open_close_mutex);
    if (ret != ESP_OK) {
        return ret;
    }

    copy->device_desc = (usb_device_desc_t *)malloc(sizeof(usb_device_desc_t));
    copy->config_desc = (usb_config_desc_t *)malloc(config_desc->wTotalLength);
    if (copy->device_desc == NULL || copy->config_desc == NULL) {
        _cdc_desc_copy_release(copy);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy->device_desc, device_desc, sizeof(usb_device_desc_t));
    memcpy(copy->config_desc, config_desc, config_desc->wTotalLength);
    return ESP_OK;
}

/**
 * @brief Replay the CDC_HOST_DEVICE_EVENT_CONNECTED event for the devices already enumerated.
 *
 * USB Host does not re-send NEW_DEV to clients that register after a device finished
 * enumeration. Without this replay a device could stay unhandled until it is re-plugged.
 * The delivered address bitmap avoids delivering the same device twice.
 */
static void _cdc_replay_existing_devices(dev_event_cb_t *registered_cb)
{
    if (p_usbh_cdc_obj == NULL || registered_cb == NULL) {
        return;
    }
    uint8_t dev_addr_list[CDC_MAX_DEV_ADDR];
    int num_dev = 0;
    if (usb_host_device_addr_list_fill(sizeof(dev_addr_list), dev_addr_list, &num_dev) != ESP_OK) {
        return;
    }
    if (num_dev > (int)sizeof(dev_addr_list)) {
        num_dev = sizeof(dev_addr_list);
    }
    if (!_cdc_dispatch_begin()) {
        return;
    }
    for (int i = 0; i < num_dev; i++) {
        uint8_t dev_addr = dev_addr_list[i];

        xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
        bool skip = registered_cb->removed || _dev_bit_test(registered_cb, dev_addr);
        xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
        if (skip) {
            continue;
        }

        cdc_desc_copy_t desc_copy;
        if (_cdc_load_descriptors(dev_addr, &desc_copy) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to load the descriptors of device %d while replaying the connected event", dev_addr);
            continue;
        }
        if (desc_copy.device_desc->bDeviceClass != USB_CLASS_HUB) {
            int matched_intf_num = -1;
            bool deliver = false;
            xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
            if (!registered_cb->removed && !_dev_bit_test(registered_cb, dev_addr) &&
                    usbh_match_id_from_list(desc_copy.device_desc, desc_copy.config_desc, registered_cb->match_id_list, &matched_intf_num) == 1) {
                _dev_bit_set(registered_cb, dev_addr);
                deliver = true;
            }
            xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
            if (deliver && registered_cb->cb) {
                usbh_cdc_device_event_data_t evt_data = {
                    .new_dev.dev_addr = dev_addr,
                    .new_dev.matched_intf_num = matched_intf_num,
                    .new_dev.device_desc = desc_copy.device_desc,
                    .new_dev.active_config_desc = desc_copy.config_desc,
                };
                ESP_LOGI(TAG, "Replaying the CONNECTED event for device %d", dev_addr);
                registered_cb->cb(CDC_HOST_DEVICE_EVENT_CONNECTED, &evt_data, registered_cb->user_data);
            }
        }
        _cdc_desc_copy_release(&desc_copy);
    }
    _cdc_dispatch_end();
}

/**
 * @brief Periodically replay CONNECTED events for devices already on the bus.
 *
 * The USB Host client event queue is a fixed-size queue; if it ever fills up
 * (e.g. a burst of connect/disconnect events), NEW_DEV can be dropped and a
 * device would stay unhandled forever. The registration-time replay covers
 * the common case; this periodic rescan is a cheap safety net. It only
 * dispatches devices that were not delivered to that callback yet.
 *
 * Runs in the client task; the outer dispatch section keeps callbacks from
 * being freed while the snapshot is iterated.
 */
static void _cdc_rescan_existing_devices(void)
{
    if (p_usbh_cdc_obj == NULL) {
        return;
    }
    if (!_cdc_dispatch_begin()) {
        return;
    }

    dev_event_cb_t *cbs[8];
    int num_cbs = 0;
    xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
    dev_event_cb_t *cb = NULL;
    SLIST_FOREACH(cb, &p_usbh_cdc_obj->dev_event_cb_list, list_entry) {
        if (num_cbs < (int)(sizeof(cbs) / sizeof(cbs[0]))) {
            cbs[num_cbs++] = cb;
        }
    }
    xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);

    for (int i = 0; i < num_cbs; i++) {
        xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
        bool removed = cbs[i]->removed;
        xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
        if (!removed) {
            _cdc_replay_existing_devices(cbs[i]);
        }
    }
    _cdc_dispatch_end();
}

/**
 * @brief Close the ports that belong to a disappeared device.
 */
static void _cdc_close_ports_on_device(usb_device_handle_t dev_hdl)
{
    if (p_usbh_cdc_obj == NULL) {
        return;
    }
    // The public close function removes the port from the list, so close them one by one.
    for (int i = 0; i < 64; i++) {
        usbh_cdc_port_t *cdc_port = NULL;
        CDC_ENTER_CRITICAL();
        usbh_cdc_port_t *current = NULL;
        SLIST_FOREACH(current, &p_usbh_cdc_obj->cdc_devices_port_list, list_entry) {
            if (current->cdc_dev && current->cdc_dev->dev_hdl == dev_hdl) {
                cdc_port = current;
                break;
            }
        }
        CDC_EXIT_CRITICAL();
        if (cdc_port == NULL) {
            return;
        }
        esp_err_t ret = usbh_cdc_port_close((usbh_cdc_port_handle_t)cdc_port);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) {
            ESP_LOGE(TAG, "Failed to close the port of a gone device: %s", esp_err_to_name(ret));
            return;
        }
    }
}

/**
 * @brief USB Host client event callback (runs in the client task context).
 */
static void usb_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: {
        uint8_t dev_addr = event_msg->new_dev.address;
        cdc_desc_copy_t desc_copy;
        esp_err_t err = _cdc_load_descriptors(dev_addr, &desc_copy);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "New device connected, address: %d, but its descriptors could not be loaded: %s",
                     dev_addr, esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "New device connected, address: %d, speed: %s", dev_addr,
                 desc_copy.speed_valid ? _cdc_speed_str(desc_copy.speed) : "unknown");
        if (desc_copy.device_desc->bDeviceClass == USB_CLASS_HUB) {
            ESP_LOGD(TAG, "Detect hub device, skip");
            _cdc_desc_copy_release(&desc_copy);
            return;
        }

        // The callbacks receive a private copy of the descriptors: a callback may open/close
        // ports (and even make the device be released), so the cached descriptors cannot be
        // handed out directly.
        _cdc_dispatch_connected(dev_addr, desc_copy.device_desc, desc_copy.config_desc);
        _cdc_desc_copy_release(&desc_copy);
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_GONE: {
        usb_device_handle_t dev_hdl = event_msg->dev_gone.dev_hdl;
        usb_device_info_t device_info;
        esp_err_t err = usb_host_device_info(dev_hdl, &device_info);
        uint8_t dev_addr;
        if (err == ESP_OK) {
            dev_addr = device_info.dev_addr;
            ESP_LOGW(TAG, "Device %d suddenly disconnected", dev_addr);
        } else {
            // Never use the uninitialized device_info. Fall back to the address cached in the
            // driver's own device list.
            dev_addr = get_cdc_dev_addr_by_hdl(dev_hdl);
            ESP_LOGW(TAG, "Device (hdl %p, addr %d) disconnected (device_info failed: %s)",
                     dev_hdl, dev_addr, esp_err_to_name(err));
        }

        _cdc_dispatch_disconnected(dev_addr, dev_hdl);
        _cdc_close_ports_on_device(dev_hdl);
        break;
    }
    default:
        ESP_LOGW(TAG, "Unhandled USB Host client event: %d", event_msg->event);
        break;
    }
}

/*--------------------------------------------- Transfer helper functions ---------------------------------------------*/
/**
 * @brief Halt/flush/clear the endpoint of a transfer so it can be reused.
 *
 * The endpoint may already be halted (e.g. after a device gone), in which case halt/flush return
 * an error that is not fatal. The caller decides whether the error matters.
 */
static esp_err_t _cdc_reset_transfer_endpoint(usb_device_handle_t dev_hdl, usb_transfer_t *transfer)
{
    if (dev_hdl == NULL || transfer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    esp_err_t err = usb_host_endpoint_halt(dev_hdl, transfer->bEndpointAddress);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_NOT_FOUND) {
        ret = err;
    }
    err = usb_host_endpoint_flush(dev_hdl, transfer->bEndpointAddress);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_NOT_FOUND) {
        ret = err;
    }
    err = usb_host_endpoint_clear(dev_hdl, transfer->bEndpointAddress);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_NOT_FOUND) {
        ret = err;
    }
    return ret;
}

/**
 * @brief Submit a transfer and account it as in-flight.
 *
 * The port close path uses xfer_inflight to know when all the completion callbacks of the port
 * have been serviced; only then the transfer/ring buffer/port objects are released.
 */
static esp_err_t _cdc_submit_transfer(usbh_cdc_port_t *cdc_port, usb_transfer_t *xfer)
{
    if (cdc_port == NULL || xfer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    CDC_ENTER_CRITICAL();
    if (cdc_port->to_close) {
        CDC_EXIT_CRITICAL();
        return ESP_ERR_INVALID_STATE;
    }
    cdc_port->xfer_inflight++;
    CDC_EXIT_CRITICAL();

    esp_err_t ret = usb_host_transfer_submit(xfer);
    if (ret != ESP_OK) {
        CDC_ENTER_CRITICAL();
        if (cdc_port->xfer_inflight > 0) {
            cdc_port->xfer_inflight--;
        }
        CDC_EXIT_CRITICAL();
    }
    return ret;
}

/**
 * @brief Account the completion of a transfer.
 *
 * @return false when the port is being closed: the callback must not touch the port anymore.
 */
static bool _cdc_xfer_completed(usbh_cdc_port_t *cdc_port)
{
    bool closing;
    CDC_ENTER_CRITICAL();
    if (cdc_port->xfer_inflight > 0) {
        cdc_port->xfer_inflight--;
    }
    closing = cdc_port->to_close;
    CDC_EXIT_CRITICAL();
    return !closing;
}

/**
 * @brief Invoke the user notification callback, serialized with usbh_cdc_port_close().
 *
 * close() sets to_close and clears the callbacks before taking cb_mutex, so once it holds the
 * mutex no callback can be running and no new one can start. This is what makes it safe for the
 * upper layer to free its user_data (e.g. the ECM object) right after close() returns.
 */
static void _cdc_invoke_notif_cb(usbh_cdc_port_t *cdc_port, uint32_t length)
{
    if (cdc_port->cb_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(cdc_port->cb_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!cdc_port->to_close) {
        iot_cdc_notification_t *notif = (iot_cdc_notification_t *)cdc_port->notif.xfer->data_buffer;
        if (cdc_port->cbs.notif_cb_ex) {
            cdc_port->cbs.notif_cb_ex((usbh_cdc_port_handle_t)cdc_port, notif, length, cdc_port->cbs.user_data);
        } else if (cdc_port->cbs.notif_cb) {
            cdc_port->cbs.notif_cb((usbh_cdc_port_handle_t)cdc_port, notif, cdc_port->cbs.user_data);
        }
    }
    xSemaphoreGive(cdc_port->cb_mutex);
}

static void _cdc_invoke_recv_cb(usbh_cdc_port_t *cdc_port)
{
    if (cdc_port->cb_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(cdc_port->cb_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!cdc_port->to_close && cdc_port->cbs.recv_data) {
        cdc_port->cbs.recv_data((usbh_cdc_port_handle_t)cdc_port, cdc_port->cbs.user_data);
    }
    xSemaphoreGive(cdc_port->cb_mutex);
}

/*--------------------------------------------- Transfer callbacks ----------------------------------------------------*/
static void notif_xfer_cb(usb_transfer_t *notif_xfer)
{
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *)notif_xfer->context;
    if (cdc_port == NULL || !_cdc_xfer_completed(cdc_port)) {
        return;
    }

    switch (notif_xfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED: {
        uint32_t length = notif_xfer->actual_num_bytes;
        // The flexible `Data[]` member is only valid when at least the 8-byte header was
        // received. The upper layer validates notif->wLength before reading the payload.
        if (length >= offsetof(iot_cdc_notification_t, Data)) {
            _cdc_invoke_notif_cb(cdc_port, length);
        } else {
            ESP_LOGW(TAG, "Notification transfer too short: %"PRIu32" bytes", length);
        }
        cdc_port->notif_retry_count = 0;
        // Start polling for the next notification
        _cdc_submit_transfer(cdc_port, notif_xfer);
        return;
    }
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
        return;
    default: {
        // Any other error: retry a bounded number of times, then stop polling and report the
        // error through usbh_cdc_port_get_status(). The endpoint is reset so a later
        // usbh_cdc_port_start_data()/port reopen can recover.
        ESP_LOGW(TAG, "Notif xfer failed, EP:%d, status %d",
                 notif_xfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_NUM_MASK, notif_xfer->status);
        if (++cdc_port->notif_retry_count <= CONFIG_USBH_CDC_IN_EP_RETRY_COUNT) {
            _cdc_reset_transfer_endpoint(cdc_port->cdc_dev->dev_hdl, notif_xfer);
            esp_err_t ret = _cdc_submit_transfer(cdc_port, notif_xfer);
            if (ret == ESP_OK) {
                return;
            }
            ESP_LOGW(TAG, "Failed to resubmit the notification transfer: %s", esp_err_to_name(ret));
        }
        cdc_port->notif_error = true;
        ESP_LOGE(TAG, "Notification polling stopped after %d failures", cdc_port->notif_retry_count);
        return;
    }
    }
}

static void control_xfer_cb(usb_transfer_t *ctrl_xfer)
{
    usbh_cdc_dev_t *cdc_dev = (usbh_cdc_dev_t *)ctrl_xfer->context;
    if (cdc_dev == NULL) {
        return;
    }
    CDC_ENTER_CRITICAL();
    cdc_dev->ctrl.pending = false;
    CDC_EXIT_CRITICAL();
    if (cdc_dev->ctrl.done) {
        xSemaphoreGive(cdc_dev->ctrl.done);
    }
}

static void in_xfer_cb(usb_transfer_t *in_xfer)
{
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *)in_xfer->context;
    if (cdc_port == NULL || !_cdc_xfer_completed(cdc_port)) {
        return;
    }

    switch (in_xfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED: {
        if (cdc_port->in_ringbuf_handle) {
            size_t data_len = _get_ringbuf_len(cdc_port->in_ringbuf_handle);
            if (data_len + in_xfer->actual_num_bytes >= cdc_port->in_ringbuf_size) {
                // TODO: add notify cb for user
                // if ringbuffer overflow, drop the data
                ESP_LOGW(TAG, "CDC in ringbuf is full!");
            } else {
                ESP_LOG_BUFFER_HEXDUMP(TAG, in_xfer->data_buffer, in_xfer->actual_num_bytes, ESP_LOG_DEBUG);
                if (_ringbuf_push(cdc_port->in_ringbuf_handle, in_xfer->data_buffer, in_xfer->actual_num_bytes, pdMS_TO_TICKS(TIMEOUT_USB_RINGBUF_MS)) != ESP_OK) {
                    ESP_LOGE(TAG, "in ringbuf push failed");
                }
            }
        }

        if (cdc_port->cbs.recv_data) {
            _cdc_invoke_recv_cb(cdc_port);
        }

        cdc_port->in_retry_count = 0;
        cdc_port->rx_error = false;
        _cdc_submit_transfer(cdc_port, in_xfer);
        return;
    }
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
        return;
    default: {
        // Any other error: retry a bounded number of times. The retry counter is per port, and
        // once it is exhausted the polling stops (the error is visible via
        // usbh_cdc_port_get_status() and start_data() can be used to restart it).
        ESP_LOGW(TAG, "IN Transfer failed, EP:%d, status %d",
                 in_xfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_NUM_MASK, in_xfer->status);
        if (++cdc_port->in_retry_count <= CONFIG_USBH_CDC_IN_EP_RETRY_COUNT) {
            _cdc_reset_transfer_endpoint(cdc_port->cdc_dev->dev_hdl, in_xfer);
            esp_err_t ret = _cdc_submit_transfer(cdc_port, in_xfer);
            if (ret == ESP_OK) {
                return;
            }
            ESP_LOGW(TAG, "Failed to resubmit the BULK IN transfer: %s", esp_err_to_name(ret));
        }
        cdc_port->rx_error = true;
        cdc_port->data_started = false;
        ESP_LOGE(TAG, "BULK IN polling stopped after %d failures", cdc_port->in_retry_count);
        return;
    }
    }
}

static void out_xfer_cb(usb_transfer_t *out_xfer)
{
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *)out_xfer->context;
    if (cdc_port == NULL || !_cdc_xfer_completed(cdc_port)) {
        return;
    }

    switch (out_xfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED: {
        if (cdc_port->out_ringbuf_handle) {
            _cdc_tx_xfer_submit(out_xfer);
        } else {
            xSemaphoreGive(cdc_port->data.out_xfer_free_sem);
        }
        return;
    }
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
        // The user is notified about the device disconnection from usb_event_cb(), but the TX
        // token must be given back so a writer blocked on it does not time out.
        xSemaphoreGive(cdc_port->data.out_xfer_free_sem);
        return;
    default: {
        ESP_LOGW(TAG, "OUT Transfer failed, EP:%d, status %d",
                 out_xfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_NUM_MASK, out_xfer->status);
        // Clear the endpoint so the next transfer is not immediately rejected, and release the
        // transfer token so the TX path can retry. Without this a single OUT error permanently
        // leaks out_xfer_free_sem and every later write times out.
        _cdc_reset_transfer_endpoint(cdc_port->cdc_dev->dev_hdl, out_xfer);
        xSemaphoreGive(cdc_port->data.out_xfer_free_sem);
        return;
    }
    }
}

/**
 * @brief Submit the next OUT transfer (ring buffer mode) or give the TX token back.
 */
static esp_err_t _cdc_tx_xfer_submit(usb_transfer_t *out_xfer)
{
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) out_xfer->context;
    if (cdc_port == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;

    if (cdc_port->out_ringbuf_handle) {
        size_t data_len = _get_ringbuf_len(cdc_port->out_ringbuf_handle);
        if (data_len > 0) {
            if (data_len > out_xfer->data_buffer_size) {
                data_len = out_xfer->data_buffer_size;
            }
            size_t actual_num_bytes = 0;
            if (_ringbuf_pop(cdc_port->out_ringbuf_handle, out_xfer->data_buffer, data_len, &actual_num_bytes, 0) != ESP_OK ||
                    actual_num_bytes == 0) {
                // Nothing could be dequeued; return the token so the next write can retry.
                if (actual_num_bytes == 0) {
                    xSemaphoreGive(cdc_port->data.out_xfer_free_sem);
                }
                return ESP_FAIL;
            }
            out_xfer->num_bytes = actual_num_bytes;
            ret = _cdc_submit_transfer(cdc_port, out_xfer);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to submit the OUT transfer: %s", esp_err_to_name(ret));
                xSemaphoreGive(cdc_port->data.out_xfer_free_sem);
            }
        } else {
            xSemaphoreGive(cdc_port->data.out_xfer_free_sem);
        }
    } else {
        ret = _cdc_submit_transfer(cdc_port, out_xfer);
        if (ret != ESP_OK) {
            xSemaphoreGive(cdc_port->data.out_xfer_free_sem);
        }
    }
    return ret;
}

/*----------------------------------------------- Control transfers ---------------------------------------------------*/
static esp_err_t _cdc_dev_ctrl_free(usbh_cdc_dev_t *cdc_dev)
{
    if (cdc_dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    SemaphoreHandle_t mux = cdc_dev->ctrl.mux;
    if (mux != NULL) {
        if (xSemaphoreTake(mux, pdMS_TO_TICKS(CDC_CLOSE_MUTEX_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "Timed out waiting for the control mutex of device %d", cdc_dev->dev_addr);
            return ESP_ERR_TIMEOUT;
        }
    }
    bool pending;
    CDC_ENTER_CRITICAL();
    pending = cdc_dev->ctrl.pending;
    CDC_EXIT_CRITICAL();
    if (pending) {
        ESP_LOGE(TAG, "A control transfer of device %d is still in flight", cdc_dev->dev_addr);
        if (mux) {
            xSemaphoreGive(mux);
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (cdc_dev->ctrl.xfer) {
        usb_host_transfer_free(cdc_dev->ctrl.xfer);
        cdc_dev->ctrl.xfer = NULL;
    }
    if (cdc_dev->ctrl.done) {
        vSemaphoreDelete(cdc_dev->ctrl.done);
        cdc_dev->ctrl.done = NULL;
    }
    if (mux) {
        xSemaphoreGive(mux);
        vSemaphoreDelete(mux);
        cdc_dev->ctrl.mux = NULL;
    }
    return ESP_OK;
}

static esp_err_t _cdc_dev_ctrl_allocate(usbh_cdc_dev_t *cdc_dev)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(usb_host_transfer_alloc(CONFIG_USBH_CDC_CONTROL_TRANSFER_BUFFER_SIZE + sizeof(usb_setup_packet_t), 0, &cdc_dev->ctrl.xfer),
                      err, TAG, "Failed to allocate ctrl transfer");
    cdc_dev->ctrl.xfer->timeout_ms = 1000;
    cdc_dev->ctrl.xfer->bEndpointAddress = 0;
    cdc_dev->ctrl.xfer->device_handle = cdc_dev->dev_hdl;
    cdc_dev->ctrl.xfer->callback = control_xfer_cb;
    cdc_dev->ctrl.xfer->context = cdc_dev;
    cdc_dev->ctrl.pending = false;
    cdc_dev->ctrl.done = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(cdc_dev->ctrl.done != NULL, ESP_ERR_NO_MEM, err, TAG, "Failed to create the control completion semaphore");
    cdc_dev->ctrl.mux = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(cdc_dev->ctrl.mux != NULL, ESP_ERR_NO_MEM, err, TAG, "Failed to create the control mutex");
    return ESP_OK;
err:
    _cdc_dev_ctrl_free(cdc_dev);
    return ret == ESP_OK ? ESP_ERR_NO_MEM : ret;
}

static esp_err_t _cdc_transfers_allocate(usbh_cdc_port_t *cdc_port, const usb_ep_desc_t *notif_ep_desc, const usb_ep_desc_t *in_ep_desc, const usb_ep_desc_t *out_ep_desc, size_t in_buf_len, size_t out_buf_len)
{
    esp_err_t ret = ESP_OK;

    if (notif_ep_desc) {
        ESP_GOTO_ON_ERROR(usb_host_transfer_alloc(USB_EP_DESC_GET_MPS(notif_ep_desc), 0, &cdc_port->notif.xfer),
                          err, TAG, "Failed to allocate notif transfer");
        ESP_GOTO_ON_FALSE(cdc_port->notif.xfer != NULL, ESP_ERR_NO_MEM, err, TAG, "notification transfer alloc returned NULL");
        cdc_port->notif.xfer->device_handle = cdc_port->cdc_dev->dev_hdl;
        cdc_port->notif.xfer->bEndpointAddress = notif_ep_desc->bEndpointAddress;
        cdc_port->notif.xfer->callback = notif_xfer_cb;
        cdc_port->notif.xfer->context = cdc_port;
        cdc_port->notif.xfer->num_bytes = USB_EP_DESC_GET_MPS(notif_ep_desc);
    }

    if (in_ep_desc) {
        if (in_buf_len > 0) {
            ESP_GOTO_ON_ERROR(usb_host_transfer_alloc(in_buf_len, 0, &cdc_port->data.in_xfer),
                              err, TAG, "Failed to allocate data.in_xfer transfer");
            ESP_GOTO_ON_FALSE(cdc_port->data.in_xfer != NULL, ESP_ERR_NO_MEM, err, TAG, "IN transfer alloc returned NULL");
            cdc_port->data.in_xfer->context = cdc_port;
            cdc_port->data.in_xfer->num_bytes = in_buf_len;
            cdc_port->data.in_xfer->bEndpointAddress = in_ep_desc->bEndpointAddress;
            cdc_port->data.in_xfer->device_handle = cdc_port->cdc_dev->dev_hdl;
            cdc_port->data.in_mps = USB_EP_DESC_GET_MPS(in_ep_desc);
            cdc_port->data.in_xfer->callback = in_xfer_cb;
        }
    }

    if (out_ep_desc) {
        if (out_buf_len > 0) {
            ESP_GOTO_ON_ERROR(
                usb_host_transfer_alloc(out_buf_len, 0, &cdc_port->data.out_xfer),
                err, TAG, "Failed to allocate data.out_xfer transfer");
            ESP_GOTO_ON_FALSE(cdc_port->data.out_xfer != NULL, ESP_ERR_NO_MEM, err, TAG, "OUT transfer alloc returned NULL");
            cdc_port->data.out_xfer->bEndpointAddress = out_ep_desc->bEndpointAddress;
            cdc_port->data.out_xfer->device_handle = cdc_port->cdc_dev->dev_hdl;
            cdc_port->data.out_xfer->context = cdc_port;
            cdc_port->data.out_xfer->num_bytes = out_buf_len;
            cdc_port->data.out_xfer->callback = out_xfer_cb;
        }
    }
    return ret;
err:
    // Idempotent: the pointers are set to NULL by _cdc_transfers_free()
    _cdc_transfers_free(cdc_port);
    return ret == ESP_OK ? ESP_ERR_NO_MEM : ret;
}

static void _cdc_transfers_free(usbh_cdc_port_t *cdc_port)
{
    if (cdc_port == NULL) {
        return;
    }
    if (cdc_port->notif.xfer) {
        usb_host_transfer_free(cdc_port->notif.xfer);
        cdc_port->notif.xfer = NULL;
    }
    if (cdc_port->data.in_xfer) {
        usb_host_transfer_free(cdc_port->data.in_xfer);
        cdc_port->data.in_xfer = NULL;
    }
    if (cdc_port->data.out_xfer) {
        usb_host_transfer_free(cdc_port->data.out_xfer);
        cdc_port->data.out_xfer = NULL;
    }
}

/*----------------------------------------------- Driver task loop -----------------------------------------------------*/
/**
 * @brief CDC-ACM driver handling task
 *
 * USB host client registration and deregistration is handled here.
 *
 * @param[in] arg User's argument. Handle of a task that started this task.
 */
static void usbh_cdc_client_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // Task is resumed from usbh_cdc_driver_install()
    usbh_cdc_obj_t *usbh_cdc_obj = p_usbh_cdc_obj; // Make local copy of the driver's handle
    if (usbh_cdc_obj == NULL || usbh_cdc_obj->cdc_client_hdl == NULL) {
        vTaskDelete(NULL);
        return;
    }
    TickType_t teardown_start = 0;
    // Start handling client's events
    while (1) {
        /* Use a timeout instead of portMAX_DELAY so the periodic device rescan
         * below can recover a NEW_DEV event that was lost when the client
         * event queue was full. */
        esp_err_t ret = usb_host_client_handle_events(usbh_cdc_obj->cdc_client_hdl,
                                                      pdMS_TO_TICKS(5000));
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "usb_host_client_handle_events failed: %s", esp_err_to_name(ret));
        }
        if (ret == ESP_ERR_TIMEOUT) {
            _cdc_rescan_existing_devices();
        }
        // Release the ports whose completion callbacks were just serviced
        _cdc_process_free_pending();
        EventBits_t events = xEventGroupGetBits(usbh_cdc_obj->event_group);
        if (events & CDC_TEARDOWN) {
            if (teardown_start == 0) {
                teardown_start = xTaskGetTickCount();
            }
            // Do not leave the deferred releases behind
            if (_cdc_pending_free_empty() || (xTaskGetTickCount() - teardown_start) > pdMS_TO_TICKS(CDC_UNINSTALL_TIMEOUT_MS)) {
                break;
            }
            usb_host_client_unblock(usbh_cdc_obj->cdc_client_hdl);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    xEventGroupSetBits(usbh_cdc_obj->event_group, CDC_TEARDOWN_COMPLETE);
    vTaskDelete(NULL);
}

/*------------------------------------------------ Driver lifecycle -----------------------------------------------------*/
esp_err_t usbh_cdc_driver_install(const usbh_cdc_driver_config_t *config)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj == NULL, ESP_ERR_INVALID_STATE, TAG, "usbh_cdc already installed");

    ESP_LOGI(TAG, "iot usbh cdc version: %d.%d.%d", IOT_USBH_CDC_VER_MAJOR, IOT_USBH_CDC_VER_MINOR, IOT_USBH_CDC_VER_PATCH);

    usbh_cdc_obj_t *usbh_cdc_obj = (usbh_cdc_obj_t *)calloc(1, sizeof(usbh_cdc_obj_t));
    ESP_RETURN_ON_FALSE(usbh_cdc_obj != NULL, ESP_ERR_NO_MEM, TAG, "calloc failed");
    p_usbh_cdc_obj = usbh_cdc_obj;
    SLIST_INIT(&(usbh_cdc_obj->dev_event_cb_list));
    SLIST_INIT(&(usbh_cdc_obj->cdc_devices_list));
    SLIST_INIT(&(usbh_cdc_obj->cdc_devices_port_list));
    SLIST_INIT(&(usbh_cdc_obj->free_pending_list));

    usbh_cdc_obj->event_group = xEventGroupCreate();
    usbh_cdc_obj->open_close_mutex = xSemaphoreCreateMutex();
    usbh_cdc_obj->dev_event_mutex = xSemaphoreCreateMutex();
    usbh_cdc_obj->dispatch_idle_sem = xSemaphoreCreateBinary();
    /* Reset before any error path can run; the flag only ever describes this
     * install call. */
    s_usb_host_installed_by_driver = false;
    TaskHandle_t driver_task_h = NULL;
    TaskHandle_t lib_task_h = NULL;
    bool client_registered = false;
    usb_host_client_handle_t usb_client = NULL;

    if (usbh_cdc_obj->event_group == NULL || usbh_cdc_obj->open_close_mutex == NULL ||
            usbh_cdc_obj->dev_event_mutex == NULL || usbh_cdc_obj->dispatch_idle_sem == NULL) {
        ret = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Failed to create the driver synchronization primitives");
        goto err;
    }

    if (!config->skip_init_usb_host_driver) {
        usb_lib_task_ctx_t *lib_ctx = (usb_lib_task_ctx_t *)calloc(1, sizeof(usb_lib_task_ctx_t));
        if (lib_ctx == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto err;
        }
        lib_ctx->install_task = xTaskGetCurrentTaskHandle();
        lib_ctx->enum_filter_cb = config->enum_filter_cb;
        BaseType_t core_id = (CONFIG_USBH_TASK_CORE_ID < 0) ? tskNO_AFFINITY : CONFIG_USBH_TASK_CORE_ID;
        BaseType_t task_created = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, lib_ctx,
                                                          CONFIG_USBH_TASK_BASE_PRIORITY, &lib_task_h, core_id);
        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
            free(lib_ctx);
            ret = ESP_FAIL;
            goto err;
        }
        // Wait until the USB host library is installed; the task notifies 1 on success, 0 on failure
        uint32_t notify_value = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        if (notify_value != 1) {
            ESP_LOGE(TAG, "USB host library not installed");
            ret = ESP_FAIL;
            goto err;
        }
    }

    BaseType_t core_id = (config->task_coreid < 0) ? tskNO_AFFINITY : config->task_coreid;
    if (xTaskCreatePinnedToCore(usbh_cdc_client_task, "usbh_cdc", config->task_stack_size, NULL,
                                config->task_priority, &driver_task_h, core_id) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the USB CDC client task");
        ret = ESP_FAIL;
        goto err;
    }

    // Register USB Host client
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async.client_event_callback = usb_event_cb,
        .async.callback_arg = NULL
    };
    ret = usb_host_client_register(&client_config, &usb_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register the USB host client: %s", esp_err_to_name(ret));
        goto err;
    }
    client_registered = true;
    usbh_cdc_obj->cdc_client_hdl = usb_client;

    xTaskNotifyGive(driver_task_h);
    return ESP_OK;

err:
    /* Stop our event-loop task before touching the host library: otherwise it
     * could observe NO_CLIENTS while the client is deregistered and uninstall
     * the library under our feet. */
    if (lib_task_h) {
        vTaskDelete(lib_task_h);
        lib_task_h = NULL;
    }
    if (client_registered) {
        esp_err_t err = usb_host_client_deregister(usb_client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to deregister the USB host client: %s", esp_err_to_name(err));
        }
    }
    if (driver_task_h) {
        vTaskDelete(driver_task_h);
    }
    /* Roll back the library only when this install call brought it up. Without
     * this, a retry would find it already installed and start a second
     * usb_lib_task, i.e. two tasks calling usb_host_lib_handle_events(). */
    if (s_usb_host_installed_by_driver) {
        esp_err_t err = usb_host_uninstall();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to roll back the USB Host library: %s", esp_err_to_name(err));
        }
        s_usb_host_installed_by_driver = false;
    }
    if (usbh_cdc_obj->event_group) {
        vEventGroupDelete(usbh_cdc_obj->event_group);
    }
    if (usbh_cdc_obj->open_close_mutex) {
        vSemaphoreDelete(usbh_cdc_obj->open_close_mutex);
    }
    if (usbh_cdc_obj->dev_event_mutex) {
        vSemaphoreDelete(usbh_cdc_obj->dev_event_mutex);
    }
    if (usbh_cdc_obj->dispatch_idle_sem) {
        vSemaphoreDelete(usbh_cdc_obj->dispatch_idle_sem);
    }
    free(usbh_cdc_obj);
    p_usbh_cdc_obj = NULL;
    return ret;
}

static esp_err_t _cdc_wait_drained(TickType_t timeout)
{
    TickType_t start = xTaskGetTickCount();
    while (1) {
        if (_cdc_devices_empty() && _cdc_pending_free_empty()) {
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start) >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        usb_host_client_unblock(p_usbh_cdc_obj->cdc_client_hdl);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t usbh_cdc_driver_uninstall(void)
{
    // Idempotent: allows the teardown paths to call this unconditionally
    if (p_usbh_cdc_obj == NULL) {
        return ESP_OK;
    }
    usbh_cdc_obj_t *usbh_cdc_obj = p_usbh_cdc_obj;
    esp_err_t ret = ESP_OK;

    // Stop dispatching device events before touching the callback list
    xSemaphoreTake(usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
    usbh_cdc_obj->dispatch_stopped = true;
    xSemaphoreGive(usbh_cdc_obj->dev_event_mutex);

    // Close the ports that are still open; the client task releases them
    ret = usbh_cdc_port_close_all();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to close all the CDC ports: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = _cdc_wait_drained(pdMS_TO_TICKS(CDC_UNINSTALL_TIMEOUT_MS));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Timed out waiting for the CDC ports/device to be released");
        return ret;
    }

    // Signal the CDC task to stop, unblock it and wait for its deletion
    xEventGroupSetBits(usbh_cdc_obj->event_group, CDC_TEARDOWN);
    usb_host_client_unblock(usbh_cdc_obj->cdc_client_hdl);
    if (xEventGroupWaitBits(usbh_cdc_obj->event_group, CDC_TEARDOWN_COMPLETE, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(CDC_TEARDOWN_TIMEOUT_MS)) == 0) {
        ESP_LOGE(TAG, "Timeout waiting for the CDC client task to stop");
        return ESP_ERR_TIMEOUT;
    }

    ret = usb_host_client_deregister(usbh_cdc_obj->cdc_client_hdl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deregister the USB host client: %s", esp_err_to_name(ret));
    }
    usbh_cdc_obj->cdc_client_hdl = NULL;

    // Free the remaining device event callbacks
    xSemaphoreTake(usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
    dev_event_cb_t *current, *tmp;
    SLIST_FOREACH_SAFE(current, &usbh_cdc_obj->dev_event_cb_list, list_entry, tmp) {
        SLIST_REMOVE(&(usbh_cdc_obj->dev_event_cb_list), current, dev_event_cb_s, list_entry);
        free(current);
    }
    xSemaphoreGive(usbh_cdc_obj->dev_event_mutex);

    vEventGroupDelete(usbh_cdc_obj->event_group);
    vSemaphoreDelete(usbh_cdc_obj->open_close_mutex);
    vSemaphoreDelete(usbh_cdc_obj->dev_event_mutex);
    vSemaphoreDelete(usbh_cdc_obj->dispatch_idle_sem);
    free(usbh_cdc_obj);
    p_usbh_cdc_obj = NULL;
    return ret;
}

esp_err_t usbh_cdc_register_dev_event_cb(const usb_device_match_id_t *dev_match_id_list, usbh_cdc_device_event_callback_t event_cb, void *user_data)
{
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj, ESP_ERR_INVALID_STATE, TAG, "usbh cdc not installed");
    ESP_RETURN_ON_FALSE(dev_match_id_list, ESP_ERR_INVALID_ARG, TAG, "dev_match_id_list is NULL");
    ESP_RETURN_ON_FALSE(event_cb, ESP_ERR_INVALID_ARG, TAG, "event_cb is NULL");

    dev_event_cb_t *cb = (dev_event_cb_t *) calloc(1, sizeof(dev_event_cb_t));
    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate the device event callback");
    cb->cb = event_cb;
    cb->user_data = user_data;
    cb->match_id_list = dev_match_id_list;

    xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
    if (p_usbh_cdc_obj->dispatch_stopped) {
        xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
        free(cb);
        return ESP_ERR_INVALID_STATE;
    }
    SLIST_INSERT_HEAD(&(p_usbh_cdc_obj->dev_event_cb_list), cb, list_entry);
    xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);

    // The device may have finished enumeration before the callback was registered: replay the
    // CONNECTED event for the devices that are already present.
    _cdc_replay_existing_devices(cb);
    return ESP_OK;
}

esp_err_t usbh_cdc_unregister_dev_event_cb(usbh_cdc_device_event_callback_t event_cb)
{
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj, ESP_ERR_INVALID_STATE, TAG, "usbh cdc not installed");
    ESP_RETURN_ON_FALSE(event_cb, ESP_ERR_INVALID_ARG, TAG, "event_cb is NULL");

    dev_event_cb_t *found = NULL;
    xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
    dev_event_cb_t *current, *tmp;
    SLIST_FOREACH_SAFE(current, &p_usbh_cdc_obj->dev_event_cb_list, list_entry, tmp) {
        if (current->cb == event_cb) {
            SLIST_REMOVE(&p_usbh_cdc_obj->dev_event_cb_list, current, dev_event_cb_s, list_entry);
            current->removed = true;
            found = current;
            break;
        }
    }
    xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
    if (found == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    // Wait until no device event is being dispatched anymore: after that no callback can be
    // executing with the user_data that the caller is about to free.
    TickType_t start = xTaskGetTickCount();
    while (1) {
        xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
        bool idle = (p_usbh_cdc_obj->dispatch_active == 0);
        xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);
        if (idle) {
            break;
        }
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(CDC_DEV_EVENT_DRAIN_MS)) {
            ESP_LOGE(TAG, "Timed out waiting for the device event callbacks to drain");
            return ESP_ERR_TIMEOUT;    // Do not free the node: it may still be in use
        }
        xSemaphoreTake(p_usbh_cdc_obj->dispatch_idle_sem, pdMS_TO_TICKS(50));
    }
    free(found);
    return ESP_OK;
}

/*------------------------------------------------- Port lifecycle ------------------------------------------------------*/
esp_err_t usbh_cdc_port_open(const usbh_cdc_port_config_t *port_config, usbh_cdc_port_handle_t *cdc_port_out)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, TAG, "usb cdc is not installed");
    ESP_RETURN_ON_FALSE(cdc_port_out != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_out is NULL");
    ESP_RETURN_ON_FALSE(port_config != NULL, ESP_ERR_INVALID_ARG, TAG, "port_config is NULL");
    ESP_RETURN_ON_FALSE(port_config->in_transfer_buffer_size != 0, ESP_ERR_INVALID_ARG, TAG, "in_transfer_buffer_size is 0");
    ESP_RETURN_ON_FALSE(port_config->out_transfer_buffer_size != 0, ESP_ERR_INVALID_ARG, TAG, "out_transfer_buffer_size is 0");
    *cdc_port_out = NULL;

    xSemaphoreTake(p_usbh_cdc_obj->open_close_mutex, portMAX_DELAY);

    // Check if the port is already opened
    CDC_ENTER_CRITICAL();
    usbh_cdc_port_t *tmp_cdc_port = NULL;
    bool port_opened = false;
    SLIST_FOREACH(tmp_cdc_port, &p_usbh_cdc_obj->cdc_devices_port_list, list_entry) {
        if (tmp_cdc_port->itf_num == port_config->itf_num && tmp_cdc_port->cdc_dev->dev_addr == port_config->dev_addr) {
            port_opened = true;
            break;
        }
    }
    CDC_EXIT_CRITICAL();
    ESP_GOTO_ON_FALSE(!port_opened, ESP_ERR_INVALID_STATE, err_exit, TAG, "port is already opened");

    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *)calloc(1, sizeof(usbh_cdc_port_t));
    ESP_GOTO_ON_FALSE(cdc_port != NULL, ESP_ERR_NO_MEM, err_exit, TAG, "calloc failed for cdc port");

    bool is_claimed_data = false, is_claimed_notif = false;
    bool new_dev_inserted = false;
    // If the device handle is not set, try to open the device
    usbh_cdc_dev_t *_cdc_dev = get_cdc_dev_by_addr(port_config->dev_addr);
    usbh_cdc_dev_t *new_cdc_dev = NULL;
    if (NULL == _cdc_dev) {
        new_cdc_dev = (usbh_cdc_dev_t *)calloc(1, sizeof(usbh_cdc_dev_t));
        ESP_GOTO_ON_FALSE(new_cdc_dev != NULL, ESP_ERR_NO_MEM, err, TAG, "calloc failed for cdc device");
        new_cdc_dev->dev_addr = port_config->dev_addr;

        usb_device_handle_t current_device = NULL;
        ESP_GOTO_ON_ERROR(usb_host_device_open(p_usbh_cdc_obj->cdc_client_hdl, port_config->dev_addr, &current_device), err, TAG, "Could not open device %d", port_config->dev_addr);
        new_cdc_dev->dev_hdl = current_device;
        ESP_GOTO_ON_ERROR(_cdc_dev_ctrl_allocate(new_cdc_dev), err, TAG, "Failed to allocate ctrl");
        CDC_ENTER_CRITICAL();
        SLIST_INSERT_HEAD(&p_usbh_cdc_obj->cdc_devices_list, new_cdc_dev, list_entry);
        CDC_EXIT_CRITICAL();
        new_dev_inserted = true;
        _cdc_dev = new_cdc_dev;
    }
    cdc_port->cdc_dev = _cdc_dev;

    // Get Device and Configuration descriptors
    const usb_config_desc_t *config_desc = NULL;
    const usb_device_desc_t *device_desc = NULL;
    ESP_GOTO_ON_ERROR(usb_host_get_active_config_descriptor(cdc_port->cdc_dev->dev_hdl, &config_desc), err, TAG, "Failed to get the active configuration descriptor");
    ESP_GOTO_ON_ERROR(usb_host_get_device_descriptor(cdc_port->cdc_dev->dev_hdl, &device_desc), err, TAG, "Failed to get the device descriptor");
    usbh_cdc_parsed_info_t cdc_info = {0};
    ESP_GOTO_ON_ERROR(cdc_parse_interface_descriptor(device_desc, config_desc, port_config->itf_num, &cdc_info), err, TAG, "Could not parse descriptor on interface %d", port_config->itf_num);

    cdc_port->cbs = port_config->cbs;
    cdc_port->out_ringbuf_size = port_config->out_ringbuf_size;
    cdc_port->in_ringbuf_size = port_config->in_ringbuf_size;
    cdc_port->itf_num = port_config->itf_num;

    if (cdc_port->in_ringbuf_size != 0) {
        cdc_port->in_ringbuf_handle = xRingbufferCreate(cdc_port->in_ringbuf_size, RINGBUF_TYPE_BYTEBUF);
        ESP_GOTO_ON_FALSE(cdc_port->in_ringbuf_handle != NULL, ESP_ERR_NO_MEM, err, TAG, "Failed to create ring buffer");
    }

    if (cdc_port->out_ringbuf_size != 0) {
        cdc_port->out_ringbuf_handle = xRingbufferCreate(cdc_port->out_ringbuf_size, RINGBUF_TYPE_BYTEBUF);
        ESP_GOTO_ON_FALSE(cdc_port->out_ringbuf_handle != NULL, ESP_ERR_NO_MEM, err, TAG, "Failed to create ring buffer");
    }

    cdc_port->data.out_xfer_free_sem = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(cdc_port->data.out_xfer_free_sem != NULL, ESP_ERR_NO_MEM, err, TAG, "Failed to create the TX token semaphore");
    xSemaphoreGive(cdc_port->data.out_xfer_free_sem);

    cdc_port->data.out_mux = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(cdc_port->data.out_mux != NULL, ESP_ERR_NO_MEM, err, TAG, "Failed to create mutex for out transfer");

    cdc_port->cb_mutex = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(cdc_port->cb_mutex != NULL, ESP_ERR_NO_MEM, err, TAG, "Failed to create the callback mutex");

    if (port_config->flags & USBH_CDC_FLAGS_DISABLE_NOTIFICATION) {
        cdc_info.notif_ep = NULL; // Force to null if the user does not want notifications
    }

    if (cdc_info.notif_ep) {
        cdc_port->notif.intf_desc = cdc_info.notif_intf;
    }

    // Must have data interface descriptor
    cdc_port->data.intf_desc = cdc_info.data_intf;

    if (cdc_port->in_ringbuf_handle) {
        _ringbuf_flush(cdc_port->in_ringbuf_handle);
    }
    if (cdc_port->out_ringbuf_handle) {
        _ringbuf_flush(cdc_port->out_ringbuf_handle);
    }

    ESP_GOTO_ON_ERROR(_cdc_transfers_allocate(cdc_port, cdc_info.notif_ep, cdc_info.in_ep, cdc_info.out_ep,
                                              port_config->in_transfer_buffer_size, port_config->out_transfer_buffer_size), err, TAG, "Failed to allocate transfers");

    // Claim the interface
    ESP_GOTO_ON_ERROR(
        usb_host_interface_claim(
            p_usbh_cdc_obj->cdc_client_hdl,
            cdc_port->cdc_dev->dev_hdl,
            cdc_port->data.intf_desc->bInterfaceNumber,
            cdc_port->data.intf_desc->bAlternateSetting),
        err, TAG, "Could not claim interface");
    is_claimed_data = true;
    if (cdc_port->notif.xfer) {
        // If notification are supported, claim its interface and start polling its IN endpoint
        if (cdc_port->notif.intf_desc != cdc_port->data.intf_desc) {
            ESP_GOTO_ON_ERROR(
                usb_host_interface_claim(
                    p_usbh_cdc_obj->cdc_client_hdl,
                    cdc_port->cdc_dev->dev_hdl,
                    cdc_port->notif.intf_desc->bInterfaceNumber,
                    cdc_port->notif.intf_desc->bAlternateSetting),
                err, TAG, "Could not claim interface");
            is_claimed_notif = true;
        }
    }

    CDC_ENTER_CRITICAL();
    SLIST_INSERT_HEAD(&(p_usbh_cdc_obj->cdc_devices_port_list), cdc_port, list_entry);
    CDC_EXIT_CRITICAL();

    // Start polling for data. The port is already in the list, so a submission error has to be
    // handled by closing the port (deferred release) instead of freeing it here.
    esp_err_t submit_ret = ESP_OK;
    if (cdc_port->data.in_xfer) {
        if (port_config->flags & USBH_CDC_FLAGS_DEFER_DATA_POLL) {
            ESP_LOGD(TAG, "Defer BULK IN polling until usbh_cdc_port_start_data()");
        } else {
            submit_ret = _cdc_submit_transfer(cdc_port, cdc_port->data.in_xfer);
            if (submit_ret == ESP_OK) {
                cdc_port->data_started = true;
            } else {
                ESP_LOGE(TAG, "Failed to submit the BULK IN transfer: %s", esp_err_to_name(submit_ret));
            }
        }
    }
    if (submit_ret == ESP_OK && cdc_port->notif.xfer) {
        submit_ret = _cdc_submit_transfer(cdc_port, cdc_port->notif.xfer);
        if (submit_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to submit the notification transfer: %s", esp_err_to_name(submit_ret));
        }
    }
    if (submit_ret != ESP_OK) {
        // The port is in the list and might have a submitted transfer: hand it over to the
        // deferred release path so no URB completion can touch freed memory.
        xSemaphoreGive(p_usbh_cdc_obj->open_close_mutex);
        usbh_cdc_port_close((usbh_cdc_port_handle_t)cdc_port);
        return submit_ret;
    }

    usb_host_client_unblock(p_usbh_cdc_obj->cdc_client_hdl);
    *cdc_port_out = (usbh_cdc_port_handle_t)cdc_port;
    xSemaphoreGive(p_usbh_cdc_obj->open_close_mutex);
    return ESP_OK;

err:
    if (is_claimed_notif) {
        usb_host_interface_release(p_usbh_cdc_obj->cdc_client_hdl, cdc_port->cdc_dev->dev_hdl, cdc_port->notif.intf_desc->bInterfaceNumber);
    }
    if (is_claimed_data) {
        usb_host_interface_release(p_usbh_cdc_obj->cdc_client_hdl, cdc_port->cdc_dev->dev_hdl, cdc_port->data.intf_desc->bInterfaceNumber);
    }
    if (cdc_port->in_ringbuf_handle) {
        vRingbufferDelete(cdc_port->in_ringbuf_handle);
    }
    if (cdc_port->out_ringbuf_handle) {
        vRingbufferDelete(cdc_port->out_ringbuf_handle);
    }
    if (cdc_port->data.out_xfer_free_sem) {
        vSemaphoreDelete(cdc_port->data.out_xfer_free_sem);
    }
    if (cdc_port->data.out_mux) {
        vSemaphoreDelete(cdc_port->data.out_mux);
    }
    if (cdc_port->cb_mutex) {
        vSemaphoreDelete(cdc_port->cb_mutex);
    }

    if (cdc_port) {
        // Idempotent: _cdc_transfers_allocate() already cleaned up on its own error path
        _cdc_transfers_free(cdc_port);
        free(cdc_port);
    }
    if (new_cdc_dev) {
        esp_err_t err_ctrl = _cdc_dev_ctrl_free(new_cdc_dev);
        if (err_ctrl != ESP_OK) {
            ESP_LOGE(TAG, "Failed to free the control transfer: %s", esp_err_to_name(err_ctrl));
        }
        if (new_cdc_dev->dev_hdl != NULL) {
            esp_err_t err_close = usb_host_device_close(p_usbh_cdc_obj->cdc_client_hdl, new_cdc_dev->dev_hdl);
            if (err_close != ESP_OK) {
                ESP_LOGW(TAG, "Failed to close device %d: %s", new_cdc_dev->dev_addr, esp_err_to_name(err_close));
            }
        }
        if (new_dev_inserted) {
            CDC_ENTER_CRITICAL();
            SLIST_REMOVE(&(p_usbh_cdc_obj->cdc_devices_list), new_cdc_dev, usbh_cdc_dev_s, list_entry);
            CDC_EXIT_CRITICAL();
        }
        free(new_cdc_dev);
    }
err_exit:
    xSemaphoreGive(p_usbh_cdc_obj->open_close_mutex);
    return ret;
}

esp_err_t usbh_cdc_port_close(usbh_cdc_port_handle_t cdc_port_handle)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, TAG, "usb cdc is not installed");
    ESP_RETURN_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is NULL");

    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    ESP_RETURN_ON_FALSE(is_port_opened(cdc_port), ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is invalid");

    xSemaphoreTake(p_usbh_cdc_obj->open_close_mutex, portMAX_DELAY);

    // Re-check under the mutex: another task may have closed the port meanwhile
    CDC_ENTER_CRITICAL();
    bool already_closing = cdc_port->to_close;
    if (!already_closing) {
        cdc_port->to_close = true;
        cdc_port->data_started = false;
    }
    CDC_EXIT_CRITICAL();
    if (already_closing) {
        xSemaphoreGive(p_usbh_cdc_obj->open_close_mutex);
        return ESP_OK;
    }

    // Capture and clear the callbacks so that no new user callback is dispatched from now on.
    usbh_cdc_port_event_callbacks_t cbs;
    CDC_ENTER_CRITICAL();
    cbs = cdc_port->cbs;
    cdc_port->cbs.closed = NULL;
    cdc_port->cbs.recv_data = NULL;
    cdc_port->cbs.notif_cb = NULL;
    cdc_port->cbs.notif_cb_ex = NULL;
    SLIST_REMOVE(&(p_usbh_cdc_obj->cdc_devices_port_list), cdc_port, usbh_cdc_port_s, list_entry);
    CDC_EXIT_CRITICAL();

    // Wait for a user callback that is currently running (if any). From here on, no callback of
    // this port can be invoked anymore (the callbacks were cleared above and to_close is set), so
    // the caller may free its user_data as soon as this function returns.
    if (cdc_port->cb_mutex) {
        if (xSemaphoreTake(cdc_port->cb_mutex, pdMS_TO_TICKS(CDC_CLOSE_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            xSemaphoreGive(cdc_port->cb_mutex);
        } else {
            // Report the timeout: the caller must not free the user_data pointer yet.
            ESP_LOGE(TAG, "Timed out waiting for a port callback to finish");
            ret = ESP_ERR_TIMEOUT;
        }
    }

    usb_device_handle_t dev_hdl = cdc_port->cdc_dev ? cdc_port->cdc_dev->dev_hdl : NULL;
    if (dev_hdl != NULL) {
        // Serialize with the TX path. A writer does not hold out_mux while waiting for the TX
        // token without a timeout, so this wait is bounded.
        bool got_mux = (xSemaphoreTake(cdc_port->data.out_mux, pdMS_TO_TICKS(CDC_CLOSE_MUTEX_TIMEOUT_MS)) == pdTRUE);
        if (!got_mux) {
            ESP_LOGE(TAG, "Timed out waiting for the TX mutex, cancelling the transfers anyway");
        }
        // Retire the in-flight URBs. The completion callbacks are serviced by the USB client
        // task; the port object is released only after all of them returned (free_pending_list).
        if (cdc_port->data.in_xfer) {
            _cdc_reset_transfer_endpoint(dev_hdl, cdc_port->data.in_xfer);
        }
        if (cdc_port->data.out_xfer) {
            _cdc_reset_transfer_endpoint(dev_hdl, cdc_port->data.out_xfer);
        }
        if (cdc_port->notif.xfer) {
            _cdc_reset_transfer_endpoint(dev_hdl, cdc_port->notif.xfer);
        }
        if (got_mux) {
            xSemaphoreGive(cdc_port->data.out_mux);
        }
    }

    // Notify the upper layer. The port is not usable anymore and is not in the list, but its
    // memory is still valid here.
    if (cbs.closed) {
        cbs.closed(cdc_port_handle, cbs.user_data);
    }

    // Hand the port over to the client task for the final release
    xSemaphoreTake(p_usbh_cdc_obj->dev_event_mutex, portMAX_DELAY);
    cdc_port->pending_free = true;
    cdc_port->free_retries = 0;
    SLIST_INSERT_HEAD(&p_usbh_cdc_obj->free_pending_list, cdc_port, list_entry);
    xSemaphoreGive(p_usbh_cdc_obj->dev_event_mutex);

    xSemaphoreGive(p_usbh_cdc_obj->open_close_mutex);
    usb_host_client_unblock(p_usbh_cdc_obj->cdc_client_hdl);
    return ret;
}

esp_err_t usbh_cdc_port_close_all(void)
{
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, TAG, "usb cdc is not installed");
    for (int i = 0; i < 64; i++) {
        usbh_cdc_port_t *cdc_port = NULL;
        CDC_ENTER_CRITICAL();
        cdc_port = SLIST_FIRST(&p_usbh_cdc_obj->cdc_devices_port_list);
        CDC_EXIT_CRITICAL();
        if (cdc_port == NULL) {
            return ESP_OK;
        }
        esp_err_t ret = usbh_cdc_port_close((usbh_cdc_port_handle_t)cdc_port);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) {
            ESP_LOGE(TAG, "Failed to close a CDC port: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    ESP_LOGE(TAG, "Too many CDC ports to close");
    return ESP_ERR_INVALID_STATE;
}

esp_err_t usbh_cdc_port_start_data(usbh_cdc_port_handle_t cdc_port_handle)
{
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, TAG, "usb cdc is not installed");
    ESP_RETURN_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is NULL");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    ESP_RETURN_ON_FALSE(is_port_opened(cdc_port), ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is invalid");
    ESP_RETURN_ON_FALSE(cdc_port->data.in_xfer != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "port has no IN data endpoint");

    bool started;
    CDC_ENTER_CRITICAL();
    started = cdc_port->data_started;
    CDC_EXIT_CRITICAL();
    if (started) {
        return ESP_OK;
    }

    ESP_LOGD(TAG, "Submitting poll for BULK IN transfer");
    esp_err_t ret = _cdc_submit_transfer(cdc_port, cdc_port->data.in_xfer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit the BULK IN transfer: %s", esp_err_to_name(ret));
        return ret;
    }
    CDC_ENTER_CRITICAL();
    cdc_port->data_started = true;
    cdc_port->in_retry_count = 0;
    cdc_port->rx_error = false;
    CDC_EXIT_CRITICAL();
    return ESP_OK;
}

esp_err_t usbh_cdc_port_get_status(usbh_cdc_port_handle_t cdc_port_handle, usbh_cdc_port_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is NULL");
    if (p_usbh_cdc_obj == NULL || cdc_port_handle == NULL) {
        *status = USBH_CDC_PORT_STATUS_CLOSED;
        return ESP_OK;
    }
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    if (!is_port_opened(cdc_port)) {
        *status = USBH_CDC_PORT_STATUS_CLOSED;
        return ESP_OK;
    }
    if (cdc_port->rx_error || cdc_port->notif_error) {
        *status = USBH_CDC_PORT_STATUS_ERROR;
    } else if (cdc_port->data_started) {
        *status = USBH_CDC_PORT_STATUS_DATA_STARTED;
    } else {
        *status = USBH_CDC_PORT_STATUS_OPENED;
    }
    return ESP_OK;
}

/*------------------------------------------------ Control requests ----------------------------------------------------*/
esp_err_t usbh_cdc_send_custom_request_ex(usbh_cdc_port_handle_t cdc_port_handle, uint8_t bmRequestType, uint8_t bRequest,
                                          uint16_t wValue, uint16_t wIndex, uint16_t wLength, uint8_t *data,
                                          size_t *actual_length)
{
    if (actual_length) {
        *actual_length = 0;
    }
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, TAG, "usb cdc is not installed");
    ESP_RETURN_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is NULL");
    if (wLength > 0) {
        ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "data is NULL");
    }
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    ESP_RETURN_ON_FALSE(is_port_opened(cdc_port), ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is invalid");
    usbh_cdc_dev_t *cdc_dev = cdc_port->cdc_dev;
    ESP_RETURN_ON_FALSE(cdc_dev != NULL && cdc_dev->ctrl.xfer != NULL, ESP_ERR_INVALID_STATE, TAG, "device is not ready");

    // data_buffer_size includes the 8-byte setup packet, the payload must fit after it.
    const size_t setup_len = sizeof(usb_setup_packet_t);
    ESP_RETURN_ON_FALSE(cdc_dev->ctrl.xfer->data_buffer_size >= setup_len, ESP_ERR_INVALID_STATE, TAG, "control transfer buffer is too small");
    ESP_RETURN_ON_FALSE((size_t)wLength <= (cdc_dev->ctrl.xfer->data_buffer_size - setup_len),
                        ESP_ERR_INVALID_ARG, TAG, "wLength is larger than the control transfer buffer");

    if (xSemaphoreTake(cdc_dev->ctrl.mux, pdMS_TO_TICKS(CDC_CTRL_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for the control mutex");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    bool pending;
    CDC_ENTER_CRITICAL();
    pending = cdc_dev->ctrl.pending;
    CDC_EXIT_CRITICAL();
    if (pending) {
        // The previous control transfer is still in flight (it timed out but did not complete):
        // reusing the transfer/buffer now would corrupt it.
        ESP_LOGE(TAG, "The previous control transfer is still in flight");
        ret = ESP_ERR_INVALID_STATE;
        goto unblock;
    }

    usb_setup_packet_t *req = (usb_setup_packet_t *) cdc_dev->ctrl.xfer->data_buffer;
    uint8_t *start_of_data = (uint8_t *)req + setup_len;
    req->bmRequestType = bmRequestType;
    req->bRequest = bRequest;
    req->wValue = wValue;
    req->wIndex = wIndex;
    req->wLength = wLength;

    // For IN transfers we must transfer data ownership to CDC driver
    const bool in_transfer = bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN;
    if (!in_transfer) {
        memcpy(start_of_data, data, wLength);
    }

    cdc_dev->ctrl.xfer->num_bytes = wLength + setup_len;
    // Drop any stale completion token from a previous (timed out) transfer
    while (xSemaphoreTake(cdc_dev->ctrl.done, 0) == pdTRUE) {
    }
    CDC_ENTER_CRITICAL();
    cdc_dev->ctrl.pending = true;
    CDC_EXIT_CRITICAL();

    esp_err_t submit_ret = usb_host_transfer_submit_control(p_usbh_cdc_obj->cdc_client_hdl, cdc_dev->ctrl.xfer);
    if (submit_ret != ESP_OK) {
        CDC_ENTER_CRITICAL();
        cdc_dev->ctrl.pending = false;
        CDC_EXIT_CRITICAL();
        ESP_LOGE(TAG, "CTRL transfer submit failed: %s", esp_err_to_name(submit_ret));
        ret = submit_ret;
        goto unblock;
    }

    if (xSemaphoreTake(cdc_dev->ctrl.done, pdMS_TO_TICKS(CDC_CTRL_TIMEOUT_MS)) != pdTRUE) {
        // The transfer did not complete in time. Wait until the URB is actually retired so that
        // the transfer buffer is not reused while the HCD still references it.
        TickType_t start = xTaskGetTickCount();
        bool completed = false;
        while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(CDC_CTRL_TIMEOUT_MS)) {
            CDC_ENTER_CRITICAL();
            completed = !cdc_dev->ctrl.pending;
            CDC_EXIT_CRITICAL();
            if (completed) {
                // Drain the completion token that may have been given by the callback
                while (xSemaphoreTake(cdc_dev->ctrl.done, 0) == pdTRUE) {
                }
                break;
            }
            if (xSemaphoreTake(cdc_dev->ctrl.done, pdMS_TO_TICKS(50)) == pdTRUE) {
                completed = true;
                while (xSemaphoreTake(cdc_dev->ctrl.done, 0) == pdTRUE) {
                }
                break;
            }
        }
        if (!completed) {
            ESP_LOGE(TAG, "Control transfer is still in flight, the port must be reopened");
            ret = ESP_ERR_TIMEOUT;
            goto unblock;
        }
        ESP_LOGE(TAG, "Control transfer timed out");
        ret = ESP_ERR_TIMEOUT;
        goto unblock;
    }
    // Drain extra completion tokens (defensive)
    while (xSemaphoreTake(cdc_dev->ctrl.done, 0) == pdTRUE) {
    }

    ESP_LOGD(TAG, "ctrl actual_num_bytes = %d", cdc_dev->ctrl.xfer->actual_num_bytes);
    if (cdc_dev->ctrl.xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto unblock;
    }

    if (in_transfer) {
        size_t actual = 0;
        int actual_num_bytes = cdc_dev->ctrl.xfer->actual_num_bytes;
        if (actual_num_bytes > (int)setup_len) {
            actual = (size_t)(actual_num_bytes - setup_len);
        }
        if (actual > wLength) {
            actual = wLength;
        }
        memcpy(data, start_of_data, actual);
        if (actual_length) {
            *actual_length = actual;
        }
    } else if (actual_length) {
        *actual_length = wLength;
    }
    ret = ESP_OK;
unblock:
    xSemaphoreGive(cdc_dev->ctrl.mux);
    return ret;
}

esp_err_t usbh_cdc_send_custom_request(usbh_cdc_port_handle_t cdc_port_handle, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength, uint8_t *data)
{
    return usbh_cdc_send_custom_request_ex(cdc_port_handle, bmRequestType, bRequest, wValue, wIndex, wLength, data, NULL);
}

/*--------------------------------------------------- TX / RX paths -----------------------------------------------------*/
esp_err_t usbh_cdc_write_bytes(usbh_cdc_port_handle_t cdc_port_handle, const uint8_t *buf, size_t length, TickType_t ticks_to_wait)
{
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, TAG, "usb cdc is not installed");
    ESP_RETURN_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is NULL");
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_INVALID_ARG, TAG, "buf is NULL");
    ESP_RETURN_ON_FALSE(length > 0, ESP_ERR_INVALID_ARG, TAG, "length is 0");

    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    ESP_RETURN_ON_FALSE(is_port_opened(cdc_port), ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is invalid");
    ESP_RETURN_ON_FALSE(cdc_port->data.out_xfer != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "port has no OUT data endpoint");

    // P1-7: validate the length BEFORE taking any lock so an oversized write can never leave the
    // TX mutex locked.
    if (cdc_port->out_ringbuf_handle == NULL) {
        ESP_RETURN_ON_FALSE(length <= cdc_port->data.out_xfer->data_buffer_size, ESP_ERR_INVALID_SIZE, TAG,
                            "data length larger than transfer buffer size");
    }

    esp_err_t ret = ESP_OK;
    if (xSemaphoreTake(cdc_port->data.out_mux, ticks_to_wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    CDC_ENTER_CRITICAL();
    bool closing = cdc_port->to_close;
    CDC_EXIT_CRITICAL();
    if (closing) {
        ret = ESP_ERR_INVALID_STATE;
        goto fail;
    }

    if (cdc_port->out_ringbuf_handle) {
        // Push the data to the ring buffer and kick the TX path if it is idle
        ret = _ringbuf_push(cdc_port->out_ringbuf_handle, buf, length, ticks_to_wait);
        if (ret != ESP_OK) {
            goto fail;
        }
        if (xSemaphoreTake(cdc_port->data.out_xfer_free_sem, 0) == pdTRUE) {
            _cdc_tx_xfer_submit(cdc_port->data.out_xfer);
        }
    } else {
        // Wait for the TX token. The token is given back by out_xfer_cb() (also on
        // NO_DEVICE/CANCELED/error) or below when the submission fails. The wait is done in
        // small slices so a closing port does not keep the caller blocked for long.
        TickType_t start = xTaskGetTickCount();
        while (1) {
            if (xSemaphoreTake(cdc_port->data.out_xfer_free_sem, pdMS_TO_TICKS(20)) == pdTRUE) {
                break;
            }
            CDC_ENTER_CRITICAL();
            bool closing = cdc_port->to_close;
            CDC_EXIT_CRITICAL();
            if (closing) {
                ret = ESP_ERR_INVALID_STATE;
                goto fail;
            }
            if (ticks_to_wait != portMAX_DELAY && (xTaskGetTickCount() - start) >= ticks_to_wait) {
                ret = ESP_ERR_TIMEOUT;
                goto fail;
            }
        }
        memcpy(cdc_port->data.out_xfer->data_buffer, buf, length);
        cdc_port->data.out_xfer->num_bytes = length;
        ret = _cdc_submit_transfer(cdc_port, cdc_port->data.out_xfer);
        if (ret != ESP_OK) {
            // Submission failed: no completion callback will restore the token
            xSemaphoreGive(cdc_port->data.out_xfer_free_sem);
        }
    }
fail:
    xSemaphoreGive(cdc_port->data.out_mux);
    return ret;
}

esp_err_t usbh_cdc_read_bytes(usbh_cdc_port_handle_t cdc_port_handle, uint8_t *buf, size_t *length, TickType_t ticks_to_wait)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(length != NULL, ESP_ERR_INVALID_ARG, TAG, "length is NULL");
    ESP_GOTO_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, fail, TAG, "usb cdc is not installed");
    ESP_GOTO_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, fail, TAG, "cdc_port_handle is NULL");
    ESP_GOTO_ON_FALSE(buf != NULL, ESP_ERR_INVALID_ARG, fail, TAG, "buf is NULL");

    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    ESP_GOTO_ON_FALSE(is_port_opened(cdc_port), ESP_ERR_INVALID_ARG, fail, TAG, "cdc_port_handle is invalid");

    if (cdc_port->in_ringbuf_handle == NULL && ticks_to_wait != 0) {
        ESP_LOGW(TAG, "ticks_to_wait is invalid, in_ringbuf_handle is NULL");
    }

    if (cdc_port->in_ringbuf_handle) {
        size_t data_len = *length;
        if (data_len > cdc_port->in_ringbuf_size) {
            data_len = cdc_port->in_ringbuf_size;
        }

        ret = _ringbuf_pop(cdc_port->in_ringbuf_handle, buf, data_len, length, ticks_to_wait);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "cdc read failed");
            *length = 0;
            return ret;
        }
    } else {
        ESP_GOTO_ON_FALSE(cdc_port->data.in_xfer != NULL, ESP_ERR_NOT_SUPPORTED, fail, TAG, "port has no IN data endpoint");
        if (*length != (size_t)cdc_port->data.in_xfer->actual_num_bytes) {
            // the length must be equal to actual_num_bytes, because we always read all the data in the buffer.
            ESP_LOGE(TAG, "length is invalid, length = %d, actual_num_bytes = %d", *length, cdc_port->data.in_xfer->actual_num_bytes);
            *length = cdc_port->data.in_xfer->actual_num_bytes;
            return ESP_ERR_INVALID_ARG;
        }
        memcpy((void *)buf, cdc_port->data.in_xfer->data_buffer, cdc_port->data.in_xfer->actual_num_bytes);
        *length = cdc_port->data.in_xfer->actual_num_bytes;
        cdc_port->data.in_xfer->actual_num_bytes = 0; // reset actual_num_bytes
    }
    return ESP_OK;
fail:
    *length = 0;
    return ret;
}

esp_err_t usbh_cdc_flush_rx_buffer(usbh_cdc_port_handle_t cdc_port_handle)
{
    ESP_RETURN_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is NULL");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    ESP_RETURN_ON_FALSE(is_port_opened(cdc_port), ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is invalid");
    ESP_RETURN_ON_FALSE(cdc_port->in_ringbuf_handle != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "rx ringbuf is not created");
    _ringbuf_flush(cdc_port->in_ringbuf_handle);
    return ESP_OK;
}

esp_err_t usbh_cdc_flush_tx_buffer(usbh_cdc_port_handle_t cdc_port_handle)
{
    ESP_RETURN_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is NULL");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    ESP_RETURN_ON_FALSE(is_port_opened(cdc_port), ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is invalid");
    ESP_RETURN_ON_FALSE(cdc_port->out_ringbuf_handle != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "tx ringbuf is not created");
    _ringbuf_flush(cdc_port->out_ringbuf_handle);
    return ESP_OK;
}

esp_err_t usbh_cdc_get_rx_buffer_size(usbh_cdc_port_handle_t cdc_port_handle, size_t *size)
{
    ESP_RETURN_ON_FALSE(size != NULL, ESP_ERR_INVALID_ARG, TAG, "size is NULL");
    ESP_RETURN_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is NULL");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    ESP_RETURN_ON_FALSE(is_port_opened(cdc_port), ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is invalid");
    if (cdc_port->in_ringbuf_handle) {
        *size = _get_ringbuf_len(cdc_port->in_ringbuf_handle);
    } else {
        ESP_RETURN_ON_FALSE(cdc_port->data.in_xfer != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "port has no IN data endpoint");
        *size = cdc_port->data.in_xfer->actual_num_bytes;
    }
    return ESP_OK;
}

esp_err_t usbh_cdc_desc_print(usbh_cdc_port_handle_t port_handle)
{
    ESP_RETURN_ON_FALSE(port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "port_handle is NULL");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) port_handle;
    ESP_RETURN_ON_FALSE(is_port_opened(cdc_port), ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is invalid");
    usbh_cdc_dev_t *cdc_dev = cdc_port->cdc_dev;
    ESP_RETURN_ON_FALSE(cdc_dev->dev_hdl != NULL, ESP_ERR_INVALID_STATE, TAG, "Device is not open yet");

    const usb_device_desc_t *device_desc = NULL;
    const usb_config_desc_t *config_desc = NULL;
    ESP_RETURN_ON_ERROR(usb_host_get_device_descriptor(cdc_dev->dev_hdl, &device_desc), TAG, "Failed to get the device descriptor");
    ESP_RETURN_ON_ERROR(usb_host_get_active_config_descriptor(cdc_dev->dev_hdl, &config_desc), TAG, "Failed to get the configuration descriptor");
    usb_print_device_descriptor(device_desc);
    usb_print_config_descriptor(config_desc, NULL);
    return ESP_OK;
}

esp_err_t usbh_cdc_get_dev_handle(usbh_cdc_port_handle_t port_handle, usb_device_handle_t *dev_handle)
{
    ESP_RETURN_ON_FALSE(port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "port_handle is NULL");
    ESP_RETURN_ON_FALSE(dev_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "dev_handle is NULL");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) port_handle;
    ESP_RETURN_ON_FALSE(is_port_opened(cdc_port), ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is invalid");
    usbh_cdc_dev_t *cdc_dev = cdc_port->cdc_dev;
    ESP_RETURN_ON_FALSE(cdc_dev->dev_hdl != NULL, ESP_ERR_INVALID_STATE, TAG, "Device is not open yet");
    *dev_handle = cdc_dev->dev_hdl;
    return ESP_OK;
}

esp_err_t usbh_cdc_port_get_intf_desc(usbh_cdc_port_handle_t port_handle, const usb_intf_desc_t **notif_intf,  const usb_intf_desc_t **data_intf)
{
    ESP_RETURN_ON_FALSE(port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "port_handle is NULL");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) port_handle;
    ESP_RETURN_ON_FALSE(is_port_opened(cdc_port), ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is invalid");
    ESP_RETURN_ON_FALSE(cdc_port->cdc_dev->dev_hdl != NULL, ESP_ERR_INVALID_STATE, TAG, "Device is not open yet");
    if (notif_intf) {
        *notif_intf = cdc_port->notif.intf_desc;
    }
    if (data_intf) {
        *data_intf = cdc_port->data.intf_desc;
    }
    return ESP_OK;
}
