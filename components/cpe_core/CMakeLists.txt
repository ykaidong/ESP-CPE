idf_component_register(
    SRCS "cpe_config.c" "cpe_config_store.c" "cpe_auth.c" "cpe_status.c" "cpe_recovery.c" "cpe_ota.c" "cpe_core.c" "cpe_ct.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES nvs_flash esp_timer app_update esp_partition esp_app_format
    PRIV_REQUIRES mbedtls esp_hw_support
)
