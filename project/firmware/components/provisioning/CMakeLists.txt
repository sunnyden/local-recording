idf_component_register(SRCS "provisioning.c" "control.c" INCLUDE_DIRS "include"
    REQUIRES board identity network recorder_core protocomm espressif__network_provisioning esp_timer espressif__cjson)
