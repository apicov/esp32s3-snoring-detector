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
#include "camera_ctl.hpp"
#include "img_converters.h"
#include "image_utils.h"
#include "mbedtls/base64.h"

static const char *TAG = "cam_mqtt";

#define WIFI_SSID CONFIG_CAM_MQTT_WIFI_SSID
#define WIFI_PASSWORD CONFIG_CAM_MQTT_WIFI_PASSWORD
#define MQTT_URI CONFIG_CAM_MQTT_BROKER_URI
#define MQTT_TOPIC CONFIG_CAM_MQTT_IMAGE_TOPIC
#define MQTT_CMD_TOPIC CONFIG_CAM_MQTT_CMD_TOPIC

static MQTTClient *mqtt = nullptr;
static QueueHandle_t camera_cmd_queue = nullptr;

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
        ESP_LOGI(TAG, "✓ Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "PSRAM Test Starting...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize WiFi
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
    ESP_LOGI(TAG, "✓ WiFi started, connecting...");

    // Check if PSRAM is available
    if (esp_psram_is_initialized()) {
        ESP_LOGI(TAG, "✓ PSRAM initialized");
        ESP_LOGI(TAG, "PSRAM size: %d bytes", esp_psram_get_size());
    } else {
        ESP_LOGE(TAG, "✗ PSRAM not initialized");
        return;
    }

    // Show heap stats
    ESP_LOGI(TAG, "Free internal RAM: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM: %lu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Test 1: Allocate buffer in PSRAM using heap_caps_malloc
    size_t test_size = 100000;  // 100KB
    uint8_t *psram_buf = (uint8_t*)heap_caps_malloc(test_size, MALLOC_CAP_SPIRAM);

    if (psram_buf) {
        ESP_LOGI(TAG, "✓ Allocated %d bytes at %p", test_size, psram_buf);

        // Write pattern
        for (int i = 0; i < test_size; i++) {
            psram_buf[i] = i & 0xFF;
        }

        // Verify pattern
        bool ok = true;
        for (int i = 0; i < test_size; i++) {
            if (psram_buf[i] != (i & 0xFF)) {
                ok = false;
                break;
            }
        }

        if (ok) {
            ESP_LOGI(TAG, "✓ Read/write verification passed");
        } else {
            ESP_LOGE(TAG, "✗ Read/write verification FAILED");
        }

        heap_caps_free(psram_buf);
    } else {
        ESP_LOGE(TAG, "✗ Failed to allocate PSRAM buffer");
    }

    ESP_LOGI(TAG, "PSRAM test passed. Initializing camera...");

    // Create command queue
    camera_cmd_queue = xQueueCreate(10, sizeof(uint8_t));

    // Initialize camera
    CameraCtl cam{};
    ESP_LOGI(TAG, "✓ Camera initialized");

    ESP_LOGI(TAG, "Starting MQTT...");

    // Wait for WiFi connection
    vTaskDelay(pdMS_TO_TICKS(5000));

    // Initialize lwmqtt (MQTTClient auto-starts on construction)
    mqtt = new MQTTClient(MQTT_URI);

    // Subscribe to command topic
    mqtt->on_connect([](auto _) {
        mqtt->subscribe(MQTT_CMD_TOPIC, 0);
        ESP_LOGI(TAG, "Subscribed to %s", MQTT_CMD_TOPIC);
    });

    // Handle incoming commands
    mqtt->on_data_received([](auto data) {
        ESP_LOGI(TAG, "Received on topic: %.*s", (int)data->topic_len, data->topic);
        ESP_LOGI(TAG, "Command: '%.*s'", (int)data->data_len, (const char*)data->data);

        if (strncmp(data->topic, MQTT_CMD_TOPIC, data->topic_len) == 0) {
            if (strncmp((const char*)data->data, "snap", data->data_len) == 0) {
                uint8_t cmd = 1;
                xQueueSend(camera_cmd_queue, &cmd, 0);
                ESP_LOGI(TAG, "Snapshot triggered via MQTT");
            }
        }
    });

    ESP_LOGI(TAG, "Waiting for MQTT connection...");
    for (int i = 0; i < 30 && !mqtt->is_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (mqtt->is_connected()) {
        ESP_LOGI(TAG, "✓ MQTT connected");
    }

    ESP_LOGI(TAG, "System ready. Waiting for 'snap' command on %s...", MQTT_CMD_TOPIC);

    while(1) {
        // Wait for command from MQTT
        uint8_t cmd;
        if (xQueueReceive(camera_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            if (mqtt && mqtt->is_connected()) {
                ESP_LOGI(TAG, "Capturing and sending image...");

            // Show heap before capture
            ESP_LOGI(TAG, "Before capture - Free DRAM: %lu, Free PSRAM: %lu",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

            cam.capture_do([](const auto &pic){
                auto jpeg_data = pic.image();
                auto jpeg_len = pic.size();

                ESP_LOGI(TAG, "Captured JPEG image: %zu bytes", jpeg_len);

                // Base64 encode the original JPEG for MQTT transmission
                size_t jpeg_b64_size = (4 * ((jpeg_len + 2) / 3)) + 1;
                char* jpeg_b64_buffer = (char*)heap_caps_malloc(jpeg_b64_size, MALLOC_CAP_SPIRAM);

                if (!jpeg_b64_buffer) {
                    ESP_LOGE(TAG, "Failed to allocate base64 buffer for JPEG");
                    return;
                }

                size_t olen;
                auto ret = mbedtls_base64_encode(
                    (unsigned char*)jpeg_b64_buffer, jpeg_b64_size, &olen, jpeg_data, jpeg_len);

                if (ret == 0) {
                    ESP_LOGI(TAG, "✓ Base64 encoded (%zu bytes)", olen);

                    // Publish base64 encoded JPEG via MQTT
                    if (mqtt && mqtt->is_connected()) {
                        mqtt->publish(MQTT_TOPIC, (const uint8_t*)jpeg_b64_buffer, olen, 0, 0);
                        ESP_LOGI(TAG, "✓ Sent original JPEG (%zu bytes base64)", olen);
                    }
                } else {
                    ESP_LOGE(TAG, "Base64 encoding failed");
                }

                heap_caps_free(jpeg_b64_buffer);

                // Allocate buffers for local processing (96x96 resize)
                constexpr size_t decoded_size = 160 * 120 * 3;  // RGB888: 57,600 bytes
                constexpr size_t resized_size = 96 * 96 * 3;    // RGB888: 27,648 bytes

                uint8_t* decoded_buf = (uint8_t*)heap_caps_malloc(decoded_size, MALLOC_CAP_SPIRAM);
                uint8_t* resized_buf = (uint8_t*)heap_caps_malloc(resized_size, MALLOC_CAP_SPIRAM);

                if (!decoded_buf || !resized_buf) {
                    ESP_LOGE(TAG, "Failed to allocate processing buffers in PSRAM");
                    heap_caps_free(decoded_buf);
                    heap_caps_free(resized_buf);
                    return;
                }

                // Convert JPEG to RGB888 (160x120) for local processing
                if (!fmt2rgb888(jpeg_data, jpeg_len, PIXFORMAT_JPEG, decoded_buf)) {
                    ESP_LOGE(TAG, "JPEG to RGB888 conversion failed");
                    heap_caps_free(decoded_buf);
                    heap_caps_free(resized_buf);
                    return;
                }
                ESP_LOGI(TAG, "✓ Decoded JPEG to BGR888 (160x120)");

                // Resize from 160x120 to 96x96 for local processing (resizeColorImage already swaps BGR to RGB)
                resizeColorImage(decoded_buf, 160, 120, resized_buf, 96, 96);
                ESP_LOGI(TAG, "✓ Resized to 96x96 and converted to RGB for local processing");

                // Free PSRAM buffers
                heap_caps_free(decoded_buf);
                heap_caps_free(resized_buf);

                ESP_LOGI(TAG, "After processing - Free PSRAM: %lu",
                         heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            });
            } else {
                ESP_LOGE(TAG, "MQTT not connected, cannot send image");
            }
        }
    }
}
