/**
 * @file weather_speaker.h
 * @brief Offline Chinese TTS (ESP-SR esp_tts) for weather announcements
 *
 * The voice set is read from the voice_data partition, flashed from esp-sr's
 * esp_tts_voice_data_xiaole.dat. The engine is created on first use.
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
    bool EnsureEngine();
    void SpeakOne(const std::string& text, uint32_t generation);

    AudioService* audio_ = nullptr;
    void* tts_ = nullptr;
    bool engine_failed_ = false;

    std::mutex mutex_;
    std::string pending_;
    bool has_pending_ = false;
    bool task_running_ = false;
    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> speaking_{false};
};

}  // namespace ink

#endif  // WEATHER_SPEAKER_H
