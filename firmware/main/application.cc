#include "application.h"

#include "audio_codec.h"
#include "boards/zectrix-s3-epaper-4.2/custom_lcd_display.h"
#include "boards/zectrix-s3-epaper-4.2/config.h"
#include "board.h"
#include "boards/zectrix/zectrix_nfc.h"
#include "common/qweather_client.h"
#include "common/photo_storage.h"
#include "common/power_diag.h"
#include "common/weather_prefs.h"
#include "common/weather_speaker.h"
#include "display.h"
#include "settings.h"
#include "ui/rawdraw_ui_manager.h"
#include "ui/settings_item_index.h"
#include "wifi_manager.h"

#include <esp_mac.h>
#include <esp_log.h>
#include <esp_attr.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>

namespace {

constexpr char kTag[] = "Application";

constexpr char kGalleryNamespace[] = "gallery";
constexpr char kSlideshowIntervalKey[] = "slide_min";
using ui::kSettingsAutoSpeakIndex;
using ui::kSettingsBatteryIndex;
using ui::kSettingsBroadcastIndex;
using ui::kSettingsDayRefreshIndex;
using ui::kSettingsHttpServerIndex;
using ui::kSettingsLanIpIndex;
using ui::kSettingsNightRefreshIndex;
using ui::kSettingsSlideshowIndex;
using ui::kSettingsVolumeIndex;
using ui::kSettingsWeatherVoiceIndex;
using ui::kSettingsWifiIndex;
constexpr int64_t kIdleSleepUs = 5LL * 60 * 1000 * 1000;
// Cap on waiting for a busy fetch/announcement/refresh before sleeping anyway.
constexpr int64_t kIdleBusyMaxDeferUs = 2LL * 60 * 1000 * 1000;
// Calendar page: redraw at least this often while asleep.
constexpr int kCalendarWakeSec = 2 * 60 * 60;
// Daily announcement: how late it may still play, how long to stay awake
// waiting for it after a timer wake, and how long to wait for fresh data.
constexpr int kBroadcastLateWindowMin = 60;
constexpr int kBroadcastWaitAwakeMin = 10;
constexpr int64_t kBroadcastFallbackUs = 90LL * 1000 * 1000;
constexpr int64_t kWakeDecisionTimeoutUs = 45LL * 1000 * 1000;
constexpr int64_t kResleepGraceUs = 5LL * 1000 * 1000;
// Button and timer wakes wait this long for Wi-Fi, then paint or speak from cache.
constexpr int64_t kWakeConnectTimeoutUs = 20LL * 1000 * 1000;

enum TimerWakeReason : int {
    kTimerWakeNone = 0,
    kTimerWakeBroadcast = 1,
    kTimerWakeRefresh = 2,
    kTimerWakeCalendar = 3,
};
RTC_DATA_ATTR int s_timer_wake_reason = kTimerWakeNone;
// Page shown when the device went to sleep; restored on wake for settings, weather, and calendar.
RTC_DATA_ATTR int s_sleep_page = -1;
// Settings row selected before sleep. The left category follows this row.
RTC_DATA_ATTR char s_settings_selected_label[48] = {};

int LocalDateKey(const struct tm& tm) {
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

std::string FormatMinutesLabel(int minutes) {
    if (minutes <= 0) return "关闭";
    char buf[16];
    snprintf(buf, sizeof(buf), "%dmin", minutes);
    return buf;
}

const char* FormatMinutesLogLabel(int minutes) {
    return minutes <= 0 ? "关闭" : "开启";
}

int NextSlideshowInterval(int current) {
    static constexpr int kOptions[] = {0, 5, 10, 30};
    for (size_t i = 0; i < sizeof(kOptions) / sizeof(kOptions[0]); ++i) {
        if (kOptions[i] == current) {
            return kOptions[(i + 1) % (sizeof(kOptions) / sizeof(kOptions[0]))];
        }
    }
    return 5;
}

void UpdateWifiSettingsItem(rawdraw::SettingsRenderer* renderer, bool connected,
                            const char* value = nullptr) {
    if (!renderer) return;
    renderer->UpdateChecked(kSettingsWifiIndex, connected);
    renderer->UpdateItem(kSettingsWifiIndex, value ? value : (connected ? "已连接" : "未连接"));
}

void UpdateHttpServerSettingsItem(rawdraw::SettingsRenderer* renderer, bool running,
                                  const std::string& ip_address = "") {
    if (!renderer) return;
    std::string value;
    if (running && !ip_address.empty()) {
        value = "http://" + ip_address;
    } else if (!ip_address.empty()) {
        value = ip_address;
    } else {
        value = running ? "已开启" : "已关闭";
    }
    renderer->UpdateChecked(kSettingsHttpServerIndex, running);
    renderer->UpdateItem(kSettingsHttpServerIndex, value);
}

void UpdateLanIpSettingsItem(rawdraw::SettingsRenderer* renderer, const std::string& ip_address) {
    if (!renderer) return;
    renderer->UpdateItem(kSettingsLanIpIndex, ip_address.empty() ? "未获取" : ip_address);
}

void StartSntpClockSyncOnce() {
    static bool s_started = false;
    if (s_started) return;

    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb([](struct timeval*) {
        time_t now = 0;
        time(&now);
        struct tm local_tm = {};
        localtime_r(&now, &local_tm);
        char time_buf[32] = {};
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &local_tm);
        ESP_LOGI(kTag, "SNTP time synchronized: %s", time_buf);
        Application::GetInstance().UpdateStatusBarForUi();
        Application::GetInstance().OnSntpSynced();
        ink::PowerDiag::OnSntpSynced();
    });
    esp_sntp_init();
    s_started = true;
    ESP_LOGI(kTag, "SNTP started: tz=Asia/Shanghai servers=ntp.aliyun.com,cn.pool.ntp.org,pool.ntp.org");
}

void ConfigureDynamicFrequency() {
#if CONFIG_PM_ENABLE
    // Automatic light sleep stays off: it would stall the I2S output and the
    // GPIO button interrupts.
    esp_pm_config_t pm_config = {};
    pm_config.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    pm_config.min_freq_mhz = 80;
    pm_config.light_sleep_enable = false;
    const esp_err_t err = esp_pm_configure(&pm_config);
    ESP_LOGI(kTag, "Dynamic frequency scaling %d-%d MHz: %s", pm_config.min_freq_mhz,
             pm_config.max_freq_mhz, esp_err_to_name(err));
#endif
}

}  // namespace

Application::Application() = default;

