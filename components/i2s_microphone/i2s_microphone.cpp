#include "i2s_microphone.hpp"
#include <cstring>
#include "esp_timer.h"
#include "esp_heap_caps.h"

// Internal POD used to pass buffer pointers through FreeRTOS queues.
// Kept in this translation unit — callers only see AudioChunkHandle.
struct QueuedAudioChunk {
    AudioBuffer* buffer;
    int64_t timestamp_us;
};

I2SMicrophone::I2SMicrophone(gpio_num_t clk, gpio_num_t data, uint32_t sample_rate,
                              uint32_t inter_chunk_pause_ms)
    : clk_(clk), data_(data), sample_rate_(sample_rate),
      inter_chunk_pause_ms_(inter_chunk_pause_ms)
{
    audio_queue_       = xQueueCreate(POOL_SIZE, sizeof(QueuedAudioChunk));
    empty_audio_queue_ = xQueueCreate(POOL_SIZE, sizeof(QueuedAudioChunk));

    // Pre-allocate all AudioBuffers in PSRAM to avoid exhausting internal heap.
    for (int i = 0; i < POOL_SIZE; i++) {
        auto* buffer = static_cast<AudioBuffer*>(
            heap_caps_malloc(sizeof(AudioBuffer), MALLOC_CAP_SPIRAM));
        assert(buffer && "Failed to allocate audio buffer in PSRAM");
        QueuedAudioChunk chunk = { buffer, 0 };
        xQueueSend(empty_audio_queue_, &chunk, 0);
    }

    chan_cfg_ = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg_.dma_desc_num = 4;     // number of DMA descriptors (ring buffer depth)
    chan_cfg_.dma_frame_num = 1000; // samples per DMA interrupt (~62.5 ms at 16 kHz)
    i2s_new_channel(&chan_cfg_, nullptr, &rx_handle_);  // nullptr = no TX channel

    pdm_cfg_ = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sample_rate_),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = clk_,
            .din = data_,
            .invert_flags = { .clk_inv = false }
        },
    };
    i2s_channel_init_pdm_rx_mode(rx_handle_, &pdm_cfg_);

    i2s_event_callbacks_t cbs = {
        .on_recv       = I2SMicrophone::on_receive_callback,
        .on_recv_q_ovf = nullptr,
        .on_sent       = nullptr,
        .on_send_q_ovf = nullptr
    };
    // Pass `this` as user_ctx so the static callback can reach instance state.
    i2s_channel_register_event_callback(rx_handle_, &cbs, this);

    // Grab the first empty buffer so the ISR has somewhere to write immediately.
    QueuedAudioChunk first_chunk{nullptr, 0};
    xQueueReceive(empty_audio_queue_, &first_chunk, 0);
    current_buffer_ = first_chunk.buffer;

    if (inter_chunk_pause_ms_ > 0) {
        chunk_done_sem_ = xSemaphoreCreateBinary();
        // Priority 6 — one above audio_task (5) so it reacts to chunk completion promptly.
        xTaskCreate(recorder_task_fn, "mic_rec", 2048, this, 6, &recorder_task_handle_);
    }
}

I2SMicrophone::~I2SMicrophone()
{
    if (recorder_task_handle_) vTaskDelete(recorder_task_handle_);
    if (chunk_done_sem_) vSemaphoreDelete(chunk_done_sem_);
    if (is_recording_flag_) i2s_channel_disable(rx_handle_);
    if (rx_handle_) i2s_del_channel(rx_handle_);
    // Free every buffer still in the pool (current + both queues).
    heap_caps_free(current_buffer_);
    QueuedAudioChunk chunk{nullptr, 0};
    while (xQueueReceive(audio_queue_,       &chunk, 0) == pdTRUE) heap_caps_free(chunk.buffer);
    while (xQueueReceive(empty_audio_queue_, &chunk, 0) == pdTRUE) heap_caps_free(chunk.buffer);
}

void I2SMicrophone::recorder_task_fn(void* ctx)
{
    auto* self = static_cast<I2SMicrophone*>(ctx);
    while (true) {
        // Sleep until start() sends a notification.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (self->is_recording_flag_) {
            // Start a fresh chunk cycle.
            self->reset_buffers();
            self->current_timestamp_ = esp_timer_get_time();
            i2s_channel_enable(self->rx_handle_);

            // Block until the ISR signals that a full chunk is ready.
            xSemaphoreTake(self->chunk_done_sem_, portMAX_DELAY);

            // Stop DMA so the consumer can process without racing the ISR.
            i2s_channel_disable(self->rx_handle_);

            // Pause to allow the consumer (e.g. MQTT publish) to finish.
            if (self->is_recording_flag_)
                vTaskDelay(pdMS_TO_TICKS(self->inter_chunk_pause_ms_));
        }
    }
}

