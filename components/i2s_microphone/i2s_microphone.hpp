#pragma once
#include <array>
#include <atomic>
#include <optional>
#include <cstddef>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2s_pdm.h"
#include "hal/gpio_types.h"

/// Number of int16_t samples in one audio chunk (~2 s at 16 kHz).
constexpr size_t AUDIO_CHUNK_SIZE = 32000;

/// Fixed-size buffer holding one complete audio chunk.
using AudioBuffer = std::array<int16_t, AUDIO_CHUNK_SIZE>;

/**
 * @brief PDM microphone driver using the ESP-IDF I2S peripheral.
 *
 * Continuously fills a pool of @c AudioBuffer objects via DMA interrupts.
 * When a buffer is full it is pushed onto an internal queue; the caller
 * consumes it through @c get_audio(), which returns an @c AudioChunkHandle
 * that automatically returns the buffer to the pool on destruction.
 *
 * ## Modes
 *
 * **Continuous** (@c inter_chunk_pause_ms = 0, default):
 * The ISR fills buffers back-to-back with no gaps. The caller must consume
 * chunks fast enough or they will be overwritten.
 *
 * **Pause** (@c inter_chunk_pause_ms > 0):
 * An internal task stops the channel after each chunk, waits the configured
 * pause, then restarts. The pause gives the caller time to publish or process
 * without risking buffer overruns. A 300 ms pause is enough to cover a
 * ~2.2 s MQTT publish of a 2 s chunk over a typical WiFi/internet link,
 * yielding ~87 % duty cycle.
 *
 * Thread safety: @c start(), @c stop(), @c get_audio() and the overwritten-
 * count accessors are safe to call from a single task. The ISR callback runs
 * in interrupt context and only touches ISR-safe FreeRTOS APIs.
 */
class I2SMicrophone {
public:

    /**
     * @brief RAII handle for one captured audio chunk.
     *
     * Holds a pointer to a borrowed @c AudioBuffer. When the handle goes out
     * of scope (or is explicitly destroyed) the buffer is returned to the
     * microphone's pool, making it available for the next DMA fill cycle.
     *
     * Move-only: copying is disabled to prevent double-return of the buffer.
     */
    class AudioChunkHandle {
    public:
        /// @param owner  The microphone that owns the buffer pool.
        /// @param buffer Borrowed buffer — must not be deleted by the caller.
        /// @param timestamp_us  esp_timer value at the start of this chunk.
        AudioChunkHandle(I2SMicrophone* owner, AudioBuffer* buffer, int64_t timestamp_us)
            : owner_(owner), buffer_(buffer), timestamp_us_(timestamp_us) {}

        /// Returns the buffer to the pool.
        ~AudioChunkHandle() {
            if (owner_ && buffer_) {
                owner_->return_buffer(buffer_);
                buffer_ = nullptr;
            }
        }

        AudioBuffer& buffer() { return *buffer_; }
        const AudioBuffer& buffer() const { return *buffer_; }

        /// Timestamp (µs, from esp_timer_get_time) of the first sample.
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

    /**
     * @brief Initialise the I2S PDM channel and buffer pool.
     *
     * Does not start recording; call @c start() to begin.
     *
     * @param clk                  GPIO for the PDM clock output.
     * @param data                 GPIO for the PDM data input.
     * @param sample_rate          Desired sample rate in Hz (e.g. 16000).
     * @param inter_chunk_pause_ms Pause in ms between chunks (0 = continuous).
     *                             A value of 300 ms gives the consumer ~2.3 s
     *                             to process each 2 s chunk before the next
     *                             one starts, preventing buffer overruns.
     */
    I2SMicrophone(gpio_num_t clk, gpio_num_t data, uint32_t sample_rate,
                  uint32_t inter_chunk_pause_ms = 0);

    /// Stops recording (if active) and releases all I2S and buffer resources.
    ~I2SMicrophone();

    I2SMicrophone(const I2SMicrophone&) = delete;
    I2SMicrophone& operator=(const I2SMicrophone&) = delete;

    /**
     * @brief Block until a full audio chunk is available, then return it.
     *
     * Times out after 3 seconds and returns @c std::nullopt if no chunk
     * arrives (e.g. microphone not started or DMA stalled).
     */
    std::optional<AudioChunkHandle> get_audio();

    /// Start recording. In pause mode, kicks off the internal record/pause cycle.
    /// Idempotent if already running.
    void start();

    /// Stop recording. Idempotent if already stopped.
    void stop();

    /**
     * @brief Return a buffer to the empty pool.
     *
     * Called automatically by @c AudioChunkHandle::~AudioChunkHandle().
     * Do not call this directly unless you are managing a raw buffer pointer.
     */
    void return_buffer(AudioBuffer* buffer);

    /// Number of chunks dropped because the consumer was too slow.
    uint32_t overwritten_chunk_count() const;

    /// Reset the overwritten chunk counter to zero.
    void reset_overwritten_chunk_count();

private:
    /// ISR callback registered with the I2S driver. Must stay in IRAM.
    static bool IRAM_ATTR on_receive_callback(i2s_chan_handle_t handle,
                                               i2s_event_data_t* event,
                                               void* user_ctx);

    /// Internal task driving the record → pause → record cycle (pause mode only).
    static void recorder_task_fn(void* ctx);

    /// Drain both queues and refill the empty pool; used before (re)starting.
    void reset_buffers();

    gpio_num_t clk_;
    gpio_num_t data_;
    uint32_t sample_rate_;
    i2s_chan_handle_t rx_handle_{nullptr};
    i2s_chan_config_t chan_cfg_;
    i2s_pdm_rx_config_t pdm_cfg_;
    bool is_recording_flag_{false};

    /// Queue of full buffers waiting to be consumed.
    QueueHandle_t audio_queue_{nullptr};
    /// Queue of empty buffers ready for the next DMA fill cycle.
    QueueHandle_t empty_audio_queue_{nullptr};

    AudioBuffer* current_buffer_{nullptr};   ///< Buffer currently being filled by the ISR.
    int64_t current_timestamp_{0};           ///< esp_timer value when the current fill started.
    size_t samples_collected_{0};            ///< Samples written into current_buffer_ so far.

    static constexpr size_t POOL_SIZE{3};    ///< Total number of AudioBuffers in the pool.
    std::atomic<uint32_t> overwritten_chunk_count_{0};

    uint32_t inter_chunk_pause_ms_{0};       ///< 0 = continuous, >0 = pause-mode.
    /// Given by ISR when a chunk completes; taken by recorder task to trigger disable+pause.
    SemaphoreHandle_t chunk_done_sem_{nullptr};
    TaskHandle_t recorder_task_handle_{nullptr};
};
