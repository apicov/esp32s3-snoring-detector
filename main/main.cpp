#include <cstdio>
#include <cstring>
#include <optional>
#include <memory>
#include <array>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "MQTTClient.hpp"
#include "led_ctl.hpp"
#include "esp_timer.h"
#include "driver/i2s_pdm.h"
#include "hal/gpio_types.h"

static const char *TAG = "snoring_detector";

#define WIFI_SSID      CONFIG_SNORING_WIFI_SSID
#define WIFI_PASSWORD  CONFIG_SNORING_WIFI_PASSWORD
#define MQTT_URI       CONFIG_SNORING_MQTT_BROKER_URI

static MQTTClient *mqtt = nullptr;

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
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

constexpr size_t AUDIO_CHUNK_SIZE = 32000;
using AudioBuffer = std::array<int16_t, AUDIO_CHUNK_SIZE>;

struct QueuedAudioChunk {
    AudioBuffer* buffer;
    int64_t timestamp_us;
};

class I2SMicrophone{
  public:
    class AudioChunkHandle{
      public:
        AudioChunkHandle(I2SMicrophone* owner, AudioBuffer* buffer, int64_t timestamp_us)
          :owner_(owner), buffer_(buffer), timestamp_us_(timestamp_us){
        }

        ~AudioChunkHandle(){
          if (owner_ && buffer_){
            owner_->return_buffer(buffer_);
            buffer_ = nullptr;
          }
        }

        //accessors
        AudioBuffer& buffer() {return *buffer_;}
        const AudioBuffer& buffer() const { return *buffer_; }
        int64_t timestamp_us() const {return timestamp_us_;}

        //copy constructors
        AudioChunkHandle(const AudioChunkHandle&) = delete;
        AudioChunkHandle& operator=(const AudioChunkHandle&) = delete;

        //move constructor
        AudioChunkHandle(AudioChunkHandle&& other) noexcept            // transfer buffer lease safely; source becomes empty
          :owner_(other.owner_), buffer_(other.buffer_), timestamp_us_(other.timestamp_us_) {
            other.buffer_ = nullptr;
            other.owner_ = nullptr;
        }
        
        //move assignment operator
        AudioChunkHandle& operator=(AudioChunkHandle&& other) noexcept{ // transfer lease on assignment; return old buffer first if needed
          if (this != &other) {
            if (buffer_) owner_->return_buffer(buffer_);
            owner_ = other.owner_;
            buffer_ = other.buffer_;
            timestamp_us_ = other.timestamp_us_;
            other.buffer_ = nullptr;
            other.owner_ = nullptr;
          }
          return *this;                                                          //
        }

      private:
        I2SMicrophone* owner_;
        AudioBuffer* buffer_;
        int64_t timestamp_us_;
    };

