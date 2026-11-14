/**
 * @file qweather_client.cc
 * @brief Fetches weather from QWeather (v7 API, JWT authentication)
 */

#include "qweather_client.h"

#include "gzip_util.h"
#include "qweather_jwt.h"
#include "settings.h"
#include "weather_prefs.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <ctime>

namespace ink {

namespace {

constexpr char kTag[] = "QWeather";
constexpr char kNamespace[] = "app_state";
constexpr char kCityKey[] = "weather_city";
constexpr char kCacheKey[] = "wx_cache";
constexpr char kLocationKey[] = "wx_loc";
constexpr char kLegacyCoordKey[] = "wx_coord";
constexpr size_t kMaxResponseBytes = 8 * 1024;
constexpr size_t kMaxInflatedBytes = 16 * 1024;
constexpr int kHttpTimeoutMs = 10000;
constexpr int64_t kMinValidEpoch = 1704067200;  // 2024-01-01
// QWeather may freeze accounts that keep retrying failed requests.
constexpr int64_t kBackoffSec = 30 * 60;

esp_err_t CollectBody(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        auto* body = static_cast<std::string*>(evt->user_data);
        if (body->size() + evt->data_len <= kMaxResponseBytes) {
            body->append(static_cast<const char*>(evt->data), evt->data_len);
        }
    }
    return ESP_OK;
}

int64_t NowSeconds() {
    return static_cast<int64_t>(time(nullptr));
}

std::string BuildToken() {
    return BuildQWeatherJwt(CONFIG_QWEATHER_KEY_ID, CONFIG_QWEATHER_PROJECT_ID,
                            CONFIG_QWEATHER_DEVELOPER_ID, CONFIG_QWEATHER_PRIVATE_KEY, NowSeconds());
}

}  // namespace

QWeatherClient& QWeatherClient::GetInstance() {
    static QWeatherClient instance;
    return instance;
}

void QWeatherClient::SetCallback(WeatherFetchCallback callback) {
    callback_ = std::move(callback);
}

bool QWeatherClient::HasKeys() const {
    return CONFIG_QWEATHER_API_HOST[0] != '\0' && CONFIG_QWEATHER_KEY_ID[0] != '\0' &&
           CONFIG_QWEATHER_PROJECT_ID[0] != '\0' && CONFIG_QWEATHER_DEVELOPER_ID[0] != '\0' &&
           CONFIG_QWEATHER_PRIVATE_KEY[0] != '\0';
}

bool QWeatherClient::IsClockValid() {
    return NowSeconds() >= kMinValidEpoch;
}

std::string QWeatherClient::GetCity() {
    Settings nvs(kNamespace);
    std::string city = nvs.GetString(kCityKey, "");
    return city.empty() ? kDefaultCity : city;
}

void QWeatherClient::SetCity(const std::string& city) {
    Settings nvs(kNamespace, true);
    nvs.SetString(kCityKey, city);
    nvs.EraseKey(kCacheKey);
    nvs.EraseKey(kLocationKey);
    cache_fetched_at_ = 0;
    backoff_until_ = 0;
    ESP_LOGI(kTag, "City set to %s", city.c_str());
}

bool QWeatherClient::LoadCache(WeatherData* out) {
    Settings nvs(kNamespace);
    const std::string text = nvs.GetString(kCacheKey, "");
    WeatherData data;
    if (!DeserializeWeather(text, &data)) return false;
    cache_fetched_at_ = data.fetched_at;
    if (out) *out = data;
    return true;
}

bool QWeatherClient::IsCacheFresh() const {
    if (cache_fetched_at_ <= 0 || !IsClockValid()) return false;
    const time_t now = time(nullptr);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    const int interval_min = RefreshMinAt(local_tm.tm_hour * 60 + local_tm.tm_min);
    const int64_t max_age = interval_min > 0 ? interval_min * 60 : kWeatherCacheMaxAgeSec;
    return CacheAgeSec() < max_age;
}

int64_t QWeatherClient::CacheAgeSec() const {
    if (cache_fetched_at_ <= 0 || !IsClockValid()) return INT64_MAX / 2;
    return NowSeconds() - cache_fetched_at_;
}

bool QWeatherClient::IsFetching() const {
    return fetching_;
}

