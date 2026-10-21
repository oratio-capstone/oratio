#include "network.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_bridge.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh_lite.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/* Stop compilation if the build environment has not selected a valid firmware role. */
#ifndef APP_IS_ROOT
#error "APP_IS_ROOT must be supplied by the build environment"
#endif

#if (APP_IS_ROOT != 0) && (APP_IS_ROOT != 1)
#error "APP_IS_ROOT must be 0 or 1"
#endif

#if !defined(CONFIG_MESH_LITE_ENABLE)
#error "Enable Mesh-Lite in menuconfig"
#endif

/* A Wi-Fi SSID holds up to 32 bytes, plus one byte for the string terminator. */
#define SOFTAP_SSID_BUFFER_SIZE 33

static const char *TAG = "MESH_NETWORK";
static const bool IS_ROOT = (APP_IS_ROOT == 1);
static const uint8_t ROOT_LEVEL = 1;

/* Initial SoftAP channel; the upstream connection may determine the operating channel later. */
static const uint8_t MESH_CHANNEL = 6;

/* Keep the generated network name available for configuration and later status logs. */
static char s_softap_ssid[SOFTAP_SSID_BUFFER_SIZE];

/* Initialize nonvolatile storage, which Wi-Fi and Mesh-Lite use for persistent settings. */
static esp_err_t storage_init(void)
{
    esp_err_t result = nvs_flash_init();

    /* These initialization errors require erasing the NVS partition before retrying. */
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "Reinitializing NVS; stored settings will be erased");

        ESP_RETURN_ON_ERROR(
            nvs_flash_erase(), TAG, "Failed to erase NVS");

        result = nvs_flash_init();
    }

    return result;
}

/* Configure the station interface for joining Wi-Fi and the SoftAP interface for mesh children. */
static esp_err_t mesh_wifi_configure(void)
{
    wifi_config_t station_config = {0};

    /* Only the root receives laptop hotspot credentials; other nodes find a mesh parent. */
    if (IS_ROOT)
    {
        const size_t ssid_length = strlen(CONFIG_ORATIO_ROUTER_SSID);
        const size_t password_length = strlen(CONFIG_ORATIO_ROUTER_PASSWORD);

        /* Reject an empty SSID or credentials that will not fit in the destination arrays. */
        if (ssid_length == 0 ||
            ssid_length > sizeof(station_config.sta.ssid) ||
            password_length >= sizeof(station_config.sta.password))
        {
            ESP_LOGE(TAG, "Hotspot credentials are empty or exceed supported lengths");
            return ESP_ERR_INVALID_ARG;
        }

        memcpy(
            station_config.sta.ssid,
            CONFIG_ORATIO_ROUTER_SSID,
            ssid_length);

        memcpy(
            station_config.sta.password,
            CONFIG_ORATIO_ROUTER_PASSWORD,
            password_length);
    }

    ESP_RETURN_ON_ERROR(
        esp_bridge_wifi_set_config(WIFI_IF_STA, &station_config),
        TAG,
        "Failed to configure station");

    /* Provide the initial Wi-Fi settings for the access point hosted by this board. */
    wifi_config_t softap_config =
        {
            .ap =
                {
                    .ssid = CONFIG_BRIDGE_SOFTAP_SSID,
                    .password = CONFIG_BRIDGE_SOFTAP_PASSWORD,
                    .channel = MESH_CHANNEL,
                },
    };

    return esp_bridge_wifi_set_config(WIFI_IF_AP, &softap_config);
}

/* Give each board a recognizable SoftAP name using the last three bytes of its MAC address. */
static esp_err_t mesh_softap_configure(void)
{
    uint8_t softap_mac[6] = {0};

    ESP_RETURN_ON_ERROR(
        esp_wifi_get_mac(WIFI_IF_AP, softap_mac),
        TAG,
        "Failed to read SoftAP MAC");

    /*
     * Limit the base name to 25 characters so the underscore and six hexadecimal
     * characters fit within the 32-byte SSID limit. snprintf writes into the buffer.
     */
    snprintf(
        s_softap_ssid,
        sizeof(s_softap_ssid),
        "%.25s_%02x%02x%02x",
        CONFIG_BRIDGE_SOFTAP_SSID,
        softap_mac[3],
        softap_mac[4],
        softap_mac[5]);

    return esp_mesh_lite_set_softap_info(
        s_softap_ssid, CONFIG_BRIDGE_SOFTAP_PASSWORD);
}

