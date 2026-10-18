idf_component_register(
    SRCS "cpe_config.c" "cpe_auth.c" "cpe_status.c" "cpe_recovery.c" "cpe_core.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_timer
    PRIV_REQUIRES mbedtls esp_hw_support
)
