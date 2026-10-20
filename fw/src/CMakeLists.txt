idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES
        network
        esp_common
        log
        freertos
        lwip
)