Application::~Application() = default;

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);
    ConfigureDynamicFrequency();
    ink::PowerDiag::OnBoot();

    AudioCodec* codec = board.GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGE(kTag, "Audio codec is null");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }

    audio_service_.Initialize(codec);
    audio_service_.Start();
    ink::WeatherSpeaker::GetInstance().SetAudioService(&audio_service_);

    Display* display = board.GetDisplay();
    if (display == nullptr) {
        ESP_LOGW(kTag, "No display available, skipping init");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }
    if (photo_storage_init() == 0) {
        ESP_LOGI(kTag, "Photo storage ready (%d photos)", photo_get_count());
    } else {
        ESP_LOGW(kTag, "Photo storage init failed");
    }

    const bool from_deep_sleep = esp_reset_reason() == ESP_RST_DEEPSLEEP;
    const bool timer_wake = from_deep_sleep &&
                            (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_TIMER)) != 0;
    woke_for_refresh_ = timer_wake && s_timer_wake_reason == kTimerWakeRefresh;
    woke_for_calendar_ = timer_wake && s_timer_wake_reason == kTimerWakeCalendar;
    woke_for_broadcast_ = timer_wake && !woke_for_refresh_ && !woke_for_calendar_;
    s_timer_wake_reason = kTimerWakeNone;

    auto* lcd = static_cast<CustomLcdDisplay*>(display);
    rawdraw_ui_manager_ = std::make_unique<ui::RawDrawUiManager>();
    const bool restore_sleep_page =
        from_deep_sleep && (s_sleep_page == static_cast<int>(ui::RawDrawPageId::Settings) ||
                            s_sleep_page == static_cast<int>(ui::RawDrawPageId::Weather) ||
                            s_sleep_page == static_cast<int>(ui::RawDrawPageId::Calendar));
    if (restore_sleep_page) {
        rawdraw_ui_manager_->SetInitialPage(static_cast<ui::RawDrawPageId>(s_sleep_page));
    }
    s_sleep_page = -1;
    const bool button_wake = from_deep_sleep && !timer_wake;
    defer_wake_refresh_ = button_wake;
    // Keep the previous e-paper image until Wi-Fi connects or the connect wait ends.
    rawdraw_ui_manager_->SetSkipInitialRefresh(woke_for_refresh_ || woke_for_broadcast_ || button_wake);
    rawdraw::ApTransferServer::SetActivityCallback([this]() { MarkUserActivity(); });
    rawdraw_ui_manager_->Init(lcd, [lcd](const rawdraw::Rect&, bool urgent) {
        if (urgent) {
            lcd->RequestUrgentFullRefresh();
        } else {
            lcd->RequestUrgentRefresh();
        }
    });
    rawdraw_ui_manager_->SetRefreshIdleListener([this]() { OnVolumeRepeatRefreshIdle(); });

    if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
        Settings gallery_nvs(kGalleryNamespace, false);
        int slideshow_interval = gallery_nvs.GetInt(kSlideshowIntervalKey, 5);
        if (slideshow_interval != 0 && slideshow_interval != 5 &&
            slideshow_interval != 10 && slideshow_interval != 30) {
            slideshow_interval = 5;
        }
        ESP_LOGI(kTag, "Startup gallery fullscreen slideshow: %s, interval=%s",
                 FormatMinutesLogLabel(slideshow_interval),
                 FormatMinutesLabel(slideshow_interval).c_str());
        rawdraw_ui_manager_->SetGallerySlideshowIntervalMinutes(slideshow_interval);

        std::vector<rawdraw::SettingsItemDef> items;
        items.push_back({"系统", "", nullptr, rawdraw::SettingsItemType::Section, false});
        int battery_level = -1;
        bool battery_charging = false;
        bool battery_discharging = false;
        if (!board.GetBatteryLevel(battery_level, battery_charging, battery_discharging)) {
            battery_level = -1;
        }
        rawdraw::SettingsItemDef battery_item{"电池", "", nullptr, rawdraw::SettingsItemType::Meter, false};
        battery_item.slider_value = battery_level;
        items.push_back(std::move(battery_item));
        rawdraw::SettingsItemDef volume_item{"音量", "", nullptr, rawdraw::SettingsItemType::Slider, false};
        volume_item.slider_value = codec->output_volume();
        volume_item.on_slider = [codec](int volume) {
            codec->SetOutputVolume(volume);
        };
        items.push_back(std::move(volume_item));
        items.push_back({"NFC", "", nullptr, rawdraw::SettingsItemType::Checkbox, ZectrixGetNfcEnabled(),
                         []() { ZectrixSetNfcEnabled(!ZectrixGetNfcEnabled()); }});
        items.push_back({"重启", "执行", nullptr, rawdraw::SettingsItemType::Action, false,
                         []() { esp_restart(); }});
        items.push_back({"相册", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"轮播间隔", FormatMinutesLabel(slideshow_interval), nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this, sr]() {
                             Settings nvs(kGalleryNamespace, true);
                             const int current = nvs.GetInt(kSlideshowIntervalKey, 5);
                             const int next = NextSlideshowInterval(current);
                             nvs.SetInt(kSlideshowIntervalKey, next);
                             if (rawdraw_ui_manager_) {
                                 rawdraw_ui_manager_->SetGallerySlideshowIntervalMinutes(next);
                             }
                             sr->UpdateItem(kSettingsSlideshowIndex, FormatMinutesLabel(next));
                         }});
        items.push_back({"天气", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"播报时间", ink::FormatBroadcastTime(ink::GetBroadcastMinute()), nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         []() {
                             ink::SetBroadcastMinute(ink::NextBroadcastMinute(ink::GetBroadcastMinute()));
                         }});
        items.push_back({"自动播报", ink::GetAutoSpeak() ? "开启" : "关闭", nullptr,
                         rawdraw::SettingsItemType::Checkbox, ink::GetAutoSpeak(),
                         []() { ink::SetAutoSpeak(!ink::GetAutoSpeak()); }});
        items.push_back({"播报声音", ink::FormatWeatherVoice(ink::GetWeatherVoice()), nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         []() {
                             ink::SetWeatherVoice(ink::NextWeatherVoice(ink::GetWeatherVoice()));
                         }});
        items.push_back({"白天更新", ink::FormatRefreshMinutes(ink::GetDayRefreshMin()), nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         []() { ink::SetDayRefreshMin(ink::NextRefreshMinutes(ink::GetDayRefreshMin())); }});
        items.push_back({"夜间更新", ink::FormatRefreshMinutes(ink::GetNightRefreshMin()), nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         []() { ink::SetNightRefreshMin(ink::NextRefreshMinutes(ink::GetNightRefreshMin())); }});
        items.push_back({"网络", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"Wi-Fi", "未连接", nullptr, rawdraw::SettingsItemType::Checkbox, false,
                         [this, sr]() {
                             auto& wifi = WifiManager::GetInstance();
                             if (wifi_connected_.load(std::memory_order_acquire) || wifi.IsConnected()) {
                                 ESP_LOGI(kTag, "Wi-Fi setting toggled OFF");
                                 if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                                     rawdraw_ui_manager_->StopLanHttpServer();
                                     UpdateHttpServerSettingsItem(sr, false);
                                 }
                                 wifi.StopStation();
                                 wifi_connected_.store(false, std::memory_order_release);
                                 UpdateWifiSettingsItem(sr, false);
                                 UpdateLanIpSettingsItem(sr, "");
                             } else {
                                 ESP_LOGI(kTag, "Wi-Fi setting toggled ON");
                                 UpdateWifiSettingsItem(sr, false, "连接中");
                                 wifi.StartStation();
                             }
                             UpdateStatusBarForUi();
                         }});
        items.push_back({"局域网服务", "已关闭", nullptr, rawdraw::SettingsItemType::Checkbox, false,
                         [this, sr]() {
                             if (!rawdraw_ui_manager_) return;
                             if (rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                                 ESP_LOGI(kTag, "LAN HTTP server toggled OFF");
                                 rawdraw_ui_manager_->StopLanHttpServer();
                                 UpdateHttpServerSettingsItem(sr, false);
                                 UpdateStatusBarForUi();
                                 return;
                             }

                             auto& wifi = WifiManager::GetInstance();
                             if (!wifi_connected_.load(std::memory_order_acquire) && !wifi.IsConnected()) {
                                 ESP_LOGW(kTag, "LAN HTTP server requires WiFi connection");
                                 UpdateHttpServerSettingsItem(sr, false, "需先连接WiFi");
                                 UpdateStatusBarForUi();
                                 return;
                             }
                             const std::string ip = wifi.GetIpAddress();
                             if (ip.empty()) {
                                 ESP_LOGW(kTag, "LAN HTTP server requires station IP");
                                 UpdateHttpServerSettingsItem(sr, false, "等待IP");
                                 UpdateStatusBarForUi();
                                 return;
                             }
                             const bool started = rawdraw_ui_manager_->StartLanHttpServer(ip);
                             ESP_LOGI(kTag, "LAN HTTP server toggled ON: started=%d url=http://%s/",
                                      started ? 1 : 0, ip.c_str());
                             UpdateHttpServerSettingsItem(sr, started, started ? ip : "");
                             UpdateLanIpSettingsItem(sr, started ? ip : WifiManager::GetInstance().GetIpAddress());
                             UpdateStatusBarForUi();
                         }});
        items.push_back({"局域网IP", "未获取", nullptr, rawdraw::SettingsItemType::Normal, false});
        items.push_back({"省电模式", "手动进入", nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this]() {
                             ESP_LOGI(kTag, "Manual sleep requested from settings");
                             EnterManualSleep();
                         }});
        items.push_back({"关于", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"固件", PROJECT_VER, nullptr, rawdraw::SettingsItemType::Normal, false});
        sr->SetItems(items);
        if (from_deep_sleep &&
            rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Settings &&
            s_settings_selected_label[0] != '\0') {
            sr->SelectLabel(s_settings_selected_label);
        }
        sr->SetFirmwareVersion("v" PROJECT_VER);

        uint8_t mac_bytes[6] = {};
        esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac_bytes[0], mac_bytes[1], mac_bytes[2],
                 mac_bytes[3], mac_bytes[4], mac_bytes[5]);
        sr->SetDeviceInfo(mac_str, "ESP32-S3");
    }

    SetupInkWeather();
    SetupWeatherSettingsItems();

    ESP_LOGI(kTag, "Rawdraw gallery UI initialized");
    if (woke_for_broadcast_) {
        ESP_LOGI(kTag, "Timer wake for the daily weather announcement at %s",
                 ink::FormatBroadcastTime(ink::GetBroadcastMinute()).c_str());
    }
    if (woke_for_refresh_) {
        ESP_LOGI(kTag, "Timer wake for the scheduled weather refresh");
    }
    if (woke_for_calendar_) {
        ESP_LOGI(kTag, "Timer wake to redraw the calendar; staying offline");
        resleep_pending_.store(true);
    }
    if (button_wake) {
        ESP_LOGI(kTag, "Wake from deep sleep: connect Wi-Fi before refreshing");
        board.FlashActivityLed();
    }

    // Set up WiFi status callback to update StatusBar
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        switch (event) {
            case NetworkEvent::Connected:
                ESP_LOGI(kTag, "WiFi connected: %s", data.c_str());
                wifi_connected_.store(true, std::memory_order_release);
                Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::BALANCED);
                StartSntpClockSyncOnce();
                if (rawdraw_ui_manager_ && !rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                    const std::string ip = data.empty() ? WifiManager::GetInstance().GetIpAddress() : data;
                    if (!ip.empty()) {
                        const bool started = rawdraw_ui_manager_->StartLanHttpServer(ip);
                        ESP_LOGI(kTag, "LAN HTTP server auto-start after WiFi: started=%d url=http://%s/",
                                 started ? 1 : 0, ip.c_str());
                        if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
                            UpdateHttpServerSettingsItem(sr, started, started ? ip : "");
                            UpdateLanIpSettingsItem(sr, ip);
                        }
                    }
                }
                if (rawdraw_ui_manager_ &&
                    rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                    !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                    ESP_LOGI(kTag, "WiFi connected while config page is visible, returning to gallery");
                    rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
                }
                if (defer_wake_refresh_ || (woke_for_refresh_ && !refresh_link_resolved_) ||
                    broadcast_wait_link_ || woke_for_broadcast_) {
                    // The main loop decides whether to paint once the link is up.
                    UpdateStatusBarForUi(false);
                } else if (wake_refresh_cancelled_) {
                    // SwitchPage already painted the page the user opened.
                    wake_refresh_cancelled_ = false;
                    UpdateStatusBarForUi(false);
                    StartWeatherFetch(false);
                } else {
                    const bool weather_page = rawdraw_ui_manager_ &&
                        rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Weather;
                    if (weather_page && ink::QWeatherClient::IsClockValid()) {
                        const time_t now = time(nullptr);
                        struct tm local_tm = {};
                        localtime_r(&now, &local_tm);
                        const bool interval_off =
                            ink::RefreshMinAt(local_tm.tm_hour * 60 + local_tm.tm_min) <= 0;
                        if (interval_off || ink::QWeatherClient::GetInstance().IsCacheFresh()) {
                            PresentCachedWeather(false);
                        }
                    }
                    const bool fetch_started = StartWeatherFetch(false);
                    UpdateStatusBarForUi(!(fetch_started && weather_page));
                }
                break;
            case NetworkEvent::Disconnected:
                ESP_LOGI(kTag, "WiFi disconnected");
                wifi_connected_.store(false, std::memory_order_release);
                if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                    rawdraw_ui_manager_->StopLanHttpServer();
                }
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::Connecting:
            case NetworkEvent::Scanning:
                wifi_connected_.store(false, std::memory_order_release);
                // Scanning/connecting would otherwise start a 20s four-color
                // refresh with the disconnected icon, which then races the
                // later Connected frame.
                UpdateStatusBarForUi(false);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                ESP_LOGI(kTag, "WiFi config mode entered: %s", data.c_str());
                wifi_connected_.store(false, std::memory_order_release);
                if (rawdraw_ui_manager_) {
                    auto& wifi = WifiManager::GetInstance();
                    rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                            wifi.GetApPassword(),
                                                            wifi.GetApWebUrl());
                }
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::WifiConfigModeExit:
                if (rawdraw_ui_manager_ &&
                    rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                    !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                    ESP_LOGI(kTag, "WiFi config AP exited, returning to gallery without EPD refresh");
                    rawdraw_ui_manager_->SetCurrentPageWithoutRender(ui::RawDrawPageId::Gallery);
                }
                wifi_connected_.store(WifiManager::GetInstance().IsConnected(),
                                      std::memory_order_release);
                UpdateStatusBarForUi(wifi_connected_.load(std::memory_order_acquire));
                break;
            case NetworkEvent::ModemDetecting:
            case NetworkEvent::ModemErrorNoSim:
            case NetworkEvent::ModemErrorRegDenied:
            case NetworkEvent::ModemErrorInitFailed:
            case NetworkEvent::ModemErrorTimeout:
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi();
                break;
        }
    });

    // Start network (non-blocking, WiFi connects asynchronously). A calendar redraw
    // needs no network; it starts later if a button keeps the device awake.
    if (!woke_for_calendar_) {
        board.RequestNetwork();
    }

    SetDeviceState(kDeviceStateIdle);
}

