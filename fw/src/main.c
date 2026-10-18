#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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

#ifndef APP_IS_ROOT
#error "APP_IS_ROOT must be supplied by the build environment"
#endif

#if (APP_IS_ROOT != 0) && (APP_IS_ROOT != 1)
#error "APP_IS_ROOT must be 0 or 1"
#endif

#if !defined(CONFIG_MESH_LITE_ENABLE)
#error "Enable Mesh-Lite in menuconfig"
#endif

#define SOFTAP_SSID_BUFFER_SIZE 33

static const char *TAG = "MESH_NETWORK";
static const bool IS_ROOT = (APP_IS_ROOT == 1);
static const uint8_t ROOT_LEVEL = 1;
static const uint8_t MESH_CHANNEL = 6;
static const uint32_t STATUS_INTERVAL_MS = 5000;

static char s_softap_ssid[SOFTAP_SSID_BUFFER_SIZE];

static esp_err_t storage_init(void)
{
  esp_err_t result = nvs_flash_init();

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

static esp_err_t mesh_wifi_configure(void)
{
  wifi_config_t station_config = {0};

  ESP_RETURN_ON_ERROR(
      esp_bridge_wifi_set_config(WIFI_IF_STA, &station_config),
      TAG,
      "Failed to configure station");

  wifi_config_t softap_config = {
      .ap = {
          .ssid = CONFIG_BRIDGE_SOFTAP_SSID,
          .password = CONFIG_BRIDGE_SOFTAP_PASSWORD,
          .channel = MESH_CHANNEL,
      },
  };

  return esp_bridge_wifi_set_config(WIFI_IF_AP, &softap_config);
}

static esp_err_t mesh_softap_configure(void)
{
  uint8_t softap_mac[6] = {0};

  ESP_RETURN_ON_ERROR(
      esp_wifi_get_mac(WIFI_IF_AP, softap_mac),
      TAG,
      "Failed to read SoftAP MAC");

  /*
   * A unique SSID helps identify each board.
   * snprintf formats a string; it does not print to the console.
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

static esp_err_t mesh_network_init(void)
{
  ESP_RETURN_ON_ERROR(
      storage_init(), TAG, "Storage initialization failed");

  ESP_RETURN_ON_ERROR(
      esp_netif_init(), TAG, "Network interface initialization failed");

  ESP_RETURN_ON_ERROR(
      esp_event_loop_create_default(), TAG, "Event loop creation failed");

  esp_bridge_create_all_netif();

  ESP_RETURN_ON_ERROR(
      mesh_wifi_configure(), TAG, "Wi-Fi configuration failed");

  esp_mesh_lite_config_t mesh_config = ESP_MESH_LITE_DEFAULT_INIT();

  mesh_config.join_mesh_ignore_router_status = true;
  mesh_config.join_mesh_without_configured_wifi = !IS_ROOT;
  mesh_config.leaf_node = false;

  esp_mesh_lite_init(&mesh_config);

  ESP_RETURN_ON_ERROR(
      mesh_softap_configure(), TAG, "Mesh SoftAP configuration failed");

  if (IS_ROOT)
  {
    ESP_RETURN_ON_ERROR(
        esp_mesh_lite_set_allowed_level(ROOT_LEVEL),
        TAG,
        "Failed to restrict root to level 1");
  }
  else
  {
    ESP_RETURN_ON_ERROR(
        esp_mesh_lite_set_disallowed_level(ROOT_LEVEL),
        TAG,
        "Failed to prevent node from becoming root");
  }

  esp_mesh_lite_start();

  return ESP_OK;
}

static void mesh_status_log(void)
{
  const uint8_t level = esp_mesh_lite_get_level();

  ESP_LOGI(
      TAG,
      "Firmware role: %s | mesh level: %u | SoftAP: %s",
      IS_ROOT ? "root" : "node",
      (unsigned int)level,
      s_softap_ssid);

  wifi_ap_record_t parent_info = {0};

  if (esp_wifi_sta_get_ap_info(&parent_info) != ESP_OK)
  {
    return;
  }

  ESP_LOGI(
      TAG,
      "Parent BSSID: " MACSTR " | RSSI: %d dBm",
      MAC2STR(parent_info.bssid),
      parent_info.rssi);
}

void app_main(void)
{
  ESP_LOGI(TAG, "Starting %s firmware", IS_ROOT ? "root" : "node");

  const esp_err_t result = mesh_network_init();

  if (result != ESP_OK)
  {
    ESP_LOGE(TAG, "Mesh startup failed: %s", esp_err_to_name(result));
    return;
  }

  ESP_LOGI(TAG, "Mesh startup requested; waiting for topology to form");

  while (true)
  {
    mesh_status_log();
    vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
  }
}