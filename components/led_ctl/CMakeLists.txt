idf_component_register(
    SRCS "led_ctl.cpp"
    INCLUDE_DIRS "."
    REQUIRES "led_strip" "esp_driver_gpio"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