void Application::OnUpClick() {
    MarkUserActivity();
    ESP_LOGI(kTag, "UP click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kUpClick});
    }
}

void Application::OnUpDoubleClick() {
    MarkUserActivity();
    ESP_LOGI(kTag, "UP double click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kUpDoubleClick});
    }
}

void Application::SetupInkWeather() {
    auto* wr = rawdraw_ui_manager_ ? rawdraw_ui_manager_->GetInkWeatherRenderer() : nullptr;
    if (!wr) return;

    auto& client = ink::QWeatherClient::GetInstance();
    if (!client.HasKeys()) {
        ESP_LOGW(kTag, "QWeather credentials missing; set CONFIG_QWEATHER_* in sdkconfig");
        wr->SetUnconfigured(true);
    }
    ink::WeatherData cached;
    if (client.LoadCache(&cached)) {
        ESP_LOGI(kTag, "Loaded cached weather for %s", cached.city.c_str());
        wr->SetData(cached);
    }
    client.SetCallback([this, wr](bool ok, const ink::WeatherData& data) {
        if (ok) {
            wr->SetData(data);
        } else {
            // Drop the stale forecast so the failure text is what gets painted.
            wr->ClearDisplayedData();
            wr->SetFailed(true);
        }
        if (rawdraw_ui_manager_ &&
            rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Weather) {
            rawdraw_ui_manager_->RequestActivePageRefresh();
        }
        OnWeatherFetched(ok, data);
    });
    wr->SetSpeakCallback([this]() {
        if (SpeakCachedWeather()) {
            page_speech_.store(true);
            return;
        }
        speak_after_fetch_.store(true);
        StartWeatherFetch(true);
    });
    wr->SetRefreshCallback([this]() {
        ink::WeatherSpeaker::GetInstance().Stop();
        speak_after_fetch_.store(false);
        page_speech_.store(false);
        StartWeatherFetch(true);
    });
    rawdraw_ui_manager_->SetPageSwitchCallback([this](ui::RawDrawPageId page) {
        if (defer_wake_refresh_) {
            ESP_LOGI(kTag, "Page changed during wake; cancelling the deferred refresh");
            defer_wake_refresh_ = false;
            weather_wait_clock_ = false;
            wake_refresh_cancelled_ = true;
        }
        if (page == ui::RawDrawPageId::Settings) {
            int level = -1;
            bool charging = false;
            bool discharging = false;
            if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
                level = -1;
            }
            if (auto* settings = rawdraw_ui_manager_->GetSettingsRenderer()) {
                settings->UpdateMeter(kSettingsBatteryIndex, level);
            }
        }
        OnWeatherPageSwitched(page == ui::RawDrawPageId::Weather);
    });
}

