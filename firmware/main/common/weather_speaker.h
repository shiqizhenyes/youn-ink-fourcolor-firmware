/**
 * @file weather_speaker.h
 * @brief Offline Chinese TTS for weather announcements
 *
 * 小林 uses LingTTS and the lingtts_model / lingtts_fe partitions. The engine
 * is created on first use and kept for later announcements.
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
    bool EnsureLing();
    void SpeakOne(const std::string& text, uint32_t generation);
    void SpeakLing(const std::string& text, uint32_t generation);

    AudioService* audio_ = nullptr;
    void* ling_ = nullptr;
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
