#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "MQTTClient.hpp"
#include "led_ctl.hpp"

static const char *TAG = "snoring_detector";

#define WIFI_SSID      CONFIG_SNORING_WIFI_SSID
#define WIFI_PASSWORD  CONFIG_SNORING_WIFI_PASSWORD
#define MQTT_URI       CONFIG_SNORING_MQTT_BROKER_URI

static MQTTClient *mqtt = nullptr;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

extern "C" void app_main(void)
{
    LedStrip leds{GPIO_NUM_1, 8};
    leds.set(0, 0, 0, 32);  // dim blue while booting
    leds.show();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initializing WiFi...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_config = {};
    memcpy(wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi started, connecting...");

    if (esp_psram_is_initialized()) {
        ESP_LOGI(TAG, "PSRAM: %d bytes", esp_psram_get_size());
    } else {
        ESP_LOGE(TAG, "PSRAM not initialized");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(5000));

    mqtt = new MQTTClient(MQTT_URI);

    mqtt->on_connect([](auto _) {
        ESP_LOGI(TAG, "MQTT connected");
    });

    for (int i = 0; i < 30 && !mqtt->is_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (mqtt->is_connected()) {
        ESP_LOGI(TAG, "MQTT ready");
        leds.set(0, 0, 32, 0);  // dim green = ready
        leds.show();
    }

    ESP_LOGI(TAG, "System ready");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
