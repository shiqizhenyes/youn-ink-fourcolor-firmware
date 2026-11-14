#ifndef NETIF_CONFIG_H
#define NETIF_CONFIG_H

#include <string>
#include <vector>

#include <esp_netif.h>
#include <cJSON.h>

struct NetifConfig {
    std::string proto = "dhcp";
    std::string hostname;
    std::string ipv4;
    std::string netmask = "255.255.255.0";
    std::string gateway;
    std::string broadcast;
    bool use_default_gateway = true;
    bool peerdns = true;
    std::string dns0;
    std::string dns1;
    std::string ipv6_mode = "auto";
    std::string ipv6;
    std::string ipv6_prefix = "64";
    std::string ipv6_gateway;

    static NetifConfig Load();
    bool Save() const;

    static bool IsStatic();
    static void ApplyHostname(esp_netif_t* netif);
    static void ApplyStaticIPv4(esp_netif_t* netif);
    static void ApplyDns(esp_netif_t* netif, bool force_custom);
    static void ApplyIPv6(esp_netif_t* netif);
    static void ApplyOnConnected(esp_netif_t* netif);
    static cJSON* ToJson();
    static bool FromJson(cJSON* root, NetifConfig* out);
};

#endif
