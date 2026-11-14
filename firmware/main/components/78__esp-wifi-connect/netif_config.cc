#include "netif_config.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <lwip/ip6_addr.h>
#include "sdkconfig.h"
#if CONFIG_LWIP_IPV6
#include <esp_netif_net_stack.h>
#include <lwip/netif.h>
#include <lwip/priv/nd6_priv.h>
#endif

#include <cstring>

namespace {
constexpr const char* kTag = "NetifConfig";
constexpr const char* kNs = "netif";

std::string NvsGetStr(nvs_handle_t nvs, const char* key, const char* fallback) {
    size_t len = 0;
    if (nvs_get_str(nvs, key, nullptr, &len) != ESP_OK || len == 0) return fallback;
    std::string value(len, '\0');
    if (nvs_get_str(nvs, key, value.data(), &len) != ESP_OK) return fallback;
    while (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

int32_t NvsGetI32(nvs_handle_t nvs, const char* key, int32_t fallback) {
    int32_t value = fallback;
    if (nvs_get_i32(nvs, key, &value) != ESP_OK) return fallback;
    return value;
}

void NvsSetStr(nvs_handle_t nvs, const char* key, const std::string& value) {
    nvs_set_str(nvs, key, value.c_str());
}

bool ParseIPv4(const std::string& text, esp_ip4_addr_t* out) {
    if (!out || text.empty()) return false;
    return esp_netif_str_to_ip4(text.c_str(), out) == ESP_OK;
}

std::string Ip4ToString(esp_ip4_addr_t addr) {
    char buf[16] = {};
    esp_ip4addr_ntoa(&addr, buf, sizeof(buf));
    return buf;
}

void SetDnsSlot(esp_netif_t* netif, esp_netif_dns_type_t type, const std::string& text) {
    if (!netif || text.empty()) return;
    esp_netif_dns_info_t dns{};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    if (!ParseIPv4(text, &dns.ip.u_addr.ip4)) return;
    esp_netif_set_dns_info(netif, type, &dns);
}

esp_netif_t* StaNetif() {
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

#if CONFIG_LWIP_IPV6
struct Ipv6NdSnap {
    esp_netif_t* netif = nullptr;
    char gateway[48] = {};
};

esp_err_t SnapshotIpv6Gateway(void* ctx) {
    auto* snap = static_cast<Ipv6NdSnap*>(ctx);
    if (!snap || !snap->netif) return ESP_OK;
    auto* lwip_netif = static_cast<struct netif*>(esp_netif_get_netif_impl(snap->netif));
    if (!lwip_netif) return ESP_OK;
    for (int i = 0; i < LWIP_ND6_NUM_ROUTERS; ++i) {
        auto* nb = default_router_list[i].neighbor_entry;
        if (!nb || nb->netif != lwip_netif) continue;
        ip6addr_ntoa_r(&nb->next_hop_address, snap->gateway, sizeof(snap->gateway));
        if (snap->gateway[0]) break;
    }
    return ESP_OK;
}
#endif
}  // namespace

NetifConfig NetifConfig::Load() {
    NetifConfig cfg;
    nvs_handle_t nvs = 0;
    if (nvs_open(kNs, NVS_READONLY, &nvs) != ESP_OK) return cfg;
    cfg.proto = NvsGetStr(nvs, "proto", "dhcp");
    if (cfg.proto != "static") cfg.proto = "dhcp";
    cfg.hostname = NvsGetStr(nvs, "hostname", "");
    cfg.ipv4 = NvsGetStr(nvs, "ipv4", "");
    cfg.netmask = NvsGetStr(nvs, "netmask", "255.255.255.0");
    cfg.gateway = NvsGetStr(nvs, "gateway", "");
    cfg.broadcast = NvsGetStr(nvs, "broadcast", "");
    cfg.use_default_gateway = NvsGetI32(nvs, "use_gw", 1) != 0;
    cfg.peerdns = NvsGetI32(nvs, "peerdns", 1) != 0;
    cfg.dns0 = NvsGetStr(nvs, "dns0", "");
    cfg.dns1 = NvsGetStr(nvs, "dns1", "");
    cfg.ipv6_mode = NvsGetStr(nvs, "ipv6_mode", "auto");
    if (cfg.ipv6_mode != "disabled" && cfg.ipv6_mode != "static") cfg.ipv6_mode = "auto";
    cfg.ipv6 = NvsGetStr(nvs, "ipv6", "");
    cfg.ipv6_prefix = NvsGetStr(nvs, "ipv6_plen", "64");
    cfg.ipv6_gateway = NvsGetStr(nvs, "ipv6_gw", "");
    nvs_close(nvs);
    return cfg;
}

bool NetifConfig::Save() const {
    nvs_handle_t nvs = 0;
    if (nvs_open(kNs, NVS_READWRITE, &nvs) != ESP_OK) return false;
    NvsSetStr(nvs, "proto", proto == "static" ? "static" : "dhcp");
    NvsSetStr(nvs, "hostname", hostname);
    NvsSetStr(nvs, "ipv4", ipv4);
    NvsSetStr(nvs, "netmask", netmask.empty() ? "255.255.255.0" : netmask);
    NvsSetStr(nvs, "gateway", gateway);
    NvsSetStr(nvs, "broadcast", broadcast);
    nvs_set_i32(nvs, "use_gw", use_default_gateway ? 1 : 0);
    nvs_set_i32(nvs, "peerdns", peerdns ? 1 : 0);
    NvsSetStr(nvs, "dns0", dns0);
    NvsSetStr(nvs, "dns1", dns1);
    std::string mode = ipv6_mode;
    if (mode != "disabled" && mode != "static") mode = "auto";
    NvsSetStr(nvs, "ipv6_mode", mode);
    NvsSetStr(nvs, "ipv6", ipv6);
    NvsSetStr(nvs, "ipv6_plen", ipv6_prefix.empty() ? "64" : ipv6_prefix);
    NvsSetStr(nvs, "ipv6_gw", ipv6_gateway);
    nvs_commit(nvs);
    nvs_close(nvs);
    return true;
}

bool NetifConfig::IsStatic() {
    return Load().proto == "static";
}

void NetifConfig::ApplyHostname(esp_netif_t* netif) {
    if (!netif) netif = StaNetif();
    if (!netif) return;
    auto cfg = Load();
    if (cfg.hostname.empty()) return;
    esp_err_t err = esp_netif_set_hostname(netif, cfg.hostname.c_str());
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "set hostname failed: %s", esp_err_to_name(err));
    }
}

void NetifConfig::ApplyStaticIPv4(esp_netif_t* netif) {
    if (!netif) netif = StaNetif();
    if (!netif) return;
    auto cfg = Load();
    if (cfg.proto != "static") return;
    esp_netif_ip_info_t info{};
    if (!ParseIPv4(cfg.ipv4, &info.ip) || !ParseIPv4(cfg.netmask, &info.netmask)) {
        ESP_LOGW(kTag, "static IPv4 invalid");
        return;
    }
    if (cfg.use_default_gateway) {
        ParseIPv4(cfg.gateway, &info.gw);
    }
    esp_netif_dhcpc_stop(netif);
    esp_err_t err = esp_netif_set_ip_info(netif, &info);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "set_ip_info failed: %s", esp_err_to_name(err));
        return;
    }
    ApplyDns(netif, true);
}

