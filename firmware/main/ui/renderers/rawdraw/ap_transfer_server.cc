/**
 * @file ap_transfer_server.cc
 * @brief WiFi AP + HTTP Server implementation for image transfer
 */

#include "ap_transfer_server.h"
#include "boards/zectrix-s3-epaper-4.2/config.h"
#include "common/qweather_client.h"
#include "common/weather_prefs.h"
#include "common/photo_storage.h"
#include "common/power_diag.h"
#include "board.h"
#include "settings.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "wifi_credentials.h"
#include "netif_config.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_err.h>
#include <esp_sleep.h>
#include <lwip/ip_addr.h>
#include <cJSON.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace rawdraw {

namespace {
static const char* kTag = "ApTransferServer";

// AP configuration
constexpr const char* kApSsid = "InkScreen-AP";
constexpr const char* kApPassword = "12345678";
constexpr const char* kApIp = "192.168.4.1";
constexpr const char* kGalleryNamespace = "gallery";
constexpr const char* kSlideshowIntervalKey = "slide_min";

// Screen dimensions
constexpr int kScreenWidth = 400;
constexpr int kScreenHeight = 300;
constexpr size_t kImage1bppSize = kScreenWidth * kScreenHeight / 8;
constexpr size_t kImage2bppSize = kScreenWidth * kScreenHeight * 2 / 8;

extern const char photos_html_start[] asm("_binary_photos_html_start");
extern const char photos_html_end[] asm("_binary_photos_html_end");
extern const char ble_web_panel_html_start[] asm("_binary_ble_web_panel_html_start");
extern const char ble_web_panel_html_end[] asm("_binary_ble_web_panel_html_end");
extern const char layout_adjuster_html_start[] asm("_binary_layout_adjuster_html_start");
extern const char layout_adjuster_html_end[] asm("_binary_layout_adjuster_html_end");

cJSON* ReadJsonBody(httpd_req_t* req) {
    if (!req || req->content_len == 0 || req->content_len > 2048) return nullptr;
    char* buf = static_cast<char*>(calloc(1, req->content_len + 1));
    if (!buf) return nullptr;
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            free(buf);
            return nullptr;
        }
        received += static_cast<size_t>(ret);
    }
    cJSON* root = cJSON_Parse(buf);
    free(buf);
    return root;
}

void CloseCurrentSession(httpd_req_t* req) {
    if (!req || !req->handle) return;
    const int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) return;
    esp_err_t err = httpd_sess_trigger_close(req->handle, sockfd);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "httpd_sess_trigger_close(%d) failed: %s",
                 sockfd, esp_err_to_name(err));
    }
}

void SendJson(httpd_req_t* req, const char* json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    CloseCurrentSession(req);
}

void CopyJsonString(cJSON* root, const char* key, char* out, size_t out_size) {
    if (!root || !key || !out || out_size == 0) return;
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(out, item->valuestring, out_size);
    }
}

struct DeferredControlRequest {
    ApTransferServer* server = nullptr;
    bool stop_wifi = false;
    bool enter_sleep = false;
};

void DeferredControlTask(void* arg) {
    auto* request = static_cast<DeferredControlRequest*>(arg);
    if (!request) {
        vTaskDelete(nullptr);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    if (request->server) {
        request->server->Stop();
    }
    if (request->stop_wifi || request->enter_sleep) {
        ESP_LOGI(kTag, "Stopping WiFi after web control request");
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    if (request->enter_sleep) {
        ESP_LOGI(kTag, "Entering deep sleep after web control request");
        esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO), 0);
        ink::PowerDiag::BeforeDeepSleep(0);
        Board::GetInstance().PrepareForDeepSleep();
        esp_deep_sleep_start();
    }
    delete request;
    vTaskDelete(nullptr);
}

void ScheduleDeferredControl(ApTransferServer* server, bool stop_wifi, bool enter_sleep) {
    auto* request = new (std::nothrow) DeferredControlRequest{};
    if (!request) {
        ESP_LOGE(kTag, "Failed to allocate deferred control request");
        return;
    }
    request->server = server;
    request->stop_wifi = stop_wifi;
    request->enter_sleep = enter_sleep;
    BaseType_t ok = xTaskCreate(&DeferredControlTask,
                                "ap_web_control",
                                4096,
                                request,
                                4,
                                nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(kTag, "Failed to create deferred control task");
        delete request;
    }
}

}  // namespace

ApTransferServer::ApTransferServer() {
    ESP_LOGI(kTag, "ApTransferServer created");
}

ApTransferServer::~ApTransferServer() {
    Stop();
    ESP_LOGI(kTag, "ApTransferServer destroyed");
}

void ApTransferServer::Start() {
    if (running_ || starting_) {
        ESP_LOGW(kTag, "Server already running");
        return;
    }

    ESP_LOGI(kTag, "Starting AP Transfer Server async");
    mode_ = TransferMode::kAp;
    starting_ = true;
    BaseType_t ok = xTaskCreate(&ApTransferServer::StartTask,
                                "ap_transfer_start",
                                16384,
                                this,
                                5,
                                &start_task_);
    if (ok != pdPASS) {
        starting_ = false;
        start_task_ = nullptr;
        mode_ = TransferMode::kNone;
        ESP_LOGE(kTag, "Failed to create AP start task");
        NotifyState(kError, "Start task failed");
    }
}

