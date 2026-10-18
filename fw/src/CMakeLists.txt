# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_netif
        esp_event
        esp_wifi
        esp_hw_support
        espressif__iot_bridge
        espressif__mesh_lite
)