    I2SMicrophone(gpio_num_t clk, gpio_num_t data, uint32_t sample_rate):
      clk_(clk), data_(data), sample_rate_(sample_rate){
     
      audio_queue_ = xQueueCreate(POOL_SIZE, sizeof(QueuedAudioChunk));
      empty_audio_queue_ = xQueueCreate(POOL_SIZE, sizeof(QueuedAudioChunk));
      
      for (int i=0 ; i<POOL_SIZE ; i++){
        // Allocate new buffer safely on the heap
        auto buffer = std::make_unique<AudioBuffer>();
        // Release ownership to get the raw c-style pointer 
        QueuedAudioChunk chunk = { buffer.release(), 0 };
        // Push the raw pointer into the empty queue
        xQueueSend(empty_audio_queue_, &chunk, 0);
      }

      //configure the I2S channel
      chan_cfg_ = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
      // configure DMA
      chan_cfg_.dma_desc_num = 4; //number of chunks
      chan_cfg_.dma_frame_num = 1000; //samples per chunk 
      // nullptr for the TX handle. Only want to record (RX)
      i2s_new_channel(&chan_cfg_, nullptr, &rx_handle_);
      
       pdm_cfg_ = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sample_rate_), //  sample rate
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = clk_,
            .din = data_, 
            .invert_flags = { .clk_inv = false }
        },
      };
      i2s_channel_init_pdm_rx_mode(rx_handle_, &pdm_cfg_);

      //configure interruption
      i2s_event_callbacks_t cbs = {
        .on_recv = I2SMicrophone::on_receive_callback,
        .on_recv_q_ovf = nullptr,
        .on_sent = nullptr,
        .on_send_q_ovf = nullptr
      };
      i2s_channel_register_event_callback(rx_handle_, &cbs, this);

      QueuedAudioChunk first_chunk{nullptr, 0};
      xQueueReceive(empty_audio_queue_, &first_chunk, 0);
      current_buffer_.reset(first_chunk.buffer);
    }

    // Static callback required by the ESP-IDF C API
    static bool IRAM_ATTR on_receive_callback(i2s_chan_handle_t handle, 
                                              i2s_event_data_t *event, 
                                              void *user_ctx){
      // Get back to C++ object via the user context pointer
      auto* self = static_cast<I2SMicrophone*>(user_ctx);
      BaseType_t higher_priority_task_woken = pdFALSE;

      // Compute destination as a byte pointer to do offset math in bytes
      auto* dst = reinterpret_cast<uint8_t*>(self->current_buffer_->data())
                + self->samples_collected_ * sizeof(int16_t);

      //Copy DMA chunk into our big buffer
      std::memcpy(dst, event->dma_buf, event->size);
      self->samples_collected_ += event->size / sizeof(int16_t);

      // If the big buffer is full, hand it off and grab a fresh empty one
      if (self->samples_collected_ >= AUDIO_CHUNK_SIZE) {
        QueuedAudioChunk full_chunk{self->current_buffer_.release(), self->current_timestamp_};
        xQueueSendFromISR(self->audio_queue_, &full_chunk, &higher_priority_task_woken);

        //pull a fresh empty buffer to keep recording 
        QueuedAudioChunk next_chunk{nullptr, 0};
        if(xQueueReceiveFromISR(self->empty_audio_queue_, &next_chunk, &higher_priority_task_woken)
            != pdTRUE){
              // No empty buffer available, reuse the oldest full one
              xQueueReceiveFromISR(self->audio_queue_, &next_chunk, &higher_priority_task_woken);
        }
        self->current_timestamp_ = esp_timer_get_time();
        self->current_buffer_.reset(next_chunk.buffer);
        self->samples_collected_ = 0;
      }

      // Return true to trigger an immediate context switch if a higher-priority task woke up
      return higher_priority_task_woken == pdTRUE;
    }

    std::optional<AudioChunkHandle> get_audio(){
      QueuedAudioChunk chunk{nullptr, 0};
      if (xQueueReceive(audio_queue_, &chunk, pdMS_TO_TICKS(3000)) == pdTRUE)
        return AudioChunkHandle(this, chunk.buffer, chunk.timestamp_us);
      return std::nullopt;
    }

    void start(){
      if(!is_recording_flag_){
        reset_buffers();
        current_timestamp_ = esp_timer_get_time();
        i2s_channel_enable(rx_handle_);
        is_recording_flag_ = true;
      }
    }
      
    void stop(){
      if(is_recording_flag_){
        i2s_channel_disable(rx_handle_);
        is_recording_flag_ = false;
      }
    }

    void return_buffer(AudioBuffer* buffer){
      QueuedAudioChunk chunk{buffer, 0};
      xQueueSend(empty_audio_queue_, &chunk, 0);
    }

    ~I2SMicrophone(){
      if (is_recording_flag_) i2s_channel_disable(rx_handle_);
      if (rx_handle_) i2s_del_channel(rx_handle_);

      QueuedAudioChunk chunk{nullptr, 0};
      while (xQueueReceive(audio_queue_, &chunk, 0) == pdTRUE) delete chunk.buffer;
      while (xQueueReceive(empty_audio_queue_, &chunk, 0) == pdTRUE) delete chunk.buffer;
    }

    I2SMicrophone& operator=(const I2SMicrophone&) = delete;
    I2SMicrophone(const I2SMicrophone&) = delete;

    private:
      gpio_num_t clk_;
      gpio_num_t data_;
      uint32_t sample_rate_;
      i2s_chan_handle_t rx_handle_ = nullptr;
      i2s_chan_config_t chan_cfg_;
      i2s_pdm_rx_config_t pdm_cfg_;
      bool is_recording_flag_ = false;
      QueueHandle_t audio_queue_ = nullptr;
      QueueHandle_t empty_audio_queue_ = nullptr;
      std::unique_ptr<AudioBuffer> current_buffer_;
      int64_t current_timestamp_ = 0;
      size_t samples_collected_ = 0;
      static constexpr size_t POOL_SIZE = 3;


      void reset_buffers(){
        std::array<AudioBuffer*, POOL_SIZE> buffers{};
        size_t count = 0;
        if (current_buffer_) buffers[count++] = current_buffer_.release();
        // empty queues and save buffers pointers in array
        QueuedAudioChunk chunk{nullptr, 0};
        while (xQueueReceive(audio_queue_, &chunk, 0) == pdTRUE)
          buffers[count++] = chunk.buffer;
        while (xQueueReceive(empty_audio_queue_, &chunk, 0) == pdTRUE) 
          buffers[count++] = chunk.buffer;
        //send all buffers to empty_audio_queue_
        for (size_t i = 0; i < count; i++) {
          QueuedAudioChunk empty{buffers[i], 0};
          xQueueSend(empty_audio_queue_, &empty, 0);
        }
        // take one buffer from queue and use it as current
        xQueueReceive(empty_audio_queue_, &chunk, 0);
        current_buffer_.reset(chunk.buffer);
        samples_collected_ = 0;
        current_timestamp_ = 0; 
      }
};



extern "C" void app_main(void)
{
    LedStrip leds{GPIO_NUM_1, 8};
    leds.set(0, 0, 0, 32);  // dim blue while booting
    leds.set(1, 0, 0, 32);  // dim blue while booting
    leds.set(2, 0, 0, 32);  // dim blue while booting
    leds.show();


    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /*
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
    ESP_LOGI(TAG, "WiFi started, connecting...");

    if (esp_psram_is_initialized()) {
        ESP_LOGI(TAG, "PSRAM: %d bytes", esp_psram_get_size());
    } else {
        ESP_LOGE(TAG, "PSRAM not initialized");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(5000));

    mqtt = new MQTTClient(MQTT_URI);

    mqtt->on_connect([](auto _) {
        ESP_LOGI(TAG, "MQTT connected");
    });

    for (int i = 0; i < 30 && !mqtt->is_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (mqtt->is_connected()) {
        ESP_LOGI(TAG, "MQTT ready");
        leds.set(0, 0, 32, 0);  // dim green = ready
        leds.show();
    }
*/
    ESP_LOGI(TAG, "System ready");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