bool ApTransferServer::StartLan(const std::string& ip_address) {
    if (running_ || starting_) {
        ESP_LOGW(kTag, "Server already running");
        return true;
    }
    if (ip_address.empty()) {
        ESP_LOGW(kTag, "LAN HTTP server start skipped: empty IP address");
        NotifyState(kError, "No WiFi IP");
        return false;
    }

    mode_ = TransferMode::kLan;
    ap_ip_ = ip_address;
    ESP_LOGI(kTag, "Starting LAN HTTP server at http://%s/", ap_ip_.c_str());
    if (!StartHttpServer()) {
        running_ = false;
        mode_ = TransferMode::kNone;
        NotifyState(kError, "HTTP start failed");
        return false;
    }
    running_ = true;
    NotifyState(kApStarted, ap_ip_);
    return true;
}

void ApTransferServer::StartTask(void* arg) {
    auto* self = static_cast<ApTransferServer*>(arg);
    if (!self) {
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(kTag, "AP start task running, stack watermark=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!self->starting_) {
        ESP_LOGI(kTag, "AP start task cancelled before WiFi init");
        self->start_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (!self->StartAccessPoint()) {
        self->running_ = false;
        self->starting_ = false;
        self->start_task_ = nullptr;
        self->mode_ = TransferMode::kNone;
        self->NotifyState(kError, "AP start failed");
        vTaskDelete(nullptr);
        return;
    }
    if (!self->starting_) {
        ESP_LOGI(kTag, "AP start task cancelled after WiFi init");
        self->start_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (!self->StartHttpServer()) {
        self->running_ = false;
        self->starting_ = false;
        self->start_task_ = nullptr;
        self->mode_ = TransferMode::kNone;
        self->NotifyState(kError, "HTTP start failed");
        vTaskDelete(nullptr);
        return;
    }

    self->running_ = true;
    self->starting_ = false;
    self->start_task_ = nullptr;
    self->NotifyState(kApStarted, self->GetApIp());
    ESP_LOGI(kTag, "AP start task done, stack watermark=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    vTaskDelete(nullptr);
}

void ApTransferServer::Stop() {
    if (!running_ && !starting_ && server_ == nullptr && ap_netif_ == nullptr) return;

    ESP_LOGI(kTag, "Stopping AP Transfer Server");
    const TransferMode old_mode = mode_;
    starting_ = false;
    start_task_ = nullptr;

    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }

    if (old_mode == TransferMode::kAp) {
        ESP_LOGI(kTag, "Returning WiFi to STA mode");
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(err));
        }
        err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(kTag, "esp_wifi_connect failed after AP stop: %s", esp_err_to_name(err));
        }

        WifiManager::GetInstance().ResumeStationAfterExternalAp();
    }

    if (ap_netif_) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }

    running_ = false;
    mode_ = TransferMode::kNone;
    NotifyState(kStopped, "Server stopped");

}

bool ApTransferServer::StartAccessPoint() {
    WifiManager::GetInstance().SuspendStationForExternalAp();

    // AP mode is entered from several states: normal STA, user-disabled WiFi,
    // and after long idle/sleep. Stop any stale WiFi activity first so the AP
    // beacon is backed by a fresh driver state instead of a half-suspended STA.
    esp_err_t err = esp_wifi_scan_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(kTag, "esp_wifi_scan_stop before AP failed: %s", esp_err_to_name(err));
    }
    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(kTag, "esp_wifi_disconnect before AP failed: %s", esp_err_to_name(err));
    }
    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(kTag, "esp_wifi_stop before AP failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    if (ap_netif_) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }

    // Create AP netif
    if (!ap_netif_) {
        ap_netif_ = esp_netif_create_default_wifi_ap();
    }
    if (!ap_netif_) {
        ESP_LOGE(kTag, "Failed to create AP netif");
        return false;
    }

    // Configure IP: 192.168.4.1
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    err = esp_netif_dhcps_stop(ap_netif_);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "esp_netif_dhcps_stop failed: %s", esp_err_to_name(err));
    }
    err = esp_netif_set_ip_info(ap_netif_, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_netif_set_ip_info failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_netif_dhcps_start(ap_netif_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_netif_dhcps_start failed: %s", esp_err_to_name(err));
        return false;
    }

    // WiFi AP config
    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, kApSsid);
    wifi_config.ap.ssid_len = strlen(kApSsid);
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.channel = 1;
    strcpy((char*)wifi_config.ap.password, kApPassword);
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_LOGI(kTag, "Setting WiFi AP mode");
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_set_mode(AP) failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "Setting WiFi AP config");
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "esp_wifi_set_ps(NONE) failed: %s", esp_err_to_name(err));
    }
    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_start failed: %s", esp_err_to_name(start_err));
        return false;
    }

    // Keep the AP IP fixed and log the same address that the screen renders.
    // This avoids confusing users with any transient netif readback while Wi-Fi
    // mode is switching.
    ap_ip_ = kApIp;

    ESP_LOGI(kTag, "AP started: SSID=%s, IP=%s", kApSsid, ap_ip_.c_str());
    return true;
}

