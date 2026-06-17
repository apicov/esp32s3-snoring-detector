#pragma once
#include <cstdint>
#include "driver/gpio.h"
#include "led_strip.h"

/**
 * @brief Thin wrapper around the ESP-IDF led_strip driver for WS2812 LEDs.
 *
 * Drives an addressable RGB LED strip over RMT. Pixel colours are staged in
 * RAM and only sent to the strip when @c show() is called.
 */
class LedStrip {
public:
    /**
     * @brief Initialise the RMT peripheral and LED strip.
     * @param pin      GPIO driving the data line of the strip.
     * @param num_leds Number of LEDs in the strip.
     */
    LedStrip(gpio_num_t pin, uint32_t num_leds);
    ~LedStrip();

    /**
     * @brief Stage a pixel colour (does not update the strip until @c show()).
     * @param index Zero-based LED index.
     * @param r,g,b Colour components (0–255).
     */
    void set(uint32_t index, uint8_t r, uint8_t g, uint8_t b);

    /// Set all pixels to off (does not update the strip until @c show()).
    void clear();

    /// Transmit staged pixel data to the physical strip via RMT.
    void show();

private:
    led_strip_handle_t handle_;
    uint32_t num_leds_;
};
