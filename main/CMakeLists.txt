idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "."
                       REQUIRES cpe_core app_wifi web_console cpe_wan cpe_at cpe_maintenance cpe_sms
                                cpe_sms_mqtt
                                esp_driver_gpio esp_driver_rmt esp_netif esp_event nvs_flash esp_timer)
