# The management web console is a mandatory part of the CPE product: it is the
# only way to configure the device and to diagnose a dead 4G link. It is
# therefore always compiled (there is deliberately no "enable web router"
# switch that would produce a firmware which fails to build when disabled).
set(PRIV_REQ esp_wifi esp_event esp_system esp_hw_support esp_timer
             nvs_flash spiffs vfs lwip esp_http_server)

idf_component_register(SRC_DIRS "." "router_web"
                INCLUDE_DIRS "." "include" "router_web"
                REQUIRES cpe_core esp_netif
                PRIV_REQUIRES ${PRIV_REQ})

spiffs_create_partition_image(storage ./spiffs FLASH_IN_PROJECT)
