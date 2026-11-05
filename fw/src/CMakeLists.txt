# Build the normal firmware, or the capture test when ORATIO_AUDIO_CAPTURE_TEST is on.
if(ORATIO_AUDIO_CAPTURE_TEST)
    set(app_source "../test/test_157_audio_capture/test_main.c")
else()
    set(app_source "main.c")
endif()

idf_component_register(
    SRCS ${app_source}
    INCLUDE_DIRS "."
    REQUIRES audio esp_driver_gpio freertos log
)
