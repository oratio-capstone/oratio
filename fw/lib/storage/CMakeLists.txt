idf_component_register(
    SRCS "storage.c"
    INCLUDE_DIRS "."
    REQUIRES esp_common
    PRIV_REQUIRES nvs_flash log
)