bool ApTransferServer::StartHttpServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 28;
    config.max_open_sockets = 4;
    config.recv_wait_timeout = 30;  // Large images take time
    config.send_wait_timeout = 10;
    config.lru_purge_enable = true;
    
    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start HTTP server: %s", esp_err_to_name(err));
        server_ = nullptr;
        return false;
    }

    // Register handlers
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = IndexHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &index_uri) != ESP_OK) return false;

    httpd_uri_t upload_uri = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = UploadHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &upload_uri) != ESP_OK) return false;

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = StatusHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &status_uri) != ESP_OK) return false;

    httpd_uri_t power_log_uri = {
        .uri = "/power_log",
        .method = HTTP_GET,
        .handler = PowerLogHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &power_log_uri) != ESP_OK) return false;

    httpd_uri_t settings_get_uri = {
        .uri = "/settings",
        .method = HTTP_GET,
        .handler = SettingsHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &settings_get_uri) != ESP_OK) return false;

    httpd_uri_t settings_post_uri = {
        .uri = "/settings",
        .method = HTTP_POST,
        .handler = SettingsHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &settings_post_uri) != ESP_OK) return false;

    httpd_uri_t photos_uri = {
        .uri = "/photos",
        .method = HTTP_GET,
        .handler = PhotosHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &photos_uri) != ESP_OK) return false;

    httpd_uri_t photo_get_uri = {
        .uri = "/photo",
        .method = HTTP_GET,
        .handler = PhotoHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &photo_get_uri) != ESP_OK) return false;

    httpd_uri_t photo_delete_uri = {
        .uri = "/photo",
        .method = HTTP_DELETE,
        .handler = PhotoHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &photo_delete_uri) != ESP_OK) return false;

    httpd_uri_t photo_meta_uri = {
        .uri = "/photo/meta",
        .method = HTTP_POST,
        .handler = PhotoMetaHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &photo_meta_uri) != ESP_OK) return false;

    httpd_uri_t photo_move_uri = {
        .uri = "/photos/move",
        .method = HTTP_POST,
        .handler = PhotoMoveHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &photo_move_uri) != ESP_OK) return false;

    httpd_uri_t photo_show_uri = {
        .uri = "/photo/show",
        .method = HTTP_POST,
        .handler = PhotoShowHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &photo_show_uri) != ESP_OK) return false;

    httpd_uri_t overview_uri = {
        .uri = "/system/overview",
        .method = HTTP_GET,
        .handler = OverviewHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &overview_uri) != ESP_OK) return false;

    httpd_uri_t iface_get_uri = {
        .uri = "/network/interface",
        .method = HTTP_GET,
        .handler = InterfaceHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &iface_get_uri) != ESP_OK) return false;

    httpd_uri_t iface_post_uri = {
        .uri = "/network/interface",
        .method = HTTP_POST,
        .handler = InterfaceHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &iface_post_uri) != ESP_OK) return false;

    httpd_uri_t scan_uri = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = ScanHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &scan_uri) != ESP_OK) return false;

    httpd_uri_t saved_list_uri = {
        .uri = "/saved/list",
        .method = HTTP_GET,
        .handler = SavedListHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &saved_list_uri) != ESP_OK) return false;

    httpd_uri_t saved_delete_uri = {
        .uri = "/saved/delete",
        .method = HTTP_GET,
        .handler = SavedDeleteHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &saved_delete_uri) != ESP_OK) return false;

    httpd_uri_t saved_default_uri = {
        .uri = "/saved/set_default",
        .method = HTTP_GET,
        .handler = SavedSetDefaultHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &saved_default_uri) != ESP_OK) return false;

    httpd_uri_t submit_uri = {
        .uri = "/submit",
        .method = HTTP_POST,
        .handler = SubmitWifiHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &submit_uri) != ESP_OK) return false;

    httpd_uri_t ble_uri = {
        .uri = "/developer/ble",
        .method = HTTP_GET,
        .handler = DeveloperBleHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &ble_uri) != ESP_OK) return false;

    httpd_uri_t layout_uri = {
        .uri = "/developer/layout",
        .method = HTTP_GET,
        .handler = DeveloperLayoutHandler,
        .user_ctx = this
    };
    if (httpd_register_uri_handler(server_, &layout_uri) != ESP_OK) return false;

    ESP_LOGI(kTag, "HTTP server started at http://%s/", ap_ip_.c_str());
    return true;
}

namespace {
std::function<void()> s_activity_callback;
}  // namespace

void ApTransferServer::SetActivityCallback(std::function<void()> callback) {
    s_activity_callback = std::move(callback);
}

void ApTransferServer::NotifyActivity() {
    if (s_activity_callback) s_activity_callback();
}

esp_err_t ApTransferServer::IndexHandler(httpd_req_t* req) {
    NotifyActivity();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    const size_t html_len = static_cast<size_t>(photos_html_end - photos_html_start);
    esp_err_t ret = httpd_resp_send(req, photos_html_start, html_len);
    CloseCurrentSession(req);
    return ret;
}