/*
 * Initialize the shared networking services, configure the selected role, and start Mesh-Lite.
 * ESP_RETURN_ON_ERROR logs and returns an error if a checked initialization step fails.
 */
esp_err_t mesh_network_init(void)
{
    ESP_RETURN_ON_ERROR(
        storage_init(), TAG, "Storage initialization failed");

    ESP_RETURN_ON_ERROR(
        esp_netif_init(), TAG, "Network interface initialization failed");

    /* The default event loop delivers Wi-Fi and IP events to registered handlers. */
    ESP_RETURN_ON_ERROR(
        esp_event_loop_create_default(), TAG, "Event loop creation failed");

    /* Create the network interfaces enabled in the IoT-Bridge configuration. */
    esp_bridge_create_all_netif();

    ESP_RETURN_ON_ERROR(
        mesh_wifi_configure(), TAG, "Wi-Fi configuration failed");

    esp_mesh_lite_config_t mesh_config = ESP_MESH_LITE_DEFAULT_INIT();

    /*
     * Allow mesh formation without requiring an existing router connection.
     * Non-root nodes can join without their own router credentials.
     * Keeping leaf_node false allows a node to serve as a relay for children.
     */
    mesh_config.join_mesh_ignore_router_status = true;
    mesh_config.join_mesh_without_configured_wifi = !IS_ROOT;
    mesh_config.leaf_node = false;

    esp_mesh_lite_init(&mesh_config);

    /*
     * Also supply the root's upstream credentials to Mesh-Lite itself.
     * The earlier Wi-Fi configuration step has already validated their lengths.
     * This API must be called after Mesh-Lite initialization and before startup.
     */
    if (IS_ROOT)
    {
        mesh_lite_sta_config_t router_config = {0};

        memcpy(
            router_config.ssid,
            CONFIG_ORATIO_ROUTER_SSID,
            strlen(CONFIG_ORATIO_ROUTER_SSID));

        memcpy(
            router_config.password,
            CONFIG_ORATIO_ROUTER_PASSWORD,
            strlen(CONFIG_ORATIO_ROUTER_PASSWORD));

        ESP_RETURN_ON_ERROR(
            esp_mesh_lite_set_router_config(&router_config),
            TAG,
            "Failed to configure Mesh-Lite router");
    }

    ESP_RETURN_ON_ERROR(
        mesh_softap_configure(), TAG, "Mesh SoftAP configuration failed");

    /* Enforce a fixed root rather than allowing non-root devices to become root. */
    if (IS_ROOT)
    {
        ESP_RETURN_ON_ERROR(
            esp_mesh_lite_set_allowed_level(ROOT_LEVEL),
            TAG,
            "Failed to restrict root to level 1");
    }
    else
    {
#if defined(APP_FORCE_LEVEL3) && (APP_FORCE_LEVEL3 == 1)
        /* Temporary test restriction: this board must join through a level-2 parent. */
        ESP_RETURN_ON_ERROR(
            esp_mesh_lite_set_allowed_level(3),
            TAG,
            "Failed to restrict test node to level 3");
#else
        /* Ordinary nodes may choose a suitable mesh level, but cannot become level 1. */
        ESP_RETURN_ON_ERROR(
            esp_mesh_lite_set_disallowed_level(ROOT_LEVEL),
            TAG,
            "Failed to prevent node from becoming root");
#endif
    }

    /* Connection and topology formation continue asynchronously after this call. */
    esp_mesh_lite_start();

    /* Successful initialization does not yet guarantee an IP address or server connectivity. */
    return ESP_OK;
}

/* Log this board's role and mesh level, plus parent information for non-root nodes. */
void mesh_network_log_status(void)
{
    const uint8_t level = esp_mesh_lite_get_level();

    ESP_LOGI(
        TAG,
        "Firmware role: %s | mesh level: %u | SoftAP: %s",
        IS_ROOT ? "root" : "node",
        (unsigned int)level,
        s_softap_ssid);

    /* Skip parent reporting for the root, even when it has an upstream hotspot connection. */
    if (IS_ROOT)
    {
        return;
    }

    wifi_ap_record_t parent_info = {0};

    /* A node without a current Wi-Fi parent has no parent details to report. */
    if (esp_wifi_sta_get_ap_info(&parent_info) != ESP_OK)
    {
        return;
    }

    /* BSSID identifies the parent's access point; RSSI reports received signal strength. */
    ESP_LOGI(
        TAG,
        "Parent BSSID: " MACSTR " | RSSI: %d dBm",
        MAC2STR(parent_info.bssid),
        parent_info.rssi);
}