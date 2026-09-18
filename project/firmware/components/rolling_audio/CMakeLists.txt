idf_component_register(SRCS "rolling_audio.c" INCLUDE_DIRS "include"
    REQUIRES audio_io recorder recorder_core heap esp_hw_support)