esp_err_t ApTransferServer::UploadHandler(httpd_req_t* req) {
    NotifyActivity();
    auto* self = static_cast<ApTransferServer*>(req->user_ctx);
    
    self->NotifyState(kReceivingImage, "Receiving image...");

    char query[96] = {};
    char format[16] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "format", format, sizeof(format));
    }
    const bool is_2bpp = strcmp(format, "bwry2bpp") == 0 || strcmp(format, "2bpp") == 0;
    const size_t expected_size = is_2bpp ? kImage2bppSize : kImage1bppSize;

    if (req->content_len != expected_size) {
        ESP_LOGW(kTag, "Invalid upload size: %u", static_cast<unsigned>(req->content_len));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        esp_err_t send_ret = httpd_resp_send(req, is_2bpp
            ? "{\"success\":false,\"error\":\"需要400x300 2bpp四色数据\"}"
            : "{\"success\":false,\"error\":\"需要400x300 1bpp数据\"}",
            HTTPD_RESP_USE_STRLEN);
        CloseCurrentSession(req);
        return send_ret == ESP_OK ? ESP_FAIL : send_ret;
    }

    auto* buf = static_cast<uint8_t*>(malloc(expected_size));
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < expected_size) {
        int ret = httpd_req_recv(req, reinterpret_cast<char*>(buf + received),
                                 expected_size - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
            return ESP_FAIL;
        }
        received += static_cast<size_t>(ret);
    }

    ESP_LOGI(kTag, "Received %u bytes", static_cast<unsigned>(received));
    self->NotifyState(kProcessingImage, "Processing...");

    PhotoInfo info = {};
    const uint32_t now = static_cast<uint32_t>(time(nullptr));
    const uint64_t ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    snprintf(info.id, sizeof(info.id), "ap%011llu",
             static_cast<unsigned long long>(ms % 100000000000ULL));
    snprintf(info.title, sizeof(info.title), is_2bpp ? "WiFi四色图片" : "WiFi黑白图片");
    snprintf(info.location, sizeof(info.location), "WiFi AP");
    snprintf(info.body, sizeof(info.body), is_2bpp ? "手机 WiFi 传图 · 2 BP 四色" : "手机 WiFi 传图 · 1 BP 黑白");
    info.width = kScreenWidth;
    info.height = kScreenHeight;
    info.file_size = expected_size;
    info.timestamp = now > 0 ? now : static_cast<uint32_t>(ms / 1000);

    if (now > 0) {
        time_t t = now;
        struct tm tm_info;
        localtime_r(&t, &tm_info);
        strftime(info.date, sizeof(info.date), "%Y-%m-%d", &tm_info);
    }

    const bool saved = photo_save(&info, buf) == 0;
    free(buf);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (saved) {
        self->NotifyState(kImageSaved, info.id);
        if (self->image_received_callback_) {
            self->image_received_callback_(info.id);
        }
        if (self->photos_changed_callback_) {
            self->photos_changed_callback_();
        }
        std::string response = std::string("{\"success\":true,\"id\":\"") + info.id + "\"}";
        esp_err_t send_ret = httpd_resp_send(req, response.c_str(), static_cast<ssize_t>(response.size()));
        if (send_ret != ESP_OK) {
            ESP_LOGW(kTag, "Upload response send failed: %s", esp_err_to_name(send_ret));
            return send_ret;
        }
        CloseCurrentSession(req);
        // Return the device screen to the connection/instructions page after a
        // successful upload. Keeping the renderer in kComplete left the panel
        // showing the saved file id and made the AP flow look stuck before the
        // next upload.
        self->NotifyState(kApStarted, self->GetApIp());
        return ESP_OK;
    }

    self->NotifyState(kError, "Save failed");
    esp_err_t send_ret = httpd_resp_send(req, "{\"success\":false,\"error\":\"保存失败\"}", HTTPD_RESP_USE_STRLEN);
    CloseCurrentSession(req);
    return send_ret == ESP_OK ? ESP_FAIL : send_ret;
}

esp_err_t ApTransferServer::StatusHandler(httpd_req_t* req) {
    auto* self = static_cast<ApTransferServer*>(req->user_ctx);
    const char* mode = "ap";
    const char* ip = kApIp;
    if (self != nullptr) {
        mode = self->mode_ == TransferMode::kLan ? "lan" : "ap";
        ip = self->ap_ip_.empty() ? kApIp : self->ap_ip_.c_str();
    }
    char response[128];
    snprintf(response, sizeof(response),
             "{\"status\":\"ready\",\"mode\":\"%s\",\"ip\":\"%s\",\"url\":\"http://%s/\"}",
             mode, ip, ip);
    SendJson(req, response);
    return ESP_OK;
}

esp_err_t ApTransferServer::PowerLogHandler(httpd_req_t* req) {
    const std::string body = ink::PowerDiag::HistoryJson();
    SendJson(req, body.c_str());
    return ESP_OK;
}

