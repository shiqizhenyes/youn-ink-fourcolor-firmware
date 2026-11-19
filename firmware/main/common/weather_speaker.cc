/**
 * @file weather_speaker.cc
 * @brief Offline Chinese TTS for weather announcements
 */

#include "weather_speaker.h"

#include "audio_service.h"
#include "weather_prefs.h"

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_pm.h>
#include <esp_tts.h>
#include <esp_tts_voice_template.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if CONFIG_IDF_TARGET_ESP32S3
#include <lingtts_engine.h>
#endif

#include <vector>

namespace ink {

namespace {

constexpr char kTag[] = "WeatherSpeaker";
constexpr char kVoicePartition[] = "voice_data";
constexpr esp_partition_subtype_t kVoiceSubtype = static_cast<esp_partition_subtype_t>(0x40);
constexpr unsigned int kSpeechSpeed = 3;  // 0 (slowest) .. 5 (fastest), esp_tts only
constexpr uint32_t kTaskStackBytes = 20 * 1024;

#if CONFIG_IDF_TARGET_ESP32S3
constexpr size_t kLingChunkSamples = 16000 * 400 / 1000;  // 400 ms at 16 kHz

struct LingPcmCtx {
    std::atomic<uint32_t>* generation;
    uint32_t expected;
    AudioService* audio;
    std::vector<int16_t> pending;
};

int OnLingPcm(const int16_t* pcm, size_t n, void* user) {
    auto* ctx = static_cast<LingPcmCtx*>(user);
    if (ctx->generation->load() != ctx->expected) return 1;
    if (pcm != nullptr && n > 0) {
        ctx->pending.insert(ctx->pending.end(), pcm, pcm + n);
    }
    if (ctx->pending.size() < kLingChunkSamples) return 0;
    std::vector<int16_t> chunk = std::move(ctx->pending);
    ctx->pending.clear();
    ctx->pending.reserve(kLingChunkSamples);
    if (!ctx->audio->PlayPcm(std::move(chunk))) return 1;
    return ctx->generation->load() == ctx->expected ? 0 : 1;
}
#endif

#if CONFIG_PM_ENABLE
class CpuFreqGuard {
public:
    CpuFreqGuard() {
        if (!lock_) {
            if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "wx_tts", &lock_) != ESP_OK) {
                lock_ = nullptr;
            }
        }
        if (lock_) esp_pm_lock_acquire(lock_);
    }
    ~CpuFreqGuard() {
        if (lock_) esp_pm_lock_release(lock_);
    }
    CpuFreqGuard(const CpuFreqGuard&) = delete;
    CpuFreqGuard& operator=(const CpuFreqGuard&) = delete;

private:
    static esp_pm_lock_handle_t lock_;
};

esp_pm_lock_handle_t CpuFreqGuard::lock_ = nullptr;
#endif

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

bool WeatherSpeaker::EnsureXiaole() {
    if (tts_) return true;
    if (xiaole_failed_) return false;
    xiaole_failed_ = true;

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
    xiaole_failed_ = false;
    ESP_LOGI(kTag, "esp_tts ready");
    return true;
}

bool WeatherSpeaker::EnsureLing() {
#if !CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGE(kTag, "LingTTS is only built for esp32s3");
    return false;
#else
    if (ling_) return true;
    if (ling_failed_) return false;
    ling_failed_ = true;

    lingtts_engine_cfg_t cfg;
    lingtts_engine_default_cfg(&cfg);
    cfg.mem = LINGTTS_MEM_PSRAM;
    cfg.flags = LINGTTS_ENGINE_NO_COPY;
    cfg.speed = 1.0f;
    lingtts_engine_t* engine = nullptr;
    const int rc = lingtts_engine_create(&engine, &cfg);
    if (rc != LINGTTS_OK || !engine) {
        ESP_LOGE(kTag, "lingtts init failed (%d); flash the full image once", rc);
        return false;
    }
    ling_ = engine;
    ling_failed_ = false;
    ESP_LOGI(kTag, "lingtts ready");
    return true;
#endif
}

void WeatherSpeaker::ReleaseLing() {
#if CONFIG_IDF_TARGET_ESP32S3
    if (ling_) {
        lingtts_engine_destroy(static_cast<lingtts_engine_t*>(ling_));
        ling_ = nullptr;
        ESP_LOGI(kTag, "lingtts released");
    }
    ling_failed_ = false;
#else
    ling_ = nullptr;
    ling_failed_ = false;
#endif
}

void WeatherSpeaker::SpeakOne(const std::string& text, uint32_t generation) {
    if (GetWeatherVoice() == kWeatherVoiceFemaleA) {
        SpeakLing(text, generation);
        return;
    }
    ReleaseLing();
    SpeakXiaole(text, generation);
}

void WeatherSpeaker::SpeakXiaole(const std::string& text, uint32_t generation) {
    if (!EnsureXiaole()) return;
    auto still_current = [this, generation]() { return generation_.load() == generation; };
    if (!still_current()) return;

    audio_->UnmuteOutput();
    ESP_LOGI(kTag, "Speaking (小乐): %s", text.c_str());
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

void WeatherSpeaker::SpeakLing(const std::string& text, uint32_t generation) {
#if CONFIG_PM_ENABLE
    CpuFreqGuard cpu_freq;
#endif
    if (!EnsureLing()) return;
    if (generation_.load() != generation) return;

    audio_->UnmuteOutput();
    ESP_LOGI(kTag, "Speaking (女声A): %s", text.c_str());
#if !CONFIG_IDF_TARGET_ESP32S3
    (void)text;
#else
    LingPcmCtx ctx;
    ctx.generation = &generation_;
    ctx.expected = generation;
    ctx.audio = audio_;
    ctx.pending.reserve(kLingChunkSamples);
    const int rc = lingtts_engine_speak(
        static_cast<lingtts_engine_t*>(ling_), text.c_str(),
        reinterpret_cast<lingtts_pcm_cb>(OnLingPcm), &ctx);
    if (generation_.load() == generation && !ctx.pending.empty()) {
        audio_->PlayPcm(std::move(ctx.pending));
    }
    if (rc != LINGTTS_OK && rc != LINGTTS_STOPPED) {
        ESP_LOGW(kTag, "lingtts speak failed (%d)", rc);
    }
#endif
}

}  // namespace ink
