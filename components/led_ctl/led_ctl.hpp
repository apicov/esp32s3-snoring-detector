#pragma once
#include <cstdint>
#include "driver/gpio.h"
#include "led_strip.h"

class LedStrip {
public:
    LedStrip(gpio_num_t pin, uint32_t num_leds);
    ~LedStrip();

    void set(uint32_t index, uint8_t r, uint8_t g, uint8_t b);
    void clear();
    void show();

private:
    led_strip_handle_t handle_;
    uint32_t num_leds_;
};
