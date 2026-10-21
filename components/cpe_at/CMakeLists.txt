# Read-only AT telemetry: opens a second CDC port on the modem AT interface
# (in addition to the ECM data port) and polls signal/registration data into
# cpe_status. No state-changing AT command is ever sent.
idf_component_register(SRCS "cpe_at.c"
                INCLUDE_DIRS "include"
                PRIV_REQUIRES iot_usbh_cdc cpe_core usb)
