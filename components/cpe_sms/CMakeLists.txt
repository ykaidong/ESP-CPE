# Optional SMS over the modem AT interface. cpe_sms_parse.c / cpe_sms_ucs2.c are
# pure logic (no ESP-IDF dependency) and are covered by tests/host; cpe_sms.c
# implements the AT transactions on top of cpe_at's serialized session API.
idf_component_register(SRCS "cpe_sms.c" "cpe_sms_parse.c" "cpe_sms_ucs2.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES cpe_at esp_timer)
