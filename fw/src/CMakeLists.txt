if(ORATIO_AUDIO_OUTPUT_TEST)
    set(app_source "../test/test_148_audio_output/test_main.c")
else()
    set(app_source "main.c")
endif()

idf_component_register(
    SRCS "${app_source}"
    INCLUDE_DIRS "."
    REQUIRES audio esp_driver_gpio esp_timer freertos log
)
