/**
 * @file weather_speaker.h
 * @brief Offline Chinese TTS for weather announcements
 *
 * 小乐 uses ESP-SR esp_tts and the voice_data partition (esp_tts_voice_data_xiaole.dat).
 * 女声A uses LingTTS and the lingtts_model / lingtts_fe partitions. The selected
 * voice is read from weather prefs on each announcement. Engines are created
 * on first use; leaving 女声A destroys the LingTTS engine.
 */

#ifndef WEATHER_SPEAKER_H
#define WEATHER_SPEAKER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

class AudioService;

namespace ink {

class WeatherSpeaker {
public:
    static WeatherSpeaker& GetInstance();

    void SetAudioService(AudioService* audio);

    /** @brief Speak text, interrupting any announcement in progress */
    void Speak(const std::string& text);
    /** @brief Cut the current announcement short and drop queued audio */
    void Stop();
    /** @brief True while synthesizing or an announcement is queued */
    bool IsSpeaking() const;

private:
    WeatherSpeaker() = default;
    static void SpeakTask(void* arg);
    bool EnsureXiaole();
    bool EnsureLing();
    void ReleaseLing();
    void SpeakOne(const std::string& text, uint32_t generation);
    void SpeakXiaole(const std::string& text, uint32_t generation);
    void SpeakLing(const std::string& text, uint32_t generation);

    AudioService* audio_ = nullptr;
    void* tts_ = nullptr;
    void* ling_ = nullptr;
    bool xiaole_failed_ = false;
    bool ling_failed_ = false;

    std::mutex mutex_;
    std::string pending_;
    bool has_pending_ = false;
    bool task_running_ = false;
    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> speaking_{false};
};

}  // namespace ink

#endif  // WEATHER_SPEAKER_H
