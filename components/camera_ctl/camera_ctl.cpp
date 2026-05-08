// ESP
#include <esp_log.h>
#include <esp_check.h>

// ESP-IDF
#include <driver/gpio.h>
#include <driver/i2c.h>

#include "camera_ctl.hpp"

// XIAO ESP32S3 Sense pin configuration
#define CAM_PIN_PWDN    -1 // Not used
#define CAM_PIN_RESET   -1 // Software reset will be performed
#define CAM_PIN_XCLK    10
#define CAM_PIN_SIOD    40
#define CAM_PIN_SIOC    39

#define CAM_PIN_D7      48
#define CAM_PIN_D6      11
#define CAM_PIN_D5      12
#define CAM_PIN_D4      14
#define CAM_PIN_D3      16
#define CAM_PIN_D2      18
#define CAM_PIN_D1      17
#define CAM_PIN_D0      15
#define CAM_PIN_VSYNC   38
#define CAM_PIN_HREF    47
#define CAM_PIN_PCLK    13
#define CAM_FLASH_LAMP  -1  // XIAO ESP32S3 Sense doesn't have built-in flash

#define CONFIG_XCLK_FREQ 20'000'000
#define CONFIG_OV2640_SUPPORT 1
#define CONFIG_OV7725_SUPPORT 1
#define CONFIG_OV3660_SUPPORT 1
#define CONFIG_OV5640_SUPPORT 1

CameraCtl::CameraCtl()
{
    camera_config_t config = {
      .pin_pwdn = CAM_PIN_PWDN,
      .pin_reset = CAM_PIN_RESET,
      .pin_xclk = CAM_PIN_XCLK,
      .pin_sccb_sda = CAM_PIN_SIOD,
      .pin_sccb_scl = CAM_PIN_SIOC,
      .pin_d7 = CAM_PIN_D7,
      .pin_d6 = CAM_PIN_D6,
      .pin_d5 = CAM_PIN_D5,
      .pin_d4 = CAM_PIN_D4,
      .pin_d3 = CAM_PIN_D3,
      .pin_d2 = CAM_PIN_D2,
      .pin_d1 = CAM_PIN_D1,
      .pin_d0 = CAM_PIN_D0,
      .pin_vsync = CAM_PIN_VSYNC,
      .pin_href  = CAM_PIN_HREF,
      .pin_pclk  = CAM_PIN_PCLK,

      .xclk_freq_hz = CONFIG_XCLK_FREQ,
      .ledc_timer = LEDC_TIMER_0,
      .ledc_channel = LEDC_CHANNEL_0,

      .pixel_format = PIXFORMAT_JPEG,
      .frame_size = FRAMESIZE_QQVGA,

      .jpeg_quality = 20,
      .fb_count = 1,
      .fb_location = CAMERA_FB_IN_DRAM,  // Small JPEG in internal RAM (~4KB)
      .grab_mode = CAMERA_GRAB_LATEST,   // Only grab when requested (saves power)
      .sccb_i2c_port = I2C_NUM_0
    };

    /* XXX: for now abort if the camera couldn't be initialized,
     * but maybe is best to allow the user to do it instead
     */
    ESP_ERROR_CHECK(esp_camera_init(&config));
    // No flash lamp on XIAO ESP32S3 Sense
    ESP_LOGI(TAG, "Camera initialized, warming up...");

    // Discard first few frames to let camera stabilize
    for(int i = 0; i < 3; i++) {
        camera_fb_t* fb = esp_camera_fb_get();
        if(fb) {
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "Camera ready");
}


void CameraCtl::capture_do(std::function<void(const Picture &)> f)
{
    // Flash lamp not available on XIAO ESP32S3 Sense
    Picture p{};
    return f(p);
}


esp_err_t CameraCtl::camera_xclk_init(uint32_t freq_hz) {

    // Configure the LEDC timer
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,   // ESP32-S3 only has low-speed mode
        .duty_resolution = LEDC_TIMER_1_BIT, // Minimal duty resolution for clock
        .timer_num = LEDC_TIMER_0,           // Use LEDC_TIMER_0
        .freq_hz = freq_hz,                  // Set the desired frequency
        .clk_cfg = LEDC_AUTO_CLK,            // Automatically select clock source
        .deconfigure = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&ledc_timer), TAG, "ledc_timer");

    // Configure the LEDC channel for XCLK pin
    ledc_channel_config_t ledc_channel = {};
    ledc_channel.gpio_num = CAM_PIN_XCLK;
    ledc_channel.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_channel.channel = LEDC_CHANNEL_0;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.timer_sel = LEDC_TIMER_0;
    ledc_channel.duty = 1;
    ledc_channel.hpoint = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ledc_channel), TAG, "ledc_channel");

    return ESP_OK;
}

/* CameraCtl::Picture */
/* ================== */
CameraCtl::Picture::Picture()
{
    // With fb_count=1, discard old frame and grab fresh one
    camera_fb_t* old_fb = esp_camera_fb_get();
    if (old_fb) {
        esp_camera_fb_return(old_fb);
    }

    // Grab fresh frame
    fb = esp_camera_fb_get();
    ESP_LOGI(TAG, "Snapshot taken");
}

CameraCtl::Picture::~Picture()
{
    ESP_LOGD(TAG, "Release the snapshot's framebuffer");
    esp_camera_fb_return(fb);
}

const uint8_t *CameraCtl::Picture::image() const
{
    return fb->buf;
}

size_t CameraCtl::Picture::size() const
{
    return fb->len;
}