void Application::SetupWeatherSettingsItems() {
    ink::SetWeatherPrefsChangedCallback([this]() {
        auto* sr = rawdraw_ui_manager_ ? rawdraw_ui_manager_->GetSettingsRenderer() : nullptr;
        if (!sr) return;
        const bool auto_speak = ink::GetAutoSpeak();
        sr->UpdateItem(kSettingsBroadcastIndex, ink::FormatBroadcastTime(ink::GetBroadcastMinute()));
        sr->UpdateChecked(kSettingsAutoSpeakIndex, auto_speak);
        sr->UpdateItem(kSettingsAutoSpeakIndex, auto_speak ? "开启" : "关闭");
        sr->UpdateItem(kSettingsWeatherVoiceIndex, ink::FormatWeatherVoice(ink::GetWeatherVoice()));
        sr->UpdateItem(kSettingsDayRefreshIndex, ink::FormatRefreshMinutes(ink::GetDayRefreshMin()));
        sr->UpdateItem(kSettingsNightRefreshIndex, ink::FormatRefreshMinutes(ink::GetNightRefreshMin()));
        last_broadcast_check_minute_ = -1;
        last_refresh_check_minute_ = -1;
        rawdraw_ui_manager_->RequestActivePageRefresh();
    });
}

bool Application::StartWeatherFetch(bool force) {
    auto* wr = rawdraw_ui_manager_ ? rawdraw_ui_manager_->GetInkWeatherRenderer() : nullptr;
    if (!wifi_connected_.load(std::memory_order_acquire) && !WifiManager::GetInstance().IsConnected()) {
        if (force && wr) wr->SetFailed(true);
        return false;
    }
    auto& client = ink::QWeatherClient::GetInstance();
    if (!client.HasKeys()) return false;
    if (!ink::QWeatherClient::IsClockValid()) {
        // The JWT needs real time; the SNTP sync callback retries.
        if (wr && !wr->HasData()) wr->SetLoading(true);
        return false;
    }
    const bool started = client.RequestFetch(force);
    if (started && wr && !wr->HasData()) {
        wr->SetLoading(true);
    }
    return started;
}

void Application::OnSntpSynced() {
    sntp_synced_.store(true);
    // Wake decisions run on the main loop so they are not raced by this callback.
    if (defer_wake_refresh_ || refresh_wait_clock_ || broadcast_wait_link_ ||
        (woke_for_refresh_ && !refresh_link_resolved_)) {
        return;
    }
    StartWeatherFetch(false);
}

void Application::OnWeatherPageSwitched(bool entered) {
    if (!entered) {
        speak_after_fetch_.store(false);
        if (page_speech_.exchange(false)) {
            ink::WeatherSpeaker::GetInstance().Stop();
        }
        return;
    }
    const bool started = StartWeatherFetch(false);
    auto& client = ink::QWeatherClient::GetInstance();
    if (!started && !client.IsFetching() && ink::QWeatherClient::IsClockValid()) {
        const time_t now = time(nullptr);
        struct tm local_tm = {};
        localtime_r(&now, &local_tm);
        const bool interval_off = ink::RefreshMinAt(local_tm.tm_hour * 60 + local_tm.tm_min) <= 0;
        // SwitchPage renders after this callback. Drop an expired forecast when
        // this period does not fetch; keep a fresh cache on screen.
        if (interval_off || client.IsCacheFresh()) {
            PresentCachedWeather(false);
        }
    }
    if (!ink::GetAutoSpeak()) return;
    auto* wr = rawdraw_ui_manager_ ? rawdraw_ui_manager_->GetInkWeatherRenderer() : nullptr;
    const bool has_data = wr && wr->HasData();
    if (started || ink::QWeatherClient::GetInstance().IsFetching() || !has_data) {
        // Speak once the fetch lands (or a later one, if Wi-Fi/time is not ready yet).
        speak_after_fetch_.store(true);
        return;
    }
    if (SpeakCachedWeather()) page_speech_.store(true);
}

void Application::OnWeatherFetched(bool ok, const ink::WeatherData& data) {
    const bool on_page = rawdraw_ui_manager_ &&
                         rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Weather;
    if (woke_for_refresh_) {
        ESP_LOGI(kTag, "Scheduled weather refresh after timer wake: %s", ok ? "ok" : "failed");
        woke_for_refresh_ = false;
        refresh_link_resolved_ = true;
        if (rawdraw_ui_manager_ && !on_page) rawdraw_ui_manager_->RequestActivePageRefresh();
        resleep_pending_.store(true);
    }
    const bool broadcast = broadcast_pending_.exchange(false);
    if (broadcast) {
        if (ok) {
            ESP_LOGI(kTag, "Daily weather announcement: fresh data");
            if (rawdraw_ui_manager_ && !on_page) rawdraw_ui_manager_->RequestActivePageRefresh();
            SpeakWeather(data);
        } else {
            ESP_LOGW(kTag, "Daily weather announcement: fetch failed, not speaking stale cache");
        }
        FinishBroadcast();
        return;
    }
    const bool page_speak = speak_after_fetch_.exchange(false) && on_page;
    if (!page_speak) return;

    const bool spoken = ok ? SpeakWeather(data) : SpeakCachedWeather();
    page_speech_.store(spoken);
}

bool Application::SpeakWeather(const ink::WeatherData& data) {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr && codec->output_volume() <= 0) {
        ESP_LOGI(kTag, "Volume is 0; skipping weather speech");
        return false;
    }
    const std::string text = ink::BuildWeatherSpeech(data);
    if (text.empty()) return false;
    ink::WeatherSpeaker::GetInstance().Speak(text);
    return true;
}

bool Application::SpeakCachedWeather() {
    ink::WeatherData cached;
    if (!ink::QWeatherClient::GetInstance().LoadCache(&cached)) return false;
    return SpeakWeather(cached);
}

