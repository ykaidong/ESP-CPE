# SoftAP lifecycle only. The management console and the 4G/ECM link live in
# their own components; this one never depends on either.
idf_component_register(SRCS "app_wifi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES cpe_core esp_netif
                       PRIV_REQUIRES esp_wifi esp_event lwip)
