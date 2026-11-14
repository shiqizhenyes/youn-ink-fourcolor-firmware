/**
 * @file weather_speaker.cc
 * @brief Offline Chinese TTS (ESP-SR esp_tts) for weather announcements
 */

#include "weather_speaker.h"

#include "audio_service.h"

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_tts.h>
#include <esp_tts_voice_template.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <vector>

namespace ink {

namespace {

constexpr char kTag[] = "WeatherSpeaker";
constexpr char kVoicePartition[] = "voice_data";
constexpr esp_partition_subtype_t kVoiceSubtype = static_cast<esp_partition_subtype_t>(0x40);
constexpr unsigned int kSpeechSpeed = 3;  // 0 (slowest) .. 5 (fastest)
constexpr uint32_t kTaskStackBytes = 12 * 1024;

}  // namespace

WeatherSpeaker& WeatherSpeaker::GetInstance() {
    static WeatherSpeaker instance;
    return instance;
}

void WeatherSpeaker::SetAudioService(AudioService* audio) {
    audio_ = audio;
}

bool WeatherSpeaker::IsSpeaking() const {
    return speaking_.load(std::memory_order_acquire);
}

void WeatherSpeaker::Speak(const std::string& text) {
    if (text.empty() || !audio_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = text;
    has_pending_ = true;
    const uint32_t generation = generation_.fetch_add(1) + 1;
    speaking_.store(true, std::memory_order_release);
    if (task_running_) {
        // The running task notices the new generation and picks up pending_.
        audio_->MuteOutput();
        return;
    }
    task_running_ = true;
    if (xTaskCreate(SpeakTask, "wx_tts", kTaskStackBytes, this, 3, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Failed to create TTS task (generation %u)", static_cast<unsigned>(generation));
        task_running_ = false;
        has_pending_ = false;
        speaking_.store(false, std::memory_order_release);
    }
}

void WeatherSpeaker::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        has_pending_ = false;
        pending_.clear();
        generation_.fetch_add(1);
        if (!task_running_) speaking_.store(false, std::memory_order_release);
    }
    if (audio_) audio_->MuteOutput();
}

void WeatherSpeaker::SpeakTask(void* arg) {
    auto* self = static_cast<WeatherSpeaker*>(arg);
    while (true) {
        std::string text;
        uint32_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (!self->has_pending_) {
                self->task_running_ = false;
                self->speaking_.store(false, std::memory_order_release);
                break;
            }
            text = std::move(self->pending_);
            self->has_pending_ = false;
            generation = self->generation_.load();
        }
        self->SpeakOne(text, generation);
    }
    vTaskDelete(nullptr);
}

bool WeatherSpeaker::EnsureEngine() {
    if (tts_) return true;
    if (engine_failed_) return false;
    engine_failed_ = true;

    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kVoiceSubtype, kVoicePartition);
    if (!part) {
        ESP_LOGE(kTag, "Partition %s not found; flash the full image once", kVoicePartition);
        return false;
    }
    const void* data = nullptr;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &data, &handle) != ESP_OK) {
        ESP_LOGE(kTag, "Failed to mmap %s", kVoicePartition);
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (bytes[0] == 0xFF && bytes[1] == 0xFF && bytes[2] == 0xFF && bytes[3] == 0xFF) {
        ESP_LOGE(kTag, "Partition %s is empty; flash the full image once", kVoicePartition);
        esp_partition_munmap(handle);
        return false;
    }
    esp_tts_voice_t* voice = esp_tts_voice_set_init(&esp_tts_voice_template, const_cast<void*>(data));
    esp_tts_handle_t tts = voice ? esp_tts_create(voice) : nullptr;
    if (!tts) {
        ESP_LOGE(kTag, "esp_tts init failed");
        if (voice) esp_tts_voice_set_free(voice);
        esp_partition_munmap(handle);
        return false;
    }
    // The voice set and mapping stay alive for the engine's lifetime.
    tts_ = tts;
    engine_failed_ = false;
    ESP_LOGI(kTag, "esp_tts ready");
    return true;
}

void WeatherSpeaker::SpeakOne(const std::string& text, uint32_t generation) {
    if (!EnsureEngine()) return;
    auto still_current = [this, generation]() { return generation_.load() == generation; };
    if (!still_current()) return;

    audio_->UnmuteOutput();
    ESP_LOGI(kTag, "Speaking: %s", text.c_str());
    if (!esp_tts_parse_chinese(tts_, text.c_str())) {
        ESP_LOGW(kTag, "esp_tts could not parse text");
        return;
    }
    while (still_current()) {
        int len = 0;
        short* samples = esp_tts_stream_play(tts_, &len, kSpeechSpeed);
        if (len <= 0 || !samples) break;
        std::vector<int16_t> pcm(samples, samples + len);
        if (!audio_->PlayPcm(std::move(pcm))) break;
    }
    esp_tts_stream_reset(tts_);
}

}  // namespace ink