void Application::CheckDailyBroadcast() {
    const int64_t now_us = esp_timer_get_time();
    if (broadcast_pending_.load() && now_us - broadcast_started_us_ > kBroadcastFallbackUs &&
        !ink::QWeatherClient::GetInstance().IsFetching()) {
        broadcast_pending_.store(false);
        if (ink::QWeatherClient::GetInstance().IsCacheFresh()) {
            ESP_LOGW(kTag, "Daily weather announcement: fetch did not finish, speaking fresh cache");
            page_speech_.store(false);
            SpeakCachedWeather();
        } else {
            ESP_LOGW(kTag, "Daily weather announcement: fetch did not finish, not speaking stale cache");
        }
        FinishBroadcast();
    }

    if (user_active_.load() &&
        (woke_for_broadcast_ || (woke_for_refresh_ && refresh_link_resolved_) ||
         woke_for_calendar_ || resleep_pending_.load())) {
        ESP_LOGI(kTag, "Button pressed after timer wake; staying awake");
        woke_for_broadcast_ = false;
        woke_for_refresh_ = false;
        resleep_pending_.store(false);
        if (woke_for_calendar_) {
            woke_for_calendar_ = false;
            Board::GetInstance().RequestNetwork();
        }
    }
    if (resleep_pending_.load()) {
        const bool busy = ink::WeatherSpeaker::GetInstance().IsSpeaking() || !audio_service_.IsIdle() ||
                          ink::QWeatherClient::GetInstance().IsFetching() ||
                          broadcast_pending_.load() ||
                          (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsDisplayRefreshPending());
        if (busy) {
            resleep_ready_us_ = 0;
        } else if (resleep_ready_us_ == 0) {
            resleep_ready_us_ = now_us;
        } else if (now_us - resleep_ready_us_ > kResleepGraceUs) {
            ESP_LOGI(kTag, "Announcement done after timer wake; back to sleep");
            resleep_pending_.store(false);
            EnterManualSleep();
            return;
        }
    }

    if (!ink::QWeatherClient::IsClockValid()) return;
    // After a timer wake the RTC may have drifted; wait for SNTP before deciding.
    if (woke_for_broadcast_ && !sntp_synced_.load() && now_us < kWakeDecisionTimeoutUs) return;

    const time_t now = time(nullptr);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    const int now_minute = local_tm.tm_hour * 60 + local_tm.tm_min;
    if (now_minute == last_broadcast_check_minute_ && !woke_for_broadcast_) return;
    last_broadcast_check_minute_ = now_minute;

    const int target = ink::GetBroadcastMinute();
    const int today = LocalDateKey(local_tm);
    if (target != ink::kBroadcastOff && ink::GetLastBroadcastDate() != today &&
        now_minute >= target && now_minute < target + kBroadcastLateWindowMin) {
        TriggerDailyBroadcast(today);
        return;
    }
    if (!woke_for_broadcast_ || broadcast_pending_.load() || resleep_pending_.load()) return;

    const int minutes_left = target == ink::kBroadcastOff ? -1 : target - now_minute;
    if (minutes_left > 0 && minutes_left <= kBroadcastWaitAwakeMin) return;
    // Woke too early (RTC drift) or the time changed: sleep again until the target.
    ESP_LOGI(kTag, "Timer wake: announcement not due (%d min left); back to sleep", minutes_left);
    woke_for_broadcast_ = false;
    resleep_pending_.store(true);
}

void Application::TriggerDailyBroadcast(int today) {
    ESP_LOGI(kTag, "Daily weather announcement due (%d)", today);
    ink::SetLastBroadcastDate(today);
    speak_after_fetch_.store(false);
    page_speech_.store(false);
    broadcast_wait_link_ = true;
    TryFinishBroadcastLink();
}

void Application::FinishBroadcast() {
    broadcast_wait_link_ = false;
    if (woke_for_broadcast_) {
        woke_for_broadcast_ = false;
        resleep_pending_.store(true);
    }
}

bool Application::WakeLinkUp() const {
    return wifi_connected_.load(std::memory_order_acquire) || WifiManager::GetInstance().IsConnected();
}

bool Application::PresentCachedWeather(bool request_refresh) {
    auto* wr = rawdraw_ui_manager_ ? rawdraw_ui_manager_->GetInkWeatherRenderer() : nullptr;
    if (!wr) return true;
    if (!ink::QWeatherClient::IsClockValid()) return false;

    auto& client = ink::QWeatherClient::GetInstance();
    if (client.IsCacheFresh()) {
        ink::WeatherData cached;
        if (client.LoadCache(&cached)) {
            wr->SetData(cached);
        } else {
            wr->ClearDisplayedData();
        }
    } else {
        wr->ClearDisplayedData();
    }
    if (request_refresh && rawdraw_ui_manager_) {
        rawdraw_ui_manager_->RequestActivePageRefresh();
    }
    return true;
}

void Application::ResolveButtonWake(bool connected) {
    if (!defer_wake_refresh_) return;
    if (!rawdraw_ui_manager_) {
        defer_wake_refresh_ = false;
        weather_wait_clock_ = false;
        return;
    }
    if (rawdraw_ui_manager_->GetCurrentPage() != ui::RawDrawPageId::Weather) {
        defer_wake_refresh_ = false;
        weather_wait_clock_ = false;
        UpdateStatusBarForUi(true);
        return;
    }
    if (!ink::QWeatherClient::IsClockValid()) {
        weather_wait_clock_ = true;
        return;
    }

    weather_wait_clock_ = false;
    defer_wake_refresh_ = false;
    auto& client = ink::QWeatherClient::GetInstance();
    if (client.IsFetching()) return;
    const time_t now = time(nullptr);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    const bool interval_off = ink::RefreshMinAt(local_tm.tm_hour * 60 + local_tm.tm_min) <= 0;
    const bool fresh = client.IsCacheFresh();
    if (!connected || fresh || interval_off) {
        UpdateStatusBarForUi(false);
        PresentCachedWeather(true);
        return;
    }
    if (StartWeatherFetch(false) || client.IsFetching()) return;

    if (!client.HasKeys()) {
        UpdateStatusBarForUi(false);
        PresentCachedWeather(true);
        return;
    }
    auto* wr = rawdraw_ui_manager_->GetInkWeatherRenderer();
    if (wr) wr->SetFailed(true);
    UpdateStatusBarForUi(false);
    rawdraw_ui_manager_->RequestActivePageRefresh();
}

void Application::ResolveRefreshWake(bool connected) {
    if (refresh_link_resolved_) return;
    if (!ink::QWeatherClient::IsClockValid()) {
        refresh_wait_clock_ = true;
        return;
    }

    refresh_wait_clock_ = false;
    refresh_link_resolved_ = true;
    auto& client = ink::QWeatherClient::GetInstance();
    if (client.IsFetching()) return;
    const time_t now = time(nullptr);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    const bool interval_off = ink::RefreshMinAt(local_tm.tm_hour * 60 + local_tm.tm_min) <= 0;
    const bool fresh = client.IsCacheFresh();
    if (!connected || fresh || interval_off) {
        UpdateStatusBarForUi(false);
        PresentCachedWeather(true);
        woke_for_refresh_ = false;
        resleep_pending_.store(true);
        return;
    }
    if (StartWeatherFetch(false) || client.IsFetching()) return;

    if (!client.HasKeys()) {
        UpdateStatusBarForUi(false);
        PresentCachedWeather(true);
    } else {
        auto* wr = rawdraw_ui_manager_ ? rawdraw_ui_manager_->GetInkWeatherRenderer() : nullptr;
        if (wr) wr->SetFailed(true);
        UpdateStatusBarForUi(false);
        if (rawdraw_ui_manager_) rawdraw_ui_manager_->RequestActivePageRefresh();
    }
    woke_for_refresh_ = false;
    resleep_pending_.store(true);
}

