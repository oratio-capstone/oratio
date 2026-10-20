idf_component_register(
    SRCS "network.c"
    INCLUDE_DIRS "."
    REQUIRES esp_common
    PRIV_REQUIRES
        nvs_flash
        esp_netif
        esp_event
        esp_wifi
        esp_hw_support
        freertos
        espressif__iot_bridge
        espressif__mesh_lite
)