# The management web console is a mandatory part of the CPE product: it is the
# only way to configure the device and to diagnose a dead 4G link. It is
# therefore always compiled (there is deliberately no "enable web router"
# switch that would produce a firmware which fails to build when disabled).
# cJSON comes from the component manager (idf_component.yml) and is added to
# the requirements automatically, so it is not listed here.
set(PRIV_REQ esp_wifi esp_event esp_timer nvs_flash spiffs vfs
             lwip esp_http_server cpe_core app_wifi esp_netif)

idf_component_register(SRC_DIRS "router_web"
                INCLUDE_DIRS "include"
                PRIV_INCLUDE_DIRS "router_web/private_include"
                PRIV_REQUIRES ${PRIV_REQ})

spiffs_create_partition_image(storage ./spiffs FLASH_IN_PROJECT)
