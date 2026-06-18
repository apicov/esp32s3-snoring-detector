#include "MQTTClient.hpp"
#include "esp_mqtt.h" // lwmqtt library
#include <cstring>

static const char* TAG = "mqtt_client";
static MQTTClient* g_mqtt_instance = nullptr;

// lwmqtt callbacks
static void status_callback(esp_mqtt_status_t status) {
    if (!g_mqtt_instance) return;

    if (status == ESP_MQTT_STATUS_CONNECTED) {
        ESP_LOGI(TAG, "lwmqtt: Connected to broker");
        g_mqtt_instance->handle_connect();
    } else {
        ESP_LOGI(TAG, "lwmqtt: Disconnected from broker");
        g_mqtt_instance->handle_disconnect();
    }
}

static void message_callback(const char *topic, const uint8_t *payload, size_t len, int qos, bool retained) {
    if (!g_mqtt_instance) return;
    g_mqtt_instance->handle_message(topic, payload, len);
}

MQTTClient::MQTTClient(const char* mqtt_broker_uri)
    :mqtt_broker_uri_(mqtt_broker_uri) {

    ESP_LOGI(TAG, "Initializing lwmqtt with %s", mqtt_broker_uri_);

    // Parse URI: mqtt://host:port
    char host[128] = {0};
    int port = 1883;

    if (sscanf(mqtt_broker_uri_, "mqtt://%127[^:]:%d", host, &port) != 2) {
        if (sscanf(mqtt_broker_uri_, "mqtt://%127s", host) == 1) {
            port = 1883; // default
        } else {
            ESP_LOGE(TAG, "Invalid MQTT URI format");
            return;
        }
    }

    ESP_LOGI(TAG, "Parsed: host=%s, port=%d", host, port);

    // Save global instance for callbacks
    g_mqtt_instance = this;

    // Initialize lwmqtt (buffer_size=2048, command_timeout=10s, core=0 for unicore)
    esp_mqtt_init(status_callback, message_callback, 2048, 10000, 0);

    // Start connection
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (!esp_mqtt_start(host, port_str, "esp32_camera", nullptr, nullptr)) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
        return;
    }

    ESP_LOGI(TAG, "lwmqtt client started");
}

void MQTTClient::handle_connect() {
    is_connected_.store(true);
    ESP_LOGI(TAG, "Sending a test message");
    publish("/topic/test", "Hello from ESP32!", 1, 0);

    for (const auto& f: on_connect_cb) {
        f(nullptr);
    }
}

void MQTTClient::handle_disconnect() {
    is_connected_.store(false);
    for (const auto& f: on_disconnect_cb) {
        f(nullptr);
    }
}

void MQTTClient::handle_message(const char* topic, const uint8_t* payload, size_t len) {
    ESP_LOGI(TAG, "Received message on topic: %s (len=%zu)", topic, len);

    // Create message structure for callbacks
    MQTTMessage msg;
    msg.topic = topic;
    msg.data = payload;
    msg.topic_len = strlen(topic);
    msg.data_len = len;

    for (const auto& f: on_data_received_cb) {
        f(&msg);
    }
}

MQTTClient& MQTTClient::on_connect(MQTTEventCallback f) {
    on_connect_cb.push_back(f);
    return *this;
}

MQTTClient& MQTTClient::on_disconnect(MQTTEventCallback f) {
    on_disconnect_cb.push_back(f);
    return *this;
}

MQTTClient& MQTTClient::on_data_received(MQTTEventCallback f){
    on_data_received_cb.push_back(f);
    return *this;
}

bool MQTTClient::is_connected() const {
    return is_connected_.load();
}

void MQTTClient::publish(const char* topic, const char* data, int qos, int retain) {
    if (!is_connected()) {
        ESP_LOGW(TAG, "MQTT client is not connected. Cannot publish message.");
        return;
    }

    if (esp_mqtt_publish(topic, (const uint8_t*)data, strlen(data), qos, retain)) {
        ESP_LOGI(TAG, "Message published to %s", topic);
    } else {
        ESP_LOGE(TAG, "Failed to publish message");
    }
}

void MQTTClient::publish(const char* topic, const uint8_t* data, size_t len, int qos, int retain) {
    if (!is_connected()) {
        ESP_LOGW(TAG, "MQTT client is not connected. Cannot publish message.");
        return;
    }

    if (esp_mqtt_publish(topic, data, len, qos, retain)) {
        ESP_LOGI(TAG, "Message published to %s (%zu bytes)", topic, len);
    } else {
        ESP_LOGE(TAG, "Failed to publish message");
    }
}

esp_err_t MQTTClient::subscribe(const char* topic, int qos){
    if (esp_mqtt_subscribe(topic, qos)) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t MQTTClient::reconnect() {
    // lwmqtt handles reconnection automatically
    return ESP_OK;
}
