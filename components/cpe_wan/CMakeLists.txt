# The 4G WAN domain: USB host/CDC/ECM lifecycle, AP-side NAPT/DNS and the link
# manager with its recovery policy. main only orchestrates the boot order, and
# nothing outside this component needs its internal headers.
idf_component_register(SRCS "usb_wan.c" "wan_net.c" "link_manager.c"
                INCLUDE_DIRS "include"
                PRIV_INCLUDE_DIRS "private_include"
                PRIV_REQUIRES cpe_core cpe_at app_wifi iot_eth iot_usbh_ecm
                              iot_usbh_cdc usb esp_netif esp_event esp_timer
                              esp_wifi lwip esp_driver_gpio)
