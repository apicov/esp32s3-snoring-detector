idf_component_register(
    SRCS "i2s_microphone.cpp"
    INCLUDE_DIRS "."
    REQUIRES "esp_driver_i2s" "esp_timer" "freertos"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20 -mtext-section-literals)
