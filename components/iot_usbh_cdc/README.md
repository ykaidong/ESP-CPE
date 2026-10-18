## iot_usbh_cdc Component

This component implements a simple version of the USB host CDC driver. 

**Features:**

1. Support USB CDC device
2. Support USB Vendor device
3. Support USB CDC multiple interface
4. Support USB HUB to connect multiple USB devices

> **Local ESP-CPE fork (v3.1.1).** This is the registry v3.1.0 plus the reliability patches listed in
> [CHANGELOG.md](CHANGELOG.md). It is used as the USB transport of the ESP-CPE 4G CPE firmware. Notable
> additions on top of the upstream API:
>
> * `USBH_CDC_FLAGS_DEFER_DATA_POLL` + `usbh_cdc_port_start_data()`: open a port without starting the
>   BULK IN polling and start it once the data interface is configured (e.g. after `SET_INTERFACE`).
> * `usbh_cdc_port_close_all()`: close every open port (used before uninstalling the driver).
> * `usbh_cdc_send_custom_request_ex()`: like `usbh_cdc_send_custom_request()`, but also returns the
>   actual number of payload bytes transferred, so control IN responses can be validated.
> * `usbh_cdc_port_get_status()`: query whether the port is open, whether the BULK IN polling runs, or
>   whether it stopped after repeated transfer errors.
> * `usbh_cdc_port_event_callbacks_t::notif_cb_ex`: notification callback with the received length.
> * `usbh_cdc_driver_config_t::enum_filter_cb`: optional USB enumeration filter callback passed to
>   `usb_host_install()` when the driver installs the USB Host Library itself.
>
> Behavior notes: `usbh_cdc_port_close()` can be called from any task and guarantees that no port
> callback is invoked after it returns (the resources are released by the driver task once the
> completion callbacks have drained). `usbh_cdc_register_dev_event_cb()` replays
> `CDC_HOST_DEVICE_EVENT_CONNECTED` for devices that finished enumeration before the callback was
> registered. `usbh_cdc_driver_uninstall()` is idempotent and closes the remaining ports.

### User Guide

Please refer: https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_host/usb_host_iot_usbh_cdc.html

### Add component to your project

Please use the component manager command `add-dependency` to add the `iot_usbh_cdc` to your project's dependency, during the `CMake` step the component will be downloaded automatically

```
idf.py add-dependency "espressif/iot_usbh_cdc=*"
```

### Examples

Please use the component manager command `create-project-from-example` to create the project from the example template

* USB Host CDC Basic Example

```
idf.py create-project-from-example "espressif/iot_usbh_cdc=*:usb_cdc_basic"
```

Then the example will be downloaded in the current folder, you can check into it for build and flash.

> Or you can download examples from esp-iot-solution repository: [usb_cdc_basic](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/host/usb_cdc_basic).