void Application::DecideBroadcast(bool connected) {
    if (!ink::QWeatherClient::IsClockValid()) {
        broadcast_wait_link_ = true;
        return;
    }
    broadcast_wait_link_ = false;
    auto& client = ink::QWeatherClient::GetInstance();
    if (client.IsFetching()) {
        broadcast_pending_.store(true);
        broadcast_started_us_ = esp_timer_get_time();
        return;
    }
    if (client.IsCacheFresh()) {
        ESP_LOGI(kTag, "Daily weather announcement: cache still fresh");
        SpeakCachedWeather();
        FinishBroadcast();
        return;
    }
    if (!connected) {
        ESP_LOGW(kTag, "Daily weather announcement: offline and cache expired");
        if (rawdraw_ui_manager_ &&
            rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Weather) {
            UpdateStatusBarForUi(false);
            PresentCachedWeather(true);
        }
        FinishBroadcast();
        return;
    }

    broadcast_pending_.store(true);
    broadcast_started_us_ = esp_timer_get_time();
    if (StartWeatherFetch(true)) return;

    broadcast_pending_.store(false);
    ESP_LOGW(kTag, "Daily weather announcement: fetch did not start");
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Weather) {
        auto* wr = rawdraw_ui_manager_->GetInkWeatherRenderer();
        if (wr && client.HasKeys()) wr->SetFailed(true);
        rawdraw_ui_manager_->RequestActivePageRefresh();
    }
    FinishBroadcast();
}

void Application::TryFinishBroadcastLink() {
    if (!broadcast_wait_link_) return;
    if (!ink::QWeatherClient::IsClockValid()) return;
    const bool connected = WakeLinkUp();
    if (!connected && esp_timer_get_time() < kWakeConnectTimeoutUs) return;
    DecideBroadcast(connected);
}

void Application::CheckWakeLink() {
    bool idle = false;
    if (!wake_resolve_busy_.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
        return;
    }
    const bool connected = WakeLinkUp();
    const bool timed_out = esp_timer_get_time() >= kWakeConnectTimeoutUs;
    if (defer_wake_refresh_ && (connected || timed_out)) {
        ResolveButtonWake(connected);
    }
    if (woke_for_refresh_ && !refresh_link_resolved_ && (connected || timed_out)) {
        ResolveRefreshWake(connected);
    }
    TryFinishBroadcastLink();
    wake_resolve_busy_.store(false, std::memory_order_release);
}

uint32_t Application::ArmTimerWakeup(uint32_t* programmed_s) {
    if (programmed_s) *programmed_s = 0;
    s_timer_wake_reason = kTimerWakeNone;
    if (!ink::QWeatherClient::IsClockValid()) return 0;
    const time_t now = time(nullptr);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    const int now_sec = local_tm.tm_hour * 3600 + local_tm.tm_min * 60 + local_tm.tm_sec;

    int broadcast_wake = -1;
    const int target = ink::GetBroadcastMinute();
    if (target != ink::kBroadcastOff) {
        const int seconds = ink::SecondsUntilMinuteOfDay(local_tm.tm_hour, local_tm.tm_min,
                                                        local_tm.tm_sec, target);
        // A 20 s lead lets CheckDailyBroadcast() wait or sleep again for the remainder.
        broadcast_wake = seconds - 20;
        if (broadcast_wake < 1) broadcast_wake = 1;
    }

    // Page-specific wake: weather and calendar step toward :00/:30 or hour marks;
    // other pages only keep the announcement.
    const auto page = rawdraw_ui_manager_ ? rawdraw_ui_manager_->GetCurrentPage()
                                          : ui::RawDrawPageId::Gallery;
    int page_wake = -1;
    int page_reason = kTimerWakeNone;
    if (page == ui::RawDrawPageId::Weather) {
        page_wake = ink::SecondsUntilNextRefresh(now_sec,
                                                 ink::QWeatherClient::GetInstance().CacheAgeSec());
        if (page_wake == 0) {
            const int interval = ink::RefreshMinAt(now_sec / 60);
            page_wake = ink::SecondsUntilSteppedAlign(now_sec, interval);
        }
        page_reason = kTimerWakeRefresh;
    } else if (page == ui::RawDrawPageId::Calendar) {
        constexpr int kCalendarIntervalMin = kCalendarWakeSec / 60;
        const int stepped = ink::SecondsUntilSteppedAlign(now_sec, kCalendarIntervalMin);
        const int to_midnight = 24 * 3600 - now_sec + 30;
        page_wake = (stepped > 0 && stepped <= to_midnight) ? stepped : to_midnight;
        if (page_wake < 60) page_wake = 60;
        page_reason = kTimerWakeCalendar;
    }

    if (broadcast_wake < 0 && page_wake < 0) {
        ESP_LOGI(kTag, "No timer wake for page %s; BOOT wakes device",
                 ui::RawDrawUiManager::GetPageTitle(page));
        return 0;
    }
    const bool use_page = page_wake >= 0 && (broadcast_wake < 0 || page_wake < broadcast_wake);
    const int wall_s = use_page ? page_wake : broadcast_wake;
    s_timer_wake_reason = use_page ? page_reason : kTimerWakeBroadcast;
    const uint32_t rc_s = ink::PowerDiag::CompensateSleepSec(static_cast<uint32_t>(wall_s));
    if (programmed_s) *programmed_s = rc_s;
    if (rc_s > 0) {
        esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(rc_s) * 1000000ULL);
    }
    const float scale = ink::PowerDiag::RcSleepScale();
    if (!use_page) {
        ESP_LOGI(kTag, "Timer wake armed in %d s wall / %" PRIu32 " s RC (scale %.4f) for the %s announcement",
                 wall_s, rc_s, scale, ink::FormatBroadcastTime(target).c_str());
    } else if (page_reason == kTimerWakeRefresh) {
        ESP_LOGI(kTag, "Timer wake armed in %d s wall / %" PRIu32 " s RC (scale %.4f) for the scheduled weather refresh",
                 wall_s, rc_s, scale);
    } else {
        ESP_LOGI(kTag, "Timer wake armed in %d s wall / %" PRIu32 " s RC (scale %.4f) to redraw the calendar",
                 wall_s, rc_s, scale);
    }
    return static_cast<uint32_t>(wall_s);
}

void Application::CheckWeatherRefresh() {
    if (!ink::QWeatherClient::IsClockValid()) return;
    auto& client = ink::QWeatherClient::GetInstance();
    if (client.IsFetching()) return;
    const time_t now = time(nullptr);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    const int now_minute = local_tm.tm_hour * 60 + local_tm.tm_min;
    if (now_minute == last_refresh_check_minute_) return;
    last_refresh_check_minute_ = now_minute;
    const int now_sec = local_tm.tm_hour * 3600 + local_tm.tm_min * 60 + local_tm.tm_sec;
    if (ink::SecondsUntilNextRefresh(now_sec, client.CacheAgeSec()) != 0) return;
    if (StartWeatherFetch(false)) {
        ESP_LOGI(kTag, "Scheduled weather refresh (%s interval)",
                 ink::FormatRefreshMinutes(ink::RefreshMinAt(now_minute)).c_str());
    }
}

void Application::MarkUserActivity() {
    user_active_.store(true);
    last_activity_us_.store(esp_timer_get_time());
}

void Application::OnDownClick() {
    MarkUserActivity();
    ESP_LOGI(kTag, "DOWN click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kDownClick});
    }
}

bool Application::AdjustSettingsSlider(int delta, bool* changed, bool request_refresh) {
    if (changed) *changed = false;
    if (!rawdraw_ui_manager_ ||
        rawdraw_ui_manager_->GetCurrentPage() != ui::RawDrawPageId::Settings) {
        return false;
    }
    auto* sr = rawdraw_ui_manager_->GetSettingsRenderer();
    if (!sr || !sr->AdjustSelectedSlider(delta, changed)) return false;
    if (request_refresh && (!changed || *changed)) {
        rawdraw_ui_manager_->RequestActivePageRefresh();
    }
    return true;
}

