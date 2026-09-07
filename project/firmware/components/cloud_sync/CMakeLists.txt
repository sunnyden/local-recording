idf_component_register(SRCS "cloud_sync.c" "upload.c" "processing_outbox.c" "processing_client.c" "sync_ui.c" INCLUDE_DIRS "include"
    REQUIRES recorder board network identity mbedtls esp_http_client)
