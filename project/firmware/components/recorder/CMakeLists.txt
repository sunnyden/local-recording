idf_component_register(SRCS "storage.c" "recording.c" "recording_name.c" INCLUDE_DIRS "include"
    REQUIRES board audio_io recorder_core esp_timer)
