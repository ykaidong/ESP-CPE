# ChangeLog

## v3.1.1 - 2026-10-17

Local ESP-CPE fork (not an upstream release). Bug fixes:

* Fix `usbh_cdc_port_close()` use-after-free: the port object, transfers and ring buffers are now released by the driver task after all the completion callbacks have been drained (two-phase close, no `vTaskDelay()` heuristic).
* Fix double free of the transfers in the `usbh_cdc_port_open()` error path.
* Fix `usbh_cdc_driver_install()` leaving a dangling global handle on failure.
* Protect the device event callback list with a mutex; the callback dispatch and the register/unregister/uninstall paths are synchronized.
* Fix `usbh_cdc_send_custom_request()` out-of-bounds write (the 8-byte setup packet was not accounted) and drain the control transfer on timeout; add `usbh_cdc_send_custom_request_ex()` reporting the actual transfer length.
* Fix `usbh_cdc_write_bytes()` TX mutex leak, lost TX token on submit failure/NO_DEVICE/CANCELED and the close/writer lock order (the length is validated before taking the lock).
* Replay `CDC_HOST_DEVICE_EVENT_CONNECTED` for already enumerated devices after `usbh_cdc_register_dev_event_cb()`; the client event queue is raised from 3 to 8 messages.
* Add `USBH_CDC_FLAGS_DEFER_DATA_POLL` + `usbh_cdc_port_start_data()` so the BULK IN polling starts only after the data interface is configured.
* IN/notification retry counters are now per port; on exhaustion the port reports `USBH_CDC_PORT_STATUS_ERROR` (`usbh_cdc_port_get_status()`) and can be restarted with `usbh_cdc_port_start_data()`.
* Add `usbh_cdc_port_close_all()`; `usbh_cdc_driver_uninstall()` now closes the remaining ports, is idempotent and reports errors instead of aborting.
* Add `usbh_cdc_driver_config_t::enum_filter_cb` and pass it to `usb_host_install()`; replace the `ESP_ERROR_CHECK()` calls of the internal USB Host task with explicit error handling.
* Device/configuration descriptors are delivered to the event callbacks as a copy valid for the whole callback.
* The CDC data interface is located with the Union functional descriptor (fallback: data class interface scan) instead of assuming "control interface + 1"; IAD matching uses the interface range instead of requiring exactly 2 interfaces.
* Fix the ring buffer flush for wrapped buffers, `read_bytes()`/`get_rx_buffer_size()` NULL checks and notification length handling.

## v3.1.0 - 2026-06-11

### Features:

* Support ESP32-S31.

### Bug Fixes:

* Allow CDC notification interfaces without interrupt endpoints.

## v3.0.0 - 2025-10-15

### Features:

* Add `usbh_cdc_port_open()` and `usbh_cdc_port_close()` to open and close the CDC port on a device.
* Add API `usbh_cdc_get_dev_handle`, `usbh_cdc_port_get_intf_desc`
* Support force disable notification transfer to save channel of host controller.

### Break Changes:

* The `usbh_cdc_device_config_t` structure is modified to adapt to the new API.
* Modify API `usbh_cdc_register_new_dev_cb()` and `usbh_cdc_unregister_new_dev_cb()` to `usbh_cdc_register_dev_event_cb` and `usbh_cdc_unregister_dev_event_cb`.
* Remove API `usbh_cdc_get_state` `usbh_cdc_create`

## v2.0.1 - 2025-06-17

### Bug Fixes:

* Fix the issue of incorrectly initiating `tx_xfer` multiple times.

## v2.0.0 - 2025-05-20

### Features:

* Support no internal ring buffer mode..

### Break Changes:

* Set rx_buffer_size to 0 to disable internal ring buffer.

* Remove macro IN_RINGBUFFER_SIZE and OUT_RINGBUFFER_SIZE.

## v1.1.1 - 2025-05-13

### Features:

* Skip open usb hub device.

## v1.1.0 - 2025-04-21

### Features:

* Support control transfer and notification transfer.

* Support usbh_cdc_send_custom_request() to send custom request.

## v1.0.4 - 2025-4-1

### Bug Fixes:

* Fix USB host interface release when URB is not cleared.

## v1.0.3 - 2025-3-27

### Bug Fixes:

* Fixed an incorrect `ring_buffer pop` request value of 0.
* Fixed uninitialized structure issue.

## v1.0.2 - 2025-3-13

### Bug Fixes:

* Close 'extern C' block
* flush the ringbuffer when usb disconnect

## v1.0.1 - 2025-2-26

### Bug Fixes:

* Fix the cdc can't open the setting's interface cause crash issue.

## v1.0.0 - 2024-10-31

### Break Changes:

* Remove the dependency on the `iot_usbh` component and use `usb_host_driver` instead.

### API Break Changes:

* Add `usbh_cdc_create()`
* Add `usbh_cdc_delete()`
* Add `usbh_cdc_desc_print()` to print CDC descriptor
* Rename `usbh_cdc_get_itf_state()` to `usbh_cdc_get_state()`
* Remove `usbh_cdc_wait_connect()`
* Remove `usbh_cdc_itf_write_bytes()`
* Remove `usbh_cdc_get_buffered_data_len()`
* Remove `usbh_cdc_itf_get_buffered_data_len()`
* Remove `usbh_cdc_itf_read_bytes()`

## v0.2.2 - 2023-12-08

* Add doc and example

## v0.2.1 - 2023-11-23

* Fix possible cmake_utilities dependency issue

## v0.2.0 - 2023-10-09

* add hardware test
* decrease the stack priority to `usb base` level

## v0.1.3 - 2023-04-17

### Bug Fixes:

* Update iot_usbh version to 0.1.2, fix build error with latest ESP-IDF `release/v4.4`

## v0.1.2 - 2023-02-21

### Bug Fixes:

* Fix the multi-thread access error caused by `usbh_cdc_driver_delete()`

## v0.1.1 - 2023-02-13

* Support IDF5.0
* Change dependency iot_usbh to public

## v0.1.0 - 2023-02-02

* initial version
