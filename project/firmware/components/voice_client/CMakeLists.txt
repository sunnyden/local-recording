idf_component_register(SRCS "voice_client.c" INCLUDE_DIRS "include"
    REQUIRES audio_io board recorder_core network identity espressif__esp_websocket_client esp_timer heap)