esp_err_t ApTransferServer::SettingsHandler(httpd_req_t* req) {
    NotifyActivity();
    auto* self = static_cast<ApTransferServer*>(req->user_ctx);
    Settings nvs(kGalleryNamespace, req->method == HTTP_POST);
    int interval = nvs.GetInt(kSlideshowIntervalKey, 5);
    bool close_service = false;
    bool stop_wifi = false;
    bool enter_sleep = false;

    if (req->method == HTTP_POST) {
        cJSON* root = ReadJsonBody(req);
        if (!root) {
            SendJson(req, "{\"success\":false,\"error\":\"bad_json\"}");
            return ESP_FAIL;
        }
        cJSON* item = cJSON_GetObjectItemCaseSensitive(root, "slideshow_interval");
        if (cJSON_IsNumber(item)) {
            interval = item->valueint;
            if (interval != 0 && interval != 5 && interval != 10 && interval != 30) {
                interval = 5;
            }
            nvs.SetInt(kSlideshowIntervalKey, interval);
            if (self && self->settings_changed_callback_) {
                self->settings_changed_callback_(interval);
            }
            ESP_LOGI(kTag, "AP settings updated: slideshow_interval=%d", interval);
        }
        cJSON* city_item = cJSON_GetObjectItemCaseSensitive(root, "weather_city");
        if (cJSON_IsString(city_item) && city_item->valuestring) {
            const std::string city = city_item->valuestring;
            auto& weather = ink::QWeatherClient::GetInstance();
            if (!city.empty() && city.size() <= 48 && city != weather.GetCity()) {
                weather.SetCity(city);
                weather.RequestFetch(true);
                ESP_LOGI(kTag, "AP settings updated: weather_city=%s", city.c_str());
            }
        }
        cJSON* bcast_item = cJSON_GetObjectItemCaseSensitive(root, "bcast_time");
        if (cJSON_IsString(bcast_item) && bcast_item->valuestring) {
            // "" (or anything unparsable) turns the daily announcement off.
            const int minute = ink::ParseBroadcastTime(bcast_item->valuestring);
            ink::SetBroadcastMinute(minute);
            ESP_LOGI(kTag, "AP settings updated: bcast_time=%s",
                     ink::FormatBroadcastTime(minute).c_str());
        }
        cJSON* speak_item = cJSON_GetObjectItemCaseSensitive(root, "auto_speak");
        if (cJSON_IsBool(speak_item)) {
            ink::SetAutoSpeak(cJSON_IsTrue(speak_item));
            ESP_LOGI(kTag, "AP settings updated: auto_speak=%d", cJSON_IsTrue(speak_item) ? 1 : 0);
        }
        cJSON* voice_item = cJSON_GetObjectItemCaseSensitive(root, "bcast_voice");
        if (cJSON_IsString(voice_item) && voice_item->valuestring) {
            ink::SetWeatherVoice(ink::ParseWeatherVoice(voice_item->valuestring));
            ESP_LOGI(kTag, "AP settings updated: bcast_voice=%s",
                     ink::WeatherVoiceToken(ink::GetWeatherVoice()));
        }
        cJSON* day_item = cJSON_GetObjectItemCaseSensitive(root, "day_refresh_min");
        if (cJSON_IsNumber(day_item)) {
            ink::SetDayRefreshMin(day_item->valueint);
            ESP_LOGI(kTag, "AP settings updated: day_refresh_min=%d", ink::GetDayRefreshMin());
        }
        cJSON* night_item = cJSON_GetObjectItemCaseSensitive(root, "night_refresh_min");
        if (cJSON_IsNumber(night_item)) {
            ink::SetNightRefreshMin(night_item->valueint);
            ESP_LOGI(kTag, "AP settings updated: night_refresh_min=%d", ink::GetNightRefreshMin());
        }
        cJSON* service_item = cJSON_GetObjectItemCaseSensitive(root, "service_enabled");
        if (cJSON_IsBool(service_item) && !cJSON_IsTrue(service_item)) {
            close_service = true;
        }
        cJSON* wifi_item = cJSON_GetObjectItemCaseSensitive(root, "wifi_enabled");
        if (cJSON_IsBool(wifi_item) && !cJSON_IsTrue(wifi_item)) {
            stop_wifi = true;
        }
        cJSON* sleep_item = cJSON_GetObjectItemCaseSensitive(root, "sleep");
        if (cJSON_IsBool(sleep_item) && cJSON_IsTrue(sleep_item)) {
            close_service = true;
            stop_wifi = true;
            enter_sleep = true;
        }
        cJSON_Delete(root);
    }

    const char* mode = "ap";
    const char* ip = kApIp;
    if (self != nullptr) {
        mode = self->mode_ == TransferMode::kLan ? "lan" : "ap";
        ip = self->ap_ip_.empty() ? kApIp : self->ap_ip_.c_str();
    }
    const std::string url = std::string("http://") + ip + "/";
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddNumberToObject(resp, "slideshow_interval", interval);
    cJSON_AddBoolToObject(resp, "service_running", self && self->IsRunning());
    cJSON_AddStringToObject(resp, "mode", mode);
    cJSON_AddStringToObject(resp, "ip", ip);
    cJSON_AddStringToObject(resp, "url", url.c_str());
    cJSON_AddStringToObject(resp, "weather_city", ink::QWeatherClient::GetInstance().GetCity().c_str());
    const int bcast_minute = ink::GetBroadcastMinute();
    cJSON_AddStringToObject(resp, "bcast_time",
                            bcast_minute == ink::kBroadcastOff
                                ? ""
                                : ink::FormatBroadcastTime(bcast_minute).c_str());
    cJSON_AddBoolToObject(resp, "auto_speak", ink::GetAutoSpeak());
    cJSON_AddStringToObject(resp, "bcast_voice", ink::WeatherVoiceToken(ink::GetWeatherVoice()));
    cJSON_AddNumberToObject(resp, "day_refresh_min", ink::GetDayRefreshMin());
    cJSON_AddNumberToObject(resp, "night_refresh_min", ink::GetNightRefreshMin());
    cJSON_AddBoolToObject(resp, "closing", close_service);
    cJSON_AddBoolToObject(resp, "sleep", enter_sleep);
    char* response = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    SendJson(req, response ? response : "{\"success\":false,\"error\":\"oom\"}");
    if (response) cJSON_free(response);
    if (close_service || stop_wifi || enter_sleep) {
        ESP_LOGI(kTag, "Web control requested: close_service=%d stop_wifi=%d sleep=%d",
                 close_service ? 1 : 0,
                 stop_wifi ? 1 : 0,
                 enter_sleep ? 1 : 0);
        ScheduleDeferredControl(self, stop_wifi, enter_sleep);
    }
    return ESP_OK;
}

