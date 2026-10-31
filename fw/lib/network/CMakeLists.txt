idf_component_register(
    SRCS "network.c"
    INCLUDE_DIRS "."
    REQUIRES esp_common freertos audio
    PRIV_REQUIRES
        storage
        esp_netif
        esp_event
        esp_wifi
        esp_timer
        lwip
        esp_hw_support
        freertos
        espressif__iot_bridge
        espressif__mesh_lite
)
