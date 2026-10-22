# Periodic maintenance: scheduled ECM interface rebuild and device reboot.
idf_component_register(
    SRCS "cpe_maintenance.c"
    INCLUDE_DIRS "include"
    REQUIRES cpe_core cpe_wan esp_timer nvs_flash
)