void Application::SetUpKeyHeld(bool held) {
    up_key_held_.store(held, std::memory_order_release);
    if (!held && volume_repeat_delta_.load(std::memory_order_acquire) > 0) {
        volume_repeat_cancel_.store(true, std::memory_order_release);
    }
    if (held && down_key_held_.load(std::memory_order_acquire) &&
        volume_repeat_active_.load(std::memory_order_acquire)) {
        volume_repeat_cancel_.store(true, std::memory_order_release);
    }
}

void Application::SetDownKeyHeld(bool held) {
    down_key_held_.store(held, std::memory_order_release);
    if (!held && volume_repeat_delta_.load(std::memory_order_acquire) < 0) {
        volume_repeat_cancel_.store(true, std::memory_order_release);
    }
    if (held && up_key_held_.load(std::memory_order_acquire) &&
        volume_repeat_active_.load(std::memory_order_acquire)) {
        volume_repeat_cancel_.store(true, std::memory_order_release);
    }
}

bool Application::VolumeRepeatStillHeld() const {
    if (volume_repeat_cancel_.load(std::memory_order_acquire)) return false;
    if (up_key_held_.load(std::memory_order_acquire) &&
        down_key_held_.load(std::memory_order_acquire)) {
        return false;
    }
    const int delta = volume_repeat_delta_.load(std::memory_order_acquire);
    if (delta > 0) return up_key_held_.load(std::memory_order_acquire);
    if (delta < 0) return down_key_held_.load(std::memory_order_acquire);
    return false;
}

bool Application::BeginVolumeRepeat(int delta) {
    auto* sr = rawdraw_ui_manager_ ? rawdraw_ui_manager_->GetSettingsRenderer() : nullptr;
    if (!sr || rawdraw_ui_manager_->GetCurrentPage() != ui::RawDrawPageId::Settings ||
        !sr->SelectedIsSlider()) {
        return false;
    }
    volume_repeat_delta_.store(delta, std::memory_order_release);
    volume_repeat_cancel_.store(false, std::memory_order_release);
    volume_repeat_active_.store(true, std::memory_order_release);
    volume_step_wait_refresh_.store(false, std::memory_order_release);
    if (rawdraw_ui_manager_->IsDisplayRefreshPending()) {
        volume_step_defer_start_.store(true, std::memory_order_release);
        ESP_LOGI(kTag, "Volume repeat waits for the current frame");
        return true;
    }
    volume_step_defer_start_.store(false, std::memory_order_release);
    RunVolumeStep();
    return true;
}

void Application::RunVolumeStep() {
    if (!volume_repeat_active_.load(std::memory_order_acquire)) return;
    volume_step_gates_.store(0, std::memory_order_release);
    bool changed = false;
    if (!AdjustSettingsSlider(volume_repeat_delta_.load(std::memory_order_acquire), &changed, false)) {
        volume_repeat_active_.store(false, std::memory_order_release);
        return;
    }
    if (!changed) {
        volume_repeat_active_.store(false, std::memory_order_release);
        ESP_LOGI(kTag, "Volume repeat stopped at the limit");
        return;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    const int volume = codec ? codec->output_volume() : 0;
    ESP_LOGI(kTag, "Volume step %d", volume);
    volume_step_wait_refresh_.store(true, std::memory_order_release);
    rawdraw_ui_manager_->RefreshShownPage();
    PlayVolumePreview(volume, [this]() { OnVolumePreviewDone(); });
}

void Application::OnVolumeRepeatRefreshIdle() {
    if (volume_step_defer_start_.exchange(false, std::memory_order_acq_rel)) {
        if (volume_repeat_active_.load(std::memory_order_acquire) && VolumeRepeatStillHeld()) {
            RunVolumeStep();
        } else {
            volume_repeat_active_.store(false, std::memory_order_release);
        }
        return;
    }
    if (!volume_step_wait_refresh_.exchange(false, std::memory_order_acq_rel)) return;
    NoteVolumeStepGate(1);
}

void Application::OnVolumePreviewDone() {
    NoteVolumeStepGate(2);
}

void Application::NoteVolumeStepGate(int bit) {
    const int prev = volume_step_gates_.fetch_or(bit, std::memory_order_acq_rel);
    if ((prev & bit) != 0) return;
    if ((prev | bit) == 3) {
        TryContinueVolumeStep();
    }
}

void Application::TryContinueVolumeStep() {
    if (!volume_repeat_active_.load(std::memory_order_acquire) || !VolumeRepeatStillHeld()) {
        volume_repeat_active_.store(false, std::memory_order_release);
        return;
    }
    RunVolumeStep();
}

void Application::PlayVolumePreview(int volume, std::function<void()> on_done) {
    if (volume <= 0) {
        if (on_done) on_done();
        return;
    }
    constexpr int kSampleRate = AUDIO_OUTPUT_SAMPLE_RATE;
    constexpr int kDurationMs = 150;
    constexpr int kSamples = kSampleRate * kDurationMs / 1000;
    constexpr int kFadeSamples = kSampleRate * 10 / 1000;
    constexpr float kToneHz = 880.0f;
    std::vector<int16_t> pcm(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        float amp = 12000.0f;
        if (i < kFadeSamples) amp *= static_cast<float>(i) / kFadeSamples;
        if (kSamples - i < kFadeSamples) amp *= static_cast<float>(kSamples - i) / kFadeSamples;
        pcm[i] = static_cast<int16_t>(amp * sinf(2.0f * static_cast<float>(M_PI) * kToneHz * i / kSampleRate));
    }
    audio_service_.UnmuteOutput();
    audio_service_.PlayPcm(std::move(pcm), std::move(on_done));
}

void Application::OnUpLongPress() {
    ESP_LOGI(kTag, "UP long press");
    MarkUserActivity();
    Board::GetInstance().FlashActivityLed();
    if (!rawdraw_ui_manager_) return;
    if (BeginVolumeRepeat(rawdraw::SettingsRenderer::kSliderLongPressStep)) return;
    rawdraw_ui_manager_->RequestActivePageRefresh();
    const auto page = rawdraw_ui_manager_->GetCurrentPage();
    if (page == ui::RawDrawPageId::Settings || page == ui::RawDrawPageId::Weather ||
        page == ui::RawDrawPageId::Calendar) {
        ESP_LOGI(kTag, "UP long press - returning to gallery");
        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
    }
}

void Application::OnDownLongPress() {
    ESP_LOGI(kTag, "DOWN long press");
    MarkUserActivity();
    Board::GetInstance().FlashActivityLed();
    if (BeginVolumeRepeat(-rawdraw::SettingsRenderer::kSliderLongPressStep)) return;
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->RequestActivePageRefresh();
    }
    if (rawdraw_ui_manager_) {
        ESP_LOGI(kTag, "DOWN long press - entering settings");
        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Settings);
    }
}

void Application::OnWifiConfigComboLongPress() {
    ESP_LOGI(kTag, "UP+DOWN long press");
    NoteButtonActivity();
    EnterWifiConfigMode();
}

void Application::OnBootClick() {
    MarkUserActivity();
    ESP_LOGI(kTag, "BOOT click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootClick});
    }
}

void Application::OnBootDoubleClick() {
    MarkUserActivity();
    ESP_LOGI(kTag, "BOOT double click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootDoubleClick});
    }
}

