idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS ""
                    REQUIRES "mqtt_client" "esp_psram" "esp_wifi" "esp_netif" "nvs_flash" "led_ctl" "i2s_microphone")

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20
    # Places literal pools inline with code so IRAM_ATTR functions can reach them via L32R
    -mtext-section-literals)