esp_err_t ApTransferServer::PhotosHandler(httpd_req_t* req) {
    NotifyActivity();
    cJSON* root = cJSON_CreateObject();
    cJSON* photos = cJSON_CreateArray();
    if (!root || !photos) {
        if (root) cJSON_Delete(root);
        if (photos) cJSON_Delete(photos);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    cJSON_AddItemToObject(root, "photos", photos);

    const int count = photo_get_count();
    for (int i = 0; i < count && i < PHOTO_MAX_PHOTOS; ++i) {
        PhotoInfo info = {};
        if (photo_get_by_index(i, &info) != 0) continue;
        cJSON* item = cJSON_CreateObject();
        if (!item) continue;
        cJSON_AddStringToObject(item, "id", info.id);
        cJSON_AddStringToObject(item, "title", info.title);
        cJSON_AddStringToObject(item, "date", info.date);
        cJSON_AddStringToObject(item, "location", info.location);
        cJSON_AddStringToObject(item, "body", info.body);
        cJSON_AddNumberToObject(item, "width", info.width);
        cJSON_AddNumberToObject(item, "height", info.height);
        cJSON_AddNumberToObject(item, "size", info.file_size);
        cJSON_AddStringToObject(item, "format", info.file_size > kImage1bppSize ? "bwry2bpp" : "1bpp");
        cJSON_AddItemToArray(photos, item);
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    CloseCurrentSession(req);
    cJSON_free(json);
    return ret;
}

esp_err_t ApTransferServer::PhotoHandler(httpd_req_t* req) {
    NotifyActivity();
    char query[64] = {};
    char id[16] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id, sizeof(id)) != ESP_OK ||
        id[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_FAIL;
    }

    if (req->method == HTTP_DELETE) {
        const bool deleted = photo_delete(id) == 0;
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        esp_err_t ret = httpd_resp_send(req, deleted ? "{\"success\":true}" : "{\"success\":false}",
                                        HTTPD_RESP_USE_STRLEN);
        CloseCurrentSession(req);
        if (ret != ESP_OK) return ret;
        return deleted ? ESP_OK : ESP_FAIL;
    }

    PhotoInfo info = {};
    bool found = false;
    const int count = photo_get_count();
    for (int i = 0; i < count && i < PHOTO_MAX_PHOTOS; ++i) {
        if (photo_get_by_index(i, &info) == 0 && strcmp(info.id, id) == 0) {
            found = true;
            break;
        }
    }
    if (!found || info.file_size == 0 || info.file_size > kImage2bppSize) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    auto* buf = static_cast<uint8_t*>(malloc(info.file_size));
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    const int bytes = photo_load(id, buf, info.file_size);
    if (bytes <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t ret = httpd_resp_send(req, reinterpret_cast<const char*>(buf), bytes);
    CloseCurrentSession(req);
    free(buf);
    return ret;
}

esp_err_t ApTransferServer::PhotoMetaHandler(httpd_req_t* req) {
    NotifyActivity();
    cJSON* root = ReadJsonBody(req);
    if (!root) {
        SendJson(req, "{\"success\":false,\"error\":\"bad_json\"}");
        return ESP_FAIL;
    }

    char id[16] = {};
    CopyJsonString(root, "id", id, sizeof(id));
    PhotoInfo info = {};
    bool found = false;
    const int count = photo_get_count();
    for (int i = 0; i < count && i < PHOTO_MAX_PHOTOS; ++i) {
        if (photo_get_by_index(i, &info) == 0 && strcmp(info.id, id) == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        cJSON_Delete(root);
        SendJson(req, "{\"success\":false,\"error\":\"not_found\"}");
        return ESP_FAIL;
    }

    CopyJsonString(root, "title", info.title, sizeof(info.title));
    CopyJsonString(root, "date", info.date, sizeof(info.date));
    CopyJsonString(root, "location", info.location, sizeof(info.location));
    CopyJsonString(root, "body", info.body, sizeof(info.body));
    cJSON_Delete(root);

    auto* self = static_cast<ApTransferServer*>(req->user_ctx);
    const bool ok = photo_update_info(id, &info) == 0;
    if (ok && self && self->photos_changed_callback_) {
        self->photos_changed_callback_();
    }
    SendJson(req, ok ? "{\"success\":true}" : "{\"success\":false}");
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t ApTransferServer::PhotoMoveHandler(httpd_req_t* req) {
    NotifyActivity();
    cJSON* root = ReadJsonBody(req);
    if (!root) {
        SendJson(req, "{\"success\":false,\"error\":\"bad_json\"}");
        return ESP_FAIL;
    }
    char id[16] = {};
    CopyJsonString(root, "id", id, sizeof(id));
    cJSON* delta_item = cJSON_GetObjectItemCaseSensitive(root, "delta");
    const int delta = cJSON_IsNumber(delta_item) ? delta_item->valueint : 0;
    cJSON_Delete(root);

    auto* self = static_cast<ApTransferServer*>(req->user_ctx);
    const bool ok = photo_move(id, delta) == 0;
    if (ok && self && self->photos_changed_callback_) {
        self->photos_changed_callback_();
    }
    SendJson(req, ok ? "{\"success\":true}" : "{\"success\":false}");
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t ApTransferServer::PhotoShowHandler(httpd_req_t* req) {
    NotifyActivity();
    cJSON* root = ReadJsonBody(req);
    if (!root) {
        SendJson(req, "{\"success\":false,\"error\":\"bad_json\"}");
        return ESP_FAIL;
    }
    char id[16] = {};
    CopyJsonString(root, "id", id, sizeof(id));
    cJSON_Delete(root);

    auto* self = static_cast<ApTransferServer*>(req->user_ctx);
    const bool ok = self && self->show_photo_callback_ && self->show_photo_callback_(id);
    SendJson(req, ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"not_found\"}");
    return ok ? ESP_OK : ESP_FAIL;
}

void ApTransferServer::NotifyState(ServerState state, const std::string& message) {
    ESP_LOGI(kTag, "State: %d, message: %s", state, message.c_str());
    if (state_callback_) {
        state_callback_(state, message);
    }
}

void ApTransferServer::SetStateCallback(std::function<void(ServerState, const std::string&)> callback) {
    state_callback_ = callback;
}

void ApTransferServer::SetImageReceivedCallback(std::function<void(const char* photo_id)> callback) {
    image_received_callback_ = callback;
}

void ApTransferServer::SetSettingsChangedCallback(std::function<void(int)> callback) {
    settings_changed_callback_ = callback;
}

void ApTransferServer::SetPhotosChangedCallback(std::function<void()> callback) {
    photos_changed_callback_ = callback;
}

void ApTransferServer::SetShowPhotoCallback(std::function<bool(const std::string&)> callback) {
    show_photo_callback_ = std::move(callback);
}

static void SendEmbeddedHtml(httpd_req_t* req, const char* start, const char* end) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    const size_t html_len = static_cast<size_t>(end - start);
    httpd_resp_send(req, start, html_len);
    CloseCurrentSession(req);
}

static void FormatUptime(uint64_t seconds, char* out, size_t out_size) {
    unsigned days = static_cast<unsigned>(seconds / 86400);
    unsigned hours = static_cast<unsigned>((seconds % 86400) / 3600);
    unsigned mins = static_cast<unsigned>((seconds % 3600) / 60);
    unsigned secs = static_cast<unsigned>(seconds % 60);
    snprintf(out, out_size, "%ud %uh %um %us", days, hours, mins, secs);
}

esp_err_t ApTransferServer::OverviewHandler(httpd_req_t* req) {
    NotifyActivity();
    const uint64_t uptime_s = static_cast<uint64_t>(esp_timer_get_time() / 1000000);
    char uptime[32];
    FormatUptime(uptime_s, uptime, sizeof(uptime));

    time_t now = time(nullptr);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    char local_time[32] = {};
    strftime(local_time, sizeof(local_time), "%Y-%m-%d %H:%M:%S", &local_tm);

    char boot_time[32] = {};
    if (local_tm.tm_year + 1900 >= 2021 && now > static_cast<time_t>(uptime_s)) {
        time_t boot = now - static_cast<time_t>(uptime_s);
        struct tm boot_tm = {};
        localtime_r(&boot, &boot_tm);
        strftime(boot_time, sizeof(boot_time), "%Y-%m-%d %H:%M:%S", &boot_tm);
    }

    int bat_level = -1;
    bool charging = false;
    bool discharging = false;
    bool bat_ok = Board::GetInstance().GetBatteryLevel(bat_level, charging, discharging);
    uint16_t voltage_mv = 0;
    uint8_t voltage_pct = 0;
    bool power_present = false;
    const bool volt_ok = Board::GetInstance().GetBatteryVoltage(voltage_mv, voltage_pct, power_present);
    if (!bat_ok && volt_ok) {
        bat_level = static_cast<int>(voltage_pct);
        discharging = !power_present;
        bat_ok = true;
    }
    const char* bat_status = "未知";
    if (bat_ok) {
        if (charging) {
            bat_status = "充电中";
        } else if (discharging) {
            bat_status = "放电中";
        } else {
            bat_status = "待机";
        }
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime_s", static_cast<double>(uptime_s));
    cJSON_AddStringToObject(root, "uptime", uptime);
    cJSON_AddStringToObject(root, "boot_time", boot_time);
    cJSON_AddStringToObject(root, "local_time", local_time);
    cJSON_AddStringToObject(root, "model", BOARD_NAME);
    cJSON_AddStringToObject(root, "version", "v" PROJECT_VER);
    cJSON* battery = cJSON_CreateObject();
    cJSON_AddBoolToObject(battery, "ok", bat_ok);
    cJSON_AddNumberToObject(battery, "level", bat_ok ? bat_level : -1);
    cJSON_AddBoolToObject(battery, "charging", charging);
    cJSON_AddBoolToObject(battery, "discharging", discharging);
    cJSON_AddStringToObject(battery, "status", bat_status);
    if (volt_ok) {
        cJSON_AddNumberToObject(battery, "voltage_mv", voltage_mv);
        cJSON_AddBoolToObject(battery, "power_present", power_present);
    }
    cJSON_AddItemToObject(root, "battery", battery);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        SendJson(req, "{\"success\":false}");
        return ESP_FAIL;
    }
    SendJson(req, json);
    cJSON_free(json);
    return ESP_OK;
}

esp_err_t ApTransferServer::InterfaceHandler(httpd_req_t* req) {
    NotifyActivity();
    if (req->method == HTTP_POST) {
        cJSON* body = ReadJsonBody(req);
        if (!body) {
            SendJson(req, "{\"success\":false,\"error\":\"bad_json\"}");
            return ESP_FAIL;
        }
        NetifConfig cfg;
        bool ok = NetifConfig::FromJson(body, &cfg);
        cJSON_Delete(body);
        if (!ok || !cfg.Save()) {
            SendJson(req, "{\"success\":false,\"error\":\"invalid\"}");
            return ESP_FAIL;
        }
        NetifConfig::ApplyOnConnected(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
        cJSON* resp = NetifConfig::ToJson();
        char* json = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (!json) {
            SendJson(req, "{\"success\":true}");
            return ESP_OK;
        }
        SendJson(req, json);
        cJSON_free(json);
        return ESP_OK;
    }
    cJSON* resp = NetifConfig::ToJson();
    char* json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json) {
        SendJson(req, "{\"success\":false}");
        return ESP_FAIL;
    }
    SendJson(req, json);
    cJSON_free(json);
    return ESP_OK;
}

esp_err_t ApTransferServer::ScanHandler(httpd_req_t* req) {
    NotifyActivity();
    bool support_5g = false;
#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
    support_5g = true;
#endif
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_wifi_scan_start(&scan_config, true);

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num > 32) ap_num = 32;
    std::vector<wifi_ap_record_t> records(ap_num);
    if (ap_num > 0) {
        esp_wifi_scan_get_ap_records(&ap_num, records.data());
        records.resize(ap_num);
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "support_5g", support_5g);
    cJSON* aps = cJSON_CreateArray();
    for (const auto& ap : records) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", reinterpret_cast<const char*>(ap.ssid));
        cJSON_AddNumberToObject(item, "rssi", ap.rssi);
        cJSON_AddNumberToObject(item, "authmode", ap.authmode);
        cJSON_AddItemToArray(aps, item);
    }
    cJSON_AddItemToObject(root, "aps", aps);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        SendJson(req, "{\"support_5g\":false,\"aps\":[]}");
        return ESP_OK;
    }
    SendJson(req, json);
    cJSON_free(json);
    return ESP_OK;
}

esp_err_t ApTransferServer::SavedListHandler(httpd_req_t* req) {
    NotifyActivity();
    auto list = SsidManager::GetInstance().GetSsidList();
    cJSON* arr = cJSON_CreateArray();
    for (const auto& item : list) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(item.ssid.c_str()));
    }
    char* json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) {
        SendJson(req, "[]");
        return ESP_OK;
    }
    SendJson(req, json);
    cJSON_free(json);
    return ESP_OK;
}