void Application::OnBootLongPress() {
    ESP_LOGI(kTag, "BOOT long press");
    NoteButtonActivity();
    if (WifiManager::GetInstance().IsConfigMode()) {
        ESP_LOGI(kTag, "BOOT long press - exiting WiFi config AP");
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
        }
        WifiManager::GetInstance().StartStation();
        return;
    }
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootLongPress});
    }
}

void Application::NoteButtonActivity() {
    MarkUserActivity();
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->RequestActivePageRefresh();
    }
}

void Application::EnterWifiConfigMode() {
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
        rawdraw_ui_manager_->StopLanHttpServer();
    }
    wifi_connected_.store(false, std::memory_order_release);
    ESP_LOGI(kTag, "Entering WiFi config mode by long press");
    WifiManager::GetInstance().StartConfigAp();
    if (rawdraw_ui_manager_ && WifiManager::GetInstance().IsConfigMode()) {
        auto& wifi = WifiManager::GetInstance();
        rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                wifi.GetApPassword(),
                                                wifi.GetApWebUrl());
    }
    UpdateStatusBarForUi();
}

bool Application::IsSleepBlockedByPage() const {
    if (!rawdraw_ui_manager_) return false;
    if (WifiManager::GetInstance().IsConfigMode()) return true;
    if (rawdraw_ui_manager_->IsApTransferModeRunning()) return true;
    const auto page = rawdraw_ui_manager_->GetCurrentPage();
    if (page == ui::RawDrawPageId::APTransfer) return true;
    if (page == ui::RawDrawPageId::Gallery || page == ui::RawDrawPageId::PhotoDetail) {
        return rawdraw_ui_manager_->GetGallerySlideshowIntervalMinutes() > 0;
    }
    return false;
}

bool Application::IsBusyForSleep() {
    return ink::QWeatherClient::GetInstance().IsFetching() ||
           ink::WeatherSpeaker::GetInstance().IsSpeaking() ||
           broadcast_pending_.load() || !audio_service_.IsIdle() ||
           (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsDisplayRefreshPending());
}

void Application::CheckIdleSleep() {
    // Timer-wake flows handle their own resleep.
    if (resleep_pending_.load() || woke_for_refresh_ || woke_for_broadcast_) return;
    if (IsSleepBlockedByPage()) return;
    const int64_t idle_us = esp_timer_get_time() - last_activity_us_.load();
    if (idle_us < kIdleSleepUs) return;
    if (IsBusyForSleep() && idle_us < kIdleSleepUs + kIdleBusyMaxDeferUs) return;
    ESP_LOGI(kTag, "No input for %lld s; entering deep sleep", static_cast<long long>(idle_us / 1000000));
    EnterManualSleep();
}

void Application::WaitForDisplayRefreshIdle() {
    constexpr uint32_t kTimeoutMs = 60000;
    if (!rawdraw_ui_manager_ || !rawdraw_ui_manager_->IsDisplayRefreshPending()) {
        return;
    }
    ESP_LOGI(kTag, "Waiting for EPD refresh to finish before deep sleep");
    const int64_t t0 = esp_timer_get_time();
    const bool idle = rawdraw_ui_manager_->WaitForDisplayRefreshIdle(kTimeoutMs);
    const int waited_ms = static_cast<int>((esp_timer_get_time() - t0) / 1000);
    if (!idle || rawdraw_ui_manager_->IsDisplayRefreshPending()) {
        ESP_LOGW(kTag, "EPD still busy after %d ms; sleeping anyway", waited_ms);
    } else {
        ESP_LOGI(kTag, "EPD idle after %d ms", waited_ms);
    }
}

void Application::EnterManualSleep() {
    ESP_LOGI(kTag, "Entering deep sleep; stopping local services and WiFi");
    ink::WeatherSpeaker::GetInstance().Stop();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->BeginSleepTeardown();
    }
    s_sleep_page = rawdraw_ui_manager_ ? static_cast<int>(rawdraw_ui_manager_->GetCurrentPage())
                                       : static_cast<int>(ui::RawDrawPageId::Gallery);
    s_settings_selected_label[0] = '\0';
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Settings) {
        if (auto* settings = rawdraw_ui_manager_->GetSettingsRenderer()) {
            const std::string label = settings->GetSelectedLabel();
            snprintf(s_settings_selected_label, sizeof(s_settings_selected_label), "%s",
                     label.c_str());
        }
    }
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsHttpServerRunning()) {
        rawdraw_ui_manager_->StopHttpServersForSleep();
    }
    wifi_connected_.store(false, std::memory_order_release);
    esp_wifi_disconnect();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(300));
    WaitForDisplayRefreshIdle();
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO), 0);
    uint32_t programmed_s = 0;
    const uint32_t wall_s = ArmTimerWakeup(&programmed_s);
    ink::PowerDiag::BeforeDeepSleep(wall_s, programmed_s);
    Board::GetInstance().PrepareForDeepSleep();
    esp_deep_sleep_start();
}

void Application::Run() {
    while (true) {
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->PumpClockRefresh();
        }
        CheckWakeLink();
        CheckDailyBroadcast();
        CheckWeatherRefresh();
        CheckIdleSleep();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool Application::SetDeviceState(DeviceState state) {
    const DeviceState old_state = state_.exchange(state, std::memory_order_acq_rel);
    ESP_LOGI(kTag, "State %d -> %d", old_state, state);
    return true;
}

void Application::Schedule(std::function<void()>&& callback) {
    if (callback) {
        callback();
    }
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::PlaySound(const std::string_view& sound, int duration_ms) {
    audio_service_.PlaySound(sound, duration_ms);
}

void Application::MuteSound() {
    audio_service_.MuteOutput();
}

void Application::StopSound() {
    audio_service_.ResetDecoder();
}

bool Application::CanEnterSleepMode() const {
    return false;
}

void Application::UpdateStatusBarForUi(bool request_panel_refresh) {
    auto& board = Board::GetInstance();
    int battery_level = -1;
    bool charging = false;
    bool discharging = false;
    const bool battery_ok = board.GetBatteryLevel(battery_level, charging, discharging);

    if (rawdraw_ui_manager_) {
        const bool wifi_connected = wifi_connected_.load(std::memory_order_acquire);
        const bool http_server_running = rawdraw_ui_manager_->IsHttpServerRunning();
        ui::RawDrawStatusBarData data = rawdraw_ui_manager_->GetStatusBarData();
        data.page_title = ui::RawDrawUiManager::GetPageTitle(rawdraw_ui_manager_->GetCurrentPage());
        data.wifi_connected = wifi_connected;
        data.server_connected = http_server_running;
        data.battery_level = battery_level;
        data.battery_charging = charging;
        rawdraw_ui_manager_->UpdateStatusBar(data);
        UpdateWifiSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(), wifi_connected);
        const std::string lan_ip = wifi_connected ? WifiManager::GetInstance().GetIpAddress() : "";
        UpdateLanIpSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(), lan_ip);
        UpdateHttpServerSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(),
                                     rawdraw_ui_manager_->IsLanHttpServerRunning(),
                                     rawdraw_ui_manager_->IsLanHttpServerRunning()
                                         ? lan_ip
                                         : "");
        if (auto* settings = rawdraw_ui_manager_->GetSettingsRenderer()) {
            settings->UpdateMeter(kSettingsBatteryIndex, battery_ok ? battery_level : -1);
        }
        // Quiet timer wakes refresh the panel once, when their own work is done.
        // Sleep teardown must not queue a new ~20s four-color refresh before PowerEpdOff.
        if (request_panel_refresh &&
            !rawdraw_ui_manager_->IsSleepTeardown() &&
            !defer_wake_refresh_ &&
            !woke_for_refresh_ && !woke_for_broadcast_ && !resleep_pending_.load()) {
            rawdraw_ui_manager_->RequestActivePageRefresh();
        }
    }
    return;
}
