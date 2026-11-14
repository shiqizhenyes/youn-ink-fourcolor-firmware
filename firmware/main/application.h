#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>

#include "audio_service.h"
#include "device_state.h"

namespace ui {
class RawDrawUiManager;
}

namespace ink {
struct WeatherData;
}

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Initialize();
    void Run();

    DeviceState GetDeviceState() const { return state_.load(std::memory_order_acquire); }
    bool SetDeviceState(DeviceState state);

    void Schedule(std::function<void()>&& callback);
    void PlaySound(const std::string_view& sound);
    void PlaySound(const std::string_view& sound, int duration_ms);
    void MuteSound();
    void StopSound();
    bool CanEnterSleepMode() const;

    AudioService& GetAudioService() { return audio_service_; }
    ui::RawDrawUiManager* GetRawDrawUiManager() { return rawdraw_ui_manager_.get(); }
    void UpdateStatusBarForUi(bool request_panel_refresh = true);
    /**
     * @brief Fetch weather if Wi-Fi is up and the cache is stale (or always when force)
     * @return true if a fetch task was started
     */
    bool StartWeatherFetch(bool force);
    void OnSntpSynced();
    void OnUpClick();
    void OnUpDoubleClick();
    void OnDownClick();
    void OnUpLongPress();
    void OnDownLongPress();
    void OnWifiConfigComboLongPress();
    void OnBootClick();
    void OnBootDoubleClick();
    void OnBootLongPress();

private:
    Application();
    ~Application();

    std::atomic<DeviceState> state_{kDeviceStateUnknown};
    std::atomic<bool> wifi_connected_{false};
    AudioService audio_service_;
    std::unique_ptr<ui::RawDrawUiManager> rawdraw_ui_manager_;
    std::atomic<int64_t> last_activity_us_{0};

    // Weather announcements
    std::atomic<bool> speak_after_fetch_{false};   // weather page entered, speak when the fetch lands
    std::atomic<bool> page_speech_{false};         // current speech came from entering the page
    std::atomic<bool> broadcast_pending_{false};   // daily announcement waiting for fresh data
    std::atomic<bool> resleep_pending_{false};     // timer wake: go back to sleep once spoken
    std::atomic<bool> sntp_synced_{false};
    std::atomic<bool> user_active_{false};
    bool woke_for_broadcast_ = false;
    bool woke_for_refresh_ = false;                // timer wake for the day/night weather refresh
    bool woke_for_calendar_ = false;               // timer wake to redraw the calendar page
    int last_refresh_check_minute_ = -1;
    int64_t broadcast_started_us_ = 0;
    int64_t resleep_ready_us_ = 0;
    int last_broadcast_check_minute_ = -1;

    void SetupInkWeather();
    void SetupWeatherSettingsItems();
    void OnWeatherPageSwitched(bool entered);
    void OnWeatherFetched(bool ok, const ink::WeatherData& data);
    bool SpeakWeather(const ink::WeatherData& data);
    bool SpeakCachedWeather();
    void CheckDailyBroadcast();
    void TriggerDailyBroadcast(int today);
    void FinishBroadcast();
    /**
     * @brief Arm the ESP32 RC timer for the next wall-clock wake
     * @param programmed_s if non-null, receives the RC-timer seconds actually programmed
     * @return wall-clock seconds until the intended wake, 0 if none was armed
     */
    uint32_t ArmTimerWakeup(uint32_t* programmed_s = nullptr);
    /** @brief Long-press UP/DOWN on a selected settings Slider; false if not applicable */
    bool AdjustSettingsSlider(int delta);
    void PlayVolumePreview(int volume);
    void CheckWeatherRefresh();
    void MarkUserActivity();
    /** @brief Deep sleep after kIdleSleepUs without input, except on pages that stay awake */
    void CheckIdleSleep();
    bool IsSleepBlockedByPage() const;
    bool IsBusyForSleep();
    void WaitForDisplayRefreshIdle();
    void EnterManualSleep();
    void NoteButtonActivity();
    void EnterWifiConfigMode();
};

#endif  // _APPLICATION_H_