esp_err_t ApTransferServer::SavedDeleteHandler(httpd_req_t* req) {
    NotifyActivity();
    int index = -1;
    std::string uri = req->uri;
    auto pos = uri.find("?index=");
    if (pos != std::string::npos) sscanf(uri.c_str() + pos + 7, "%d", &index);
    if (index >= 0) SsidManager::GetInstance().RemoveSsid(index);
    SendJson(req, "{}");
    return ESP_OK;
}

esp_err_t ApTransferServer::SavedSetDefaultHandler(httpd_req_t* req) {
    NotifyActivity();
    int index = -1;
    std::string uri = req->uri;
    auto pos = uri.find("?index=");
    if (pos != std::string::npos) sscanf(uri.c_str() + pos + 7, "%d", &index);
    if (index >= 0) SsidManager::GetInstance().SetDefaultSsid(index);
    SendJson(req, "{}");
    return ESP_OK;
}

esp_err_t ApTransferServer::SubmitWifiHandler(httpd_req_t* req) {
    NotifyActivity();
    cJSON* json = ReadJsonBody(req);
    if (!json) {
        SendJson(req, "{\"success\":false,\"error\":\"bad_json\"}");
        return ESP_FAIL;
    }
    cJSON* ssid_item = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON* password_item = cJSON_GetObjectItemCaseSensitive(json, "password");
    std::string ssid;
    std::string password;
    if (cJSON_IsString(ssid_item) && ssid_item->valuestring) ssid = ssid_item->valuestring;
    if (cJSON_IsString(password_item) && password_item->valuestring) password = password_item->valuestring;
    cJSON_Delete(json);
    if (!wifi_credentials::Valid(ssid, password)) {
        SendJson(req, "{\"success\":false,\"error\":\"Invalid SSID\"}");
        return ESP_OK;
    }
    SsidManager::GetInstance().AddSsid(ssid, password);
    SendJson(req, "{\"success\":true}");
    return ESP_OK;
}

esp_err_t ApTransferServer::DeveloperBleHandler(httpd_req_t* req) {
    NotifyActivity();
    SendEmbeddedHtml(req, ble_web_panel_html_start, ble_web_panel_html_end);
    return ESP_OK;
}

esp_err_t ApTransferServer::DeveloperLayoutHandler(httpd_req_t* req) {
    NotifyActivity();
    SendEmbeddedHtml(req, layout_adjuster_html_start, layout_adjuster_html_end);
    return ESP_OK;
}

}  // namespace rawdraw
