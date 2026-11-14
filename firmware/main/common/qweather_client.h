/**
 * @file qweather_client.h
 * @brief Fetches weather from QWeather (v7 API, JWT authentication)
 *
 * Host, credential ID, project ID, developer ID and the Ed25519 seed come
 * from CONFIG_QWEATHER_* (set in the local sdkconfig). The JWT needs a synced
 * clock. The city lives in NVS (app_state/weather_city), its LocationID in
 * wx_loc and the last successful result in wx_cache, so the page renders
 * right after a deep-sleep wake.
 */

#ifndef QWEATHER_CLIENT_H
#define QWEATHER_CLIENT_H

#include "ink_weather_data.h"

#include <cstdint>
#include <functional>
#include <string>

namespace ink {

constexpr const char* kDefaultCity = "杭州";
constexpr int kWeatherCacheMaxAgeSec = 30 * 60;

/** Called from the fetch task when a fetch finishes. */
using WeatherFetchCallback = std::function<void(bool ok, const WeatherData& data)>;

class QWeatherClient {
public:
    static QWeatherClient& GetInstance();

    void SetCallback(WeatherFetchCallback callback);

    bool HasKeys() const;
    /** @brief Whether the clock is synced well enough to sign a JWT */
    static bool IsClockValid();

    bool LoadCache(WeatherData* out);
    /** @brief Fresher than the day/night refresh interval now in effect (30 min when off) */
    bool IsCacheFresh() const;
    /** @brief Seconds since the cached data was fetched; huge when unknown */
    int64_t CacheAgeSec() const;

    /**
     * @brief Start a background fetch
     * @param force Fetch even if the cached result is still fresh or the
     *        client is backing off after an auth/rate-limit error
     * @return true if a fetch task was started; false without keys, before
     *         the clock is synced, while fetching, when the cache is fresh,
     *         or while backing off
     */
    bool RequestFetch(bool force);
    bool IsFetching() const;

    std::string GetCity();
    /** @brief Change the city; clears the cached weather and LocationID. Does not fetch. */
    void SetCity(const std::string& city);

private:
    QWeatherClient() = default;
    static void FetchTask(void* arg);
    bool Fetch(WeatherData* out);
    bool ResolveLocation(const std::string& city, const std::string& token, std::string* id);
    bool Get(const std::string& path, const std::string& token, std::string* body);

    WeatherFetchCallback callback_;
    volatile bool fetching_ = false;
    int64_t cache_fetched_at_ = 0;
    int64_t backoff_until_ = 0;
};

}  // namespace ink

#endif  // QWEATHER_CLIENT_H
