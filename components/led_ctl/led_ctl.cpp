#include "led_ctl.hpp"
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "led_ctl";

LedStrip::LedStrip(gpio_num_t pin, uint32_t num_leds) : num_leds_(num_leds)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = static_cast<int>(pin),
        .max_leds = num_leds,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB, // WS2812 uses GRB order, not RGB
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz — required by the WS2812 timing spec
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &handle_));
    ESP_LOGI(TAG, "LED strip on GPIO%d, %lu LEDs", static_cast<int>(pin), num_leds);
}

LedStrip::~LedStrip()
{
    led_strip_del(handle_);
}

void LedStrip::set(uint32_t index, uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(handle_, index, r, g, b);
}

void LedStrip::clear()
{
    led_strip_clear(handle_);
}

void LedStrip::show()
{
    led_strip_refresh(handle_);
}