bool QWeatherClient::RequestFetch(bool force) {
    if (fetching_) return false;
    if (!HasKeys()) {
        ESP_LOGW(kTag, "QWeather credentials not configured, skipping fetch");
        return false;
    }
    if (!IsClockValid()) {
        ESP_LOGI(kTag, "Clock not synced yet, fetch deferred until SNTP");
        return false;
    }
    if (!force && IsCacheFresh()) {
        ESP_LOGI(kTag, "Cache still fresh, skipping fetch");
        return false;
    }
    if (!force && NowSeconds() < backoff_until_) {
        ESP_LOGI(kTag, "Backing off after an API error, skipping fetch");
        return false;
    }
    fetching_ = true;
    if (xTaskCreate(FetchTask, "qweather", 10240, this, 4, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Failed to create fetch task");
        fetching_ = false;
        return false;
    }
    return true;
}

void QWeatherClient::FetchTask(void* arg) {
    auto* self = static_cast<QWeatherClient*>(arg);
    {
        Settings nvs(kNamespace, true);
        nvs.EraseKey(kLegacyCoordKey);
    }
    WeatherData data;
    bool ok = self->Fetch(&data);
    if (ok && data.city != self->GetCity()) {
        // City changed via /settings while this request was in flight.
        data = WeatherData();
        ok = self->Fetch(&data);
    }
    if (ok) {
        Settings nvs(kNamespace, true);
        nvs.SetString(kCacheKey, SerializeWeather(data));
        self->cache_fetched_at_ = data.fetched_at;
    }
    self->fetching_ = false;
    if (self->callback_) self->callback_(ok, data);
    vTaskDelete(nullptr);
}

bool QWeatherClient::Get(const std::string& path, const std::string& token, std::string* body) {
    std::string raw;
    const std::string url = std::string("https://") + CONFIG_QWEATHER_API_HOST + path;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kHttpTimeoutMs;
    config.event_handler = CollectBody;
    config.user_data = &raw;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    config.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(kTag, "esp_http_client_init failed");
        return false;
    }
    const std::string auth = "Bearer " + token;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");
    const esp_err_t err = esp_http_client_perform(client);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    // QWeather gzips every response, including error bodies.
    body->clear();
    if (IsGzip(raw)) {
        if (!Gunzip(raw, kMaxInflatedBytes, body)) {
            ESP_LOGW(kTag, "GET %.40s: bad gzip body (%u bytes, status %d)", path.c_str(),
                     static_cast<unsigned>(raw.size()), status);
            return false;
        }
    } else {
        *body = std::move(raw);
    }

    if (err != ESP_OK || status != 200) {
        // Errors look like {"error":{"status":401,"title":"Unauthorized",...}}.
        ESP_LOGW(kTag, "GET %.40s failed: err=%s status=%d body=%.200s", path.c_str(),
                 esp_err_to_name(err), status, body->c_str());
        if (status == 401 || status == 403 || status == 429) {
            backoff_until_ = NowSeconds() + kBackoffSec;
        }
        return false;
    }
    return !body->empty();
}

bool QWeatherClient::ResolveLocation(const std::string& city, const std::string& token,
                                     std::string* id) {
    Settings nvs(kNamespace, true);
    if (ParseCachedLocation(nvs.GetString(kLocationKey, ""), city, id)) return true;

    std::string body;
    if (!Get("/geo/v2/city/lookup?location=" + UrlEncode(city) + "&number=1&range=cn", token,
             &body)) {
        return false;
    }
    if (!ParseQWeatherLocation(body.c_str(), id)) {
        ESP_LOGW(kTag, "City %s not found: %.120s", city.c_str(), body.c_str());
        return false;
    }
    nvs.SetString(kLocationKey, SerializeCachedLocation(city, *id));
    ESP_LOGI(kTag, "City %s -> LocationID %s", city.c_str(), id->c_str());
    return true;
}

bool QWeatherClient::Fetch(WeatherData* out) {
    const std::string city = GetCity();
    const std::string token = BuildToken();
    if (token.empty()) {
        ESP_LOGE(kTag, "Failed to build JWT; check CONFIG_QWEATHER_PRIVATE_KEY");
        return false;
    }

    std::string location;
    if (!ResolveLocation(city, token, &location)) return false;

    const std::string query = "?location=" + location;
    std::string now_body;
    std::string daily_body;
    if (!Get("/v7/weather/now" + query, token, &now_body) ||
        !Get("/v7/weather/7d" + query, token, &daily_body)) {
        return false;
    }

    std::string indices_body;
    const bool has_indices = Get("/v7/indices/1d" + query + "&type=1%2C3%2C5", token, &indices_body);
    if (!has_indices) {
        ESP_LOGW(kTag, "Life indices unavailable, continuing without them");
    }

    if (!ParseQWeather(now_body.c_str(), daily_body.c_str(),
                       has_indices ? indices_body.c_str() : nullptr, city, out)) {
        ESP_LOGW(kTag, "Failed to parse response (now %u bytes, daily %u bytes)",
                 static_cast<unsigned>(now_body.size()), static_cast<unsigned>(daily_body.size()));
        return false;
    }
    out->fetched_at = NowSeconds();
    backoff_until_ = 0;
    ESP_LOGI(kTag, "Weather %s: %d°C %s, %d-day forecast, life=%s", city.c_str(), out->temp,
             out->desc.c_str(), out->forecast_count, out->life.c_str());
    return true;
}

}  // namespace ink
