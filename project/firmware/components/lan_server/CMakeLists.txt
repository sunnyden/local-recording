idf_component_register(SRCS "lan_server.c" INCLUDE_DIRS "include"
    REQUIRES esp_http_server espressif__mdns recorder recorder_core heap freertos)
