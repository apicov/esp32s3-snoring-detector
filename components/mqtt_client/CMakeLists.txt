idf_component_register(SRCS "MQTTClient.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES "lwmqtt-esp-mqtt" "esp_event")
