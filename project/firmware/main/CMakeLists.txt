idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES board audio_io recorder
    rolling_audio recorder_core spi_flash network identity provisioning cloud_sync
    voice_client gui lan_server)
