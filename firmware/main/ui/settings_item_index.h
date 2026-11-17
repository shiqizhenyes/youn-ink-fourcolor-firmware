/**
 * @file settings_item_index.h
 * @brief Row indices of the device settings list built in Application::Initialize()
 *
 * Keep in sync with the push_back order there; other modules update rows by index.
 */

#ifndef UI_SETTINGS_ITEM_INDEX_H
#define UI_SETTINGS_ITEM_INDEX_H

namespace ui {

constexpr int kSettingsBatteryIndex = 1;
constexpr int kSettingsVolumeIndex = 2;
constexpr int kSettingsNfcIndex = 3;
constexpr int kSettingsSlideshowIndex = 6;
constexpr int kSettingsBroadcastIndex = 8;
constexpr int kSettingsAutoSpeakIndex = 9;
constexpr int kSettingsDayRefreshIndex = 10;
constexpr int kSettingsNightRefreshIndex = 11;
constexpr int kSettingsWifiIndex = 13;
constexpr int kSettingsHttpServerIndex = 14;
constexpr int kSettingsLanIpIndex = 15;

}  // namespace ui

#endif  // UI_SETTINGS_ITEM_INDEX_H
