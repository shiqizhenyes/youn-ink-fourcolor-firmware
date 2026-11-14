/**
 * @file settings_item_index.h
 * @brief Row indices of the device settings list built in Application::Initialize()
 *
 * Keep in sync with the push_back order there; other modules update rows by index.
 */

#ifndef UI_SETTINGS_ITEM_INDEX_H
#define UI_SETTINGS_ITEM_INDEX_H

namespace ui {

constexpr int kSettingsVolumeIndex = 1;
constexpr int kSettingsSlideshowIndex = 4;
constexpr int kSettingsBroadcastIndex = 6;
constexpr int kSettingsAutoSpeakIndex = 7;
constexpr int kSettingsDayRefreshIndex = 8;
constexpr int kSettingsNightRefreshIndex = 9;
constexpr int kSettingsWifiIndex = 11;
constexpr int kSettingsHttpServerIndex = 12;
constexpr int kSettingsLanIpIndex = 13;

}  // namespace ui

#endif  // UI_SETTINGS_ITEM_INDEX_H
