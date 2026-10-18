# ChangeLog

## v0.4.2 - 2026-10-17

Local ESP-CPE fork. Field bug fix found on real hardware (ECM module VID 2CA3/PID 4006):

* Fixed `_parse_mac_descriptor()`: the nibble counter was incremented per MAC **byte** but
  compared against 12 (the number of hex **characters**), so every device that exposes a
  MAC address string failed the parse. The ECM configuration was then aborted and the link
  was reported down after 3 attempts. The parser now stops after 6 bytes and also requires
  an even number of nibbles.
* A missing/invalid MAC address string is no longer fatal: a locally administered fallback
  MAC is generated and the ECM configuration continues (the data path does not depend on the
  MAC string).
* Re-validate the cached CDC port handle before every control request during configuration
  (`_ecm_port_is_current()`), so a device that disappears mid-configuration cannot cause a
  use-after-free.
* Cache the NETWORK_CONNECTION notification that races with the port opening and apply it
  after the handle is registered (prevents a false auto-link-up when the modem reports
  "disconnected" before `ecm->cdc_port` is assigned).
* MAC-address string read: use the long-form GET_DESCRIPTOR with the reported length and
  accept both `001122334455` and `00:11:22:33:44:55`.
* Watchdog: if the CDC driver stopped BULK IN polling after repeated transfer errors, restart
  it from the ECM task instead of leaving the WAN data path silently dead.

## v0.4.1 - 2026-10-17

Local ESP-CPE fork (not an upstream release). Bug fixes:

* `usbh_ecm_deinit()` now closes the CDC port before freeing the ECM object (fixes the callback use-after-free and allows the CDC driver to be uninstalled). The teardown is bounded and idempotent.
* `_ecm_config_intf()` now returns an error when the MAC address cannot be read/parsed; the task reports link down and never reports a fake link-up when the data path is not configured.
* Do not guess the data interface: select it from the CDC Union/functional descriptors and use its reported alternate setting for SET_INTERFACE.
* Clamp `wMaxSegmentSize` (512..16384, rounded up to 512) before allocating the transfer buffers.
* Validate the notification length before reading the SPEED_CHANGE payload; accept both the `001122334455` and `00:11:22:33:44:55` MAC address string layouts.
* Keep the deferred (pending) link state reporting; add state locking and bounded configuration retries.
* Remove the duplicate `usb_ecm_function_desc_t` definition (use `usb_cdc_ethernet_networking_func_desc_t`).

## v0.4.0 - 2026-06-30

### Features:

* Add support for the `iot_eth` input callback with extra RX frame information.

## v0.3.0 - 2026-06-11

### Features:

* Support ESP32-S31.

## v0.2.1 - 2026-01-12

### Bug Fixes:

* Fixed incorrect interface number in ECM_SET_ETHERNET_PACKET_FILTER request. Changed from hardcoded 0 to use actual interface number (`ecm->itf_num`).

## v0.2.0 - 2025-10-15

### Bug Fixes:

* Added configuration `ECM_AUTO_LINK_UP_AFTER_NO_NOTIFICATION` to improved driver compatibility with 4G and Ethernet modules.
* Fixed hot-plugging stability issues.
* Added test cases to verify the functionality of the ECM driver.

## v0.1.0 - 2025-04-22

* publish official version
