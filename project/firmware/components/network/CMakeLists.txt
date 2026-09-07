idf_component_register(SRCS "network.c" "secure_store.c" "credential_storage.c" "https.c" "proxy_endpoint.c"
    INCLUDE_DIRS "include" REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_client
    mbedtls esp_timer espressif__cjson recorder_core nvs_sec_provider efuse)
