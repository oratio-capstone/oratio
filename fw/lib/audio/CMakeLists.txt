idf_component_register(
    SRCS "audio_output.c" "playback.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2s esp_common freertos
    PRIV_REQUIRES log
)
