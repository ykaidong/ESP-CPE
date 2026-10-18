[![Component Registry](https://components.espressif.com/components/espressif/iot_usbh_ecm/badge.svg)](https://components.espressif.com/components/espressif/iot_usbh_ecm)

## IOT USBH ECM Component

`iot_usbh_ecm` is a USB `ECM` device driver for ESP32-S2/ESP32-S3/ESP32-P4. It acts as a USB host to connect ECM devices.

Features:

1. Support hot-plug of ECM devices
2. Support auto-detection and connection of ECM devices
3. Support auto-detection of MAC address

> **Local ESP-CPE fork (v0.4.1).** This is the registry v0.4.0 plus the reliability patches listed in
> [CHANGELOG.md](CHANGELOG.md): `usbh_ecm_deinit()` closes the CDC port before freeing the ECM object
> (fixes the callback use-after-free and allows the CDC driver to be uninstalled), a failed interface
> configuration is reported as link down instead of a fake link-up, the ECM data interface is located
> through the CDC Union/functional descriptors instead of the "control interface + 1" assumption,
> `wMaxSegmentSize` is clamped before allocating the transfer buffers, the notification length is
> validated, and the deferred (pending) link-up reporting is kept. The public API
> (`iot_eth_new_usb_ecm()`, `usb_ecm_get_cdc_port_handle()`) is unchanged.

### Add component to your project

Please use the component manager command `add-dependency` to add the `iot_usbh_ecm` to your project's dependency, during the `CMake` step the component will be downloaded automatically

```
idf.py add-dependency "espressif/iot_usbh_ecm=*"
```

### Docs

* [IOT USBH ECM](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_host/usb_ecm.html)

### Examples

* [USB ECM](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/host/usb_ecm_4g_module)
