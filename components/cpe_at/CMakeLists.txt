# AT telemetry: opens a second CDC port on the modem AT interface (in
# addition to the ECM data port) and polls signal/registration data into
# cpe_status. Telemetry is read-only; the single state-changing command is
# AT+CFUN=1,1, sent only when a deliberate restart asks to reset the modem.
idf_component_register(SRCS "cpe_at.c"
                INCLUDE_DIRS "include"
                PRIV_REQUIRES iot_usbh_cdc cpe_core usb)