bool IRAM_ATTR I2SMicrophone::on_receive_callback(i2s_chan_handle_t /*handle*/,
                                                    i2s_event_data_t* event,
                                                    void* user_ctx)
{
    auto* self = static_cast<I2SMicrophone*>(user_ctx);
    BaseType_t higher_priority_task_woken = pdFALSE;

    // Append the incoming DMA chunk to the current large buffer.
    // Cast to uint8_t* for byte-level offset arithmetic before memcpy.
    auto* dst = reinterpret_cast<uint8_t*>(self->current_buffer_->data())
                + self->samples_collected_ * sizeof(int16_t);
    std::memcpy(dst, event->dma_buf, event->size);
    self->samples_collected_ += event->size / sizeof(int16_t);

    if (self->samples_collected_ >= AUDIO_CHUNK_SIZE) {
        // Buffer is full: push it onto the ready queue for the consumer.
        QueuedAudioChunk full_chunk{self->current_buffer_, self->current_timestamp_};
        xQueueSendFromISR(self->audio_queue_, &full_chunk, &higher_priority_task_woken);

        // Try to grab a fresh empty buffer; if none available, steal the oldest
        // full one so recording is never interrupted (at the cost of data loss).
        QueuedAudioChunk next_chunk{nullptr, 0};
        if (xQueueReceiveFromISR(self->empty_audio_queue_, &next_chunk, &higher_priority_task_woken) != pdTRUE) {
            xQueueReceiveFromISR(self->audio_queue_, &next_chunk, &higher_priority_task_woken);
            self->overwritten_chunk_count_++;
        }

        self->current_timestamp_ = esp_timer_get_time();
        self->current_buffer_    = next_chunk.buffer;
        self->samples_collected_ = 0;

        // In pause mode, wake the recorder task to stop the channel and wait.
        if (self->chunk_done_sem_) {
            BaseType_t sem_woken = pdFALSE;
            xSemaphoreGiveFromISR(self->chunk_done_sem_, &sem_woken);
            higher_priority_task_woken |= sem_woken;
        }
    }

    // Returning true requests an immediate context switch if a higher-priority
    // task was unblocked by one of the queue operations above.
    return higher_priority_task_woken == pdTRUE;
}

std::optional<I2SMicrophone::AudioChunkHandle> I2SMicrophone::get_audio()
{
    QueuedAudioChunk chunk{nullptr, 0};
    if (xQueueReceive(audio_queue_, &chunk, pdMS_TO_TICKS(3000)) == pdTRUE)
        return AudioChunkHandle(this, chunk.buffer, chunk.timestamp_us);
    return std::nullopt;
}

void I2SMicrophone::start()
{
    if (!is_recording_flag_) {
        is_recording_flag_ = true;
        if (recorder_task_handle_) {
            // Pause mode: notify the recorder task to begin the record/pause cycle.
            xTaskNotifyGive(recorder_task_handle_);
        } else {
            // Continuous mode: enable the channel directly.
            reset_buffers();
            current_timestamp_ = esp_timer_get_time();
            i2s_channel_enable(rx_handle_);
        }
    }
}

void I2SMicrophone::stop()
{
    if (is_recording_flag_) {
        is_recording_flag_ = false;
        i2s_channel_disable(rx_handle_);
        // Unblock the recorder task if it is waiting on chunk_done_sem_.
        if (chunk_done_sem_) xSemaphoreGive(chunk_done_sem_);
    }
}

void I2SMicrophone::return_buffer(AudioBuffer* buffer)
{
    QueuedAudioChunk chunk{buffer, 0};
    xQueueSend(empty_audio_queue_, &chunk, 0);
}

uint32_t I2SMicrophone::overwritten_chunk_count() const
{
    return overwritten_chunk_count_;
}

void I2SMicrophone::reset_overwritten_chunk_count()
{
    overwritten_chunk_count_.store(0);
}

void I2SMicrophone::reset_buffers()
{
    // Collect every buffer pointer — from the current slot and both queues —
    // then return them all to the empty pool so the ISR starts clean.
    std::array<AudioBuffer*, POOL_SIZE> buffers{};
    size_t count = 0;
    if (current_buffer_) {
        buffers[count++] = current_buffer_;
        current_buffer_  = nullptr;
    }
    QueuedAudioChunk chunk{nullptr, 0};
    while (xQueueReceive(audio_queue_,       &chunk, 0) == pdTRUE) buffers[count++] = chunk.buffer;
    while (xQueueReceive(empty_audio_queue_, &chunk, 0) == pdTRUE) buffers[count++] = chunk.buffer;

    for (size_t i = 0; i < count; i++) {
        QueuedAudioChunk empty{buffers[i], 0};
        xQueueSend(empty_audio_queue_, &empty, 0);
    }

    // Take one buffer out of the empty pool for the ISR to start filling.
    xQueueReceive(empty_audio_queue_, &chunk, 0);
    current_buffer_    = chunk.buffer;
    samples_collected_ = 0;
    current_timestamp_ = 0;
}
