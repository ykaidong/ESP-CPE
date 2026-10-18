idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "."
                       REQUIRES cpe_core web_wifi iot_eth iot_usbh_ecm iot_usbh_cdc esp_driver_gpio
                                esp_netif nvs_flash esp_timer lwip)
