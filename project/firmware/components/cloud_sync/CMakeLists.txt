idf_component_register(SRCS "cloud_sync.c" "upload.c" INCLUDE_DIRS "include"
    REQUIRES recorder board network identity mbedtls esp_http_client)
