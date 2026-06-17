#pragma once
#include <array>
#include <atomic>
#include <optional>
#include <cstddef>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/i2s_pdm.h"
#include "hal/gpio_types.h"

constexpr size_t AUDIO_CHUNK_SIZE = 32000;
using AudioBuffer = std::array<int16_t, AUDIO_CHUNK_SIZE>;

class I2SMicrophone {
public:
    class AudioChunkHandle {
    public:
        AudioChunkHandle(I2SMicrophone* owner, AudioBuffer* buffer, int64_t timestamp_us)
            : owner_(owner), buffer_(buffer), timestamp_us_(timestamp_us) {}

        ~AudioChunkHandle() {
            if (owner_ && buffer_) {
                owner_->return_buffer(buffer_);
                buffer_ = nullptr;
            }
        }

        AudioBuffer& buffer() { return *buffer_; }
        const AudioBuffer& buffer() const { return *buffer_; }
        int64_t timestamp_us() const { return timestamp_us_; }

        AudioChunkHandle(const AudioChunkHandle&) = delete;
        AudioChunkHandle& operator=(const AudioChunkHandle&) = delete;

        AudioChunkHandle(AudioChunkHandle&& other) noexcept
            : owner_(other.owner_), buffer_(other.buffer_), timestamp_us_(other.timestamp_us_) {
            other.buffer_ = nullptr;
            other.owner_ = nullptr;
        }

        AudioChunkHandle& operator=(AudioChunkHandle&& other) noexcept {
            if (this != &other) {
                if (buffer_) owner_->return_buffer(buffer_);
                owner_ = other.owner_;
                buffer_ = other.buffer_;
                timestamp_us_ = other.timestamp_us_;
                other.buffer_ = nullptr;
                other.owner_ = nullptr;
            }
            return *this;
        }

    private:
        I2SMicrophone* owner_;
        AudioBuffer* buffer_;
        int64_t timestamp_us_;
    };

    I2SMicrophone(gpio_num_t clk, gpio_num_t data, uint32_t sample_rate);
    ~I2SMicrophone();

    I2SMicrophone(const I2SMicrophone&) = delete;
    I2SMicrophone& operator=(const I2SMicrophone&) = delete;

    std::optional<AudioChunkHandle> get_audio();
    void start();
    void stop();
    void return_buffer(AudioBuffer* buffer);
    uint32_t overwritten_chunk_count() const;
    void reset_overwritten_chunk_count();

private:
    static bool IRAM_ATTR on_receive_callback(i2s_chan_handle_t handle,
                                               i2s_event_data_t* event,
                                               void* user_ctx);
    void reset_buffers();

    gpio_num_t clk_;
    gpio_num_t data_;
    uint32_t sample_rate_;
    i2s_chan_handle_t rx_handle_ = nullptr;
    i2s_chan_config_t chan_cfg_;
    i2s_pdm_rx_config_t pdm_cfg_;
    bool is_recording_flag_{false};
    QueueHandle_t audio_queue_ = nullptr;
    QueueHandle_t empty_audio_queue_ = nullptr;
    AudioBuffer* current_buffer_ = nullptr;
    int64_t current_timestamp_{0};
    size_t samples_collected_{0};
    static constexpr size_t POOL_SIZE{3};
    std::atomic<uint32_t> overwritten_chunk_count_{0};
};
