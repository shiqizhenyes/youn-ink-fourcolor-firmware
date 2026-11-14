include("${CMAKE_CURRENT_LIST_DIR}/../../../webui/webui.cmake")
set(WEBUI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../webui")
set(WEBUI_GEN "${CMAKE_CURRENT_BINARY_DIR}/webui")
ink_webui_inject(
    "${CMAKE_CURRENT_LIST_DIR}/assets/wifi_configuration.html.in"
    "${WEBUI_DIR}/luci-lite.css"
    "${WEBUI_DIR}/theme.js"
    "${WEBUI_GEN}/wifi_configuration.html"
)
ink_webui_inject(
    "${CMAKE_CURRENT_LIST_DIR}/assets/wifi_configuration_done.html.in"
    "${WEBUI_DIR}/luci-lite.css"
    "${WEBUI_DIR}/theme.js"
    "${WEBUI_GEN}/wifi_configuration_done.html"
)

idf_component_register(
    SRCS
        "wifi_manager.cc"
        "wifi_configuration_ap.cc"
        "wifi_station.cc"
        "ssid_manager.cc"
        "dns_server.cc"
        "netif_config.cc"
    INCLUDE_DIRS
        "include"
    EMBED_TXTFILES
        "${WEBUI_GEN}/wifi_configuration.html"
        "${WEBUI_GEN}/wifi_configuration_done.html"
    REQUIRES
    	"esp_timer"
        "esp_http_server"
        "esp_wifi"
        "nvs_flash"
        "cjson"
        "esp_netif"
        "lwip"
)
