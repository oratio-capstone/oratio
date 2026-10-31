idf_component_register(
    SRCS "audio_capture.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_common freertos
    PRIV_REQUIRES esp_driver_i2s log
)
