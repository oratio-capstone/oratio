idf_component_register(
    SRCS "audio_output.c" "audio_playback.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_common
    PRIV_REQUIRES esp_driver_i2s esp_timer log freertos
)