void NetifConfig::ApplyDns(esp_netif_t* netif, bool force_custom) {
    if (!netif) netif = StaNetif();
    if (!netif) return;
    auto cfg = Load();
    if (!force_custom && cfg.peerdns && cfg.proto == "dhcp") return;
    if (cfg.dns0.empty() && cfg.dns1.empty()) return;
    SetDnsSlot(netif, ESP_NETIF_DNS_MAIN, cfg.dns0);
    SetDnsSlot(netif, ESP_NETIF_DNS_BACKUP, cfg.dns1);
}

void NetifConfig::ApplyIPv6(esp_netif_t* netif) {
#if CONFIG_LWIP_IPV6
    if (!netif) netif = StaNetif();
    if (!netif) return;
    auto cfg = Load();
    if (cfg.ipv6_mode == "disabled") {
        ESP_LOGI(kTag, "IPv6 disabled");
        return;
    }
    esp_err_t err = esp_netif_create_ip6_linklocal(netif);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "create IPv6 link-local failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(kTag, "IPv6 link-local started, mode=%s", cfg.ipv6_mode.c_str());
    }
    if (cfg.ipv6_mode != "static" || cfg.ipv6.empty()) return;
    esp_ip6_addr_t addr{};
    if (esp_netif_str_to_ip6(cfg.ipv6.c_str(), &addr) != ESP_OK) {
        ESP_LOGW(kTag, "static IPv6 invalid");
        return;
    }
    err = esp_netif_add_ip6_address(netif, addr, true);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "add IPv6 failed: %s", esp_err_to_name(err));
    }
