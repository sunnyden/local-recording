set(sources "")
if(CONFIG_RECORDER_GUI)
    list(APPEND sources gui_draw.c gui_screens.c gui_app.c assets/gui_assets.c assets/gui_fonts.c)
endif()
if(CONFIG_RECORDER_GUI_TEST_INPUT)
    list(APPEND sources gui_test_input.c gui_console.c)
endif()
idf_component_register(SRCS ${sources} INCLUDE_DIRS include
    REQUIRES board recorder rolling_audio recorder_core provisioning cloud_sync
    voice_client network identity esp_timer lan_server)
