# SMS forwarding over MQTTS. cpe_sms_mqtt_proto.c is pure logic covered by
# tests/host; the store and the task talk to NVS, esp-mqtt and the SMS
# component. The task file compiles to stubs when CONFIG_CPE_SMS_MQTT_ENABLE is
# off, so the console always links against one API.
idf_component_register(SRCS "cpe_sms_mqtt_proto.c" "cpe_sms_mqtt_store.c"
                            "cpe_sms_mqtt_task.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       REQUIRES cpe_core
                       PRIV_REQUIRES cpe_sms esp_timer esp_event esp_netif mqtt mbedtls
                                     nvs_flash)