#else
    (void)netif;
#endif
}

void NetifConfig::ApplyOnConnected(esp_netif_t* netif) {
    if (!netif) netif = StaNetif();
    if (!netif) return;
    ApplyHostname(netif);
    auto cfg = Load();
    if (cfg.proto == "static") {
        ApplyStaticIPv4(netif);
    } else {
        esp_netif_dhcpc_start(netif);
        ApplyDns(netif, false);
    }
    ApplyIPv6(netif);
}

cJSON* NetifConfig::ToJson() {
    auto cfg = Load();
    cJSON* root = cJSON_CreateObject();
    cJSON* live = cJSON_CreateObject();
    cJSON* config = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "live", live);
    cJSON_AddItemToObject(root, "config", config);
    cJSON_AddBoolToObject(root, "success", true);

    cJSON_AddStringToObject(config, "proto", cfg.proto.c_str());
    cJSON_AddStringToObject(config, "hostname", cfg.hostname.c_str());
    cJSON_AddStringToObject(config, "ipv4", cfg.ipv4.c_str());
    cJSON_AddStringToObject(config, "netmask", cfg.netmask.c_str());
    cJSON_AddStringToObject(config, "gateway", cfg.gateway.c_str());
    cJSON_AddStringToObject(config, "broadcast", cfg.broadcast.c_str());
    cJSON_AddBoolToObject(config, "use_default_gateway", cfg.use_default_gateway);
    cJSON_AddBoolToObject(config, "peerdns", cfg.peerdns);
    cJSON* dns = cJSON_CreateArray();
    cJSON_AddItemToArray(dns, cJSON_CreateString(cfg.dns0.c_str()));
    cJSON_AddItemToArray(dns, cJSON_CreateString(cfg.dns1.c_str()));
    cJSON_AddItemToObject(config, "dns", dns);
    cJSON_AddStringToObject(config, "ipv6_mode", cfg.ipv6_mode.c_str());
    cJSON_AddStringToObject(config, "ipv6", cfg.ipv6.c_str());
    cJSON_AddStringToObject(config, "ipv6_prefix", cfg.ipv6_prefix.c_str());
    cJSON_AddStringToObject(config, "ipv6_gateway", cfg.ipv6_gateway.c_str());

    esp_netif_t* netif = StaNetif();
    if (netif) {
        esp_netif_ip_info_t info{};
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            cJSON_AddStringToObject(live, "ipv4", Ip4ToString(info.ip).c_str());
            cJSON_AddStringToObject(live, "netmask", Ip4ToString(info.netmask).c_str());
            cJSON_AddStringToObject(live, "gateway", Ip4ToString(info.gw).c_str());
            uint32_t bcast = (info.ip.addr & info.netmask.addr) | ~info.netmask.addr;
            esp_ip4_addr_t b{};
            b.addr = bcast;
            cJSON_AddStringToObject(live, "broadcast", Ip4ToString(b).c_str());
        }
        cJSON* live_dns = cJSON_CreateArray();
        for (auto type : {ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP}) {
            esp_netif_dns_info_t dns_info{};
            if (esp_netif_get_dns_info(netif, type, &dns_info) == ESP_OK &&
                dns_info.ip.type == ESP_IPADDR_TYPE_V4 &&
                dns_info.ip.u_addr.ip4.addr != 0) {
                cJSON_AddItemToArray(live_dns, cJSON_CreateString(Ip4ToString(dns_info.ip.u_addr.ip4).c_str()));
            }
        }
        cJSON_AddItemToObject(live, "dns", live_dns);
        cJSON* live6 = cJSON_CreateArray();
        std::string preferred6;
        std::string linklocal6;
#if CONFIG_LWIP_IPV6
        esp_ip6_addr_t ip6[CONFIG_LWIP_IPV6_NUM_ADDRESSES] = {};
        int n6 = esp_netif_get_all_ip6(netif, ip6);
        for (int i = 0; i < n6; ++i) {
            char buf[48] = {};
            ip6addr_ntoa_r(reinterpret_cast<const ip6_addr_t*>(&ip6[i]), buf, sizeof(buf));
            if (!buf[0]) continue;
            cJSON_AddItemToArray(live6, cJSON_CreateString(buf));
            auto type = esp_netif_ip6_get_addr_type(&ip6[i]);
            if (type == ESP_IP6_ADDR_IS_LINK_LOCAL) {
                if (linklocal6.empty()) linklocal6 = buf;
            } else if (preferred6.empty() &&
                       (type == ESP_IP6_ADDR_IS_GLOBAL || type == ESP_IP6_ADDR_IS_UNIQUE_LOCAL)) {
                preferred6 = buf;
            }
        }
        if (preferred6.empty()) preferred6 = linklocal6;
        Ipv6NdSnap snap;
        snap.netif = netif;
        esp_netif_tcpip_exec(SnapshotIpv6Gateway, &snap);
        if (snap.gateway[0]) {
            cJSON_AddStringToObject(live, "ipv6_gateway", snap.gateway);
        }
#endif
        cJSON_AddItemToObject(live, "ipv6", live6);
        if (!preferred6.empty()) {
            cJSON_AddStringToObject(live, "ipv6_preferred", preferred6.c_str());
        }
        cJSON_AddStringToObject(live, "ipv6_prefix", "64");
        const char* hostname = nullptr;
        if (esp_netif_get_hostname(netif, &hostname) == ESP_OK && hostname) {
            cJSON_AddStringToObject(live, "hostname", hostname);
        }
    }
    return root;
}

bool NetifConfig::FromJson(cJSON* root, NetifConfig* out) {
    if (!root || !out) return false;
    *out = Load();
    auto str = [&](const char* key, std::string* dest) {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsString(item) && item->valuestring) *dest = item->valuestring;
    };
    auto boolean = [&](const char* key, bool* dest) {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsBool(item)) *dest = cJSON_IsTrue(item);
    };
    str("proto", &out->proto);
    if (out->proto != "static") out->proto = "dhcp";
    str("hostname", &out->hostname);
    str("ipv4", &out->ipv4);
    str("netmask", &out->netmask);
    str("gateway", &out->gateway);
    str("broadcast", &out->broadcast);
    boolean("use_default_gateway", &out->use_default_gateway);
    boolean("peerdns", &out->peerdns);
    str("ipv6_mode", &out->ipv6_mode);
    str("ipv6", &out->ipv6);
    str("ipv6_prefix", &out->ipv6_prefix);
    str("ipv6_gateway", &out->ipv6_gateway);
    cJSON* dns = cJSON_GetObjectItemCaseSensitive(root, "dns");
    if (cJSON_IsArray(dns)) {
        cJSON* a = cJSON_GetArrayItem(dns, 0);
        cJSON* b = cJSON_GetArrayItem(dns, 1);
        if (cJSON_IsString(a) && a->valuestring) out->dns0 = a->valuestring;
        if (cJSON_IsString(b) && b->valuestring) out->dns1 = b->valuestring;
    }
    if (out->proto == "static") {
        esp_ip4_addr_t dummy{};
        if (!ParseIPv4(out->ipv4, &dummy) || !ParseIPv4(out->netmask, &dummy)) return false;
        if (out->use_default_gateway && !out->gateway.empty() && !ParseIPv4(out->gateway, &dummy)) {
            return false;
        }
    }
    if (out->ipv6_mode != "disabled" && out->ipv6_mode != "static") out->ipv6_mode = "auto";
    if (out->hostname.size() > 32) out->hostname.resize(32);
    return true;
}
