#include "smart_softap.hpp"

#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "smart_softap";
esp_netif_t *g_ap_netif = nullptr;
bool g_wifi_started = false;

void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *event_data)
{
    if (base != WIFI_EVENT) {
        return;
    }

    if (id == WIFI_EVENT_AP_STACONNECTED) {
        auto *event = static_cast<wifi_event_ap_staconnected_t *>(event_data);
        ESP_LOGI(kTag,
                 "Station joined, aid=%d, mac=%02x:%02x:%02x:%02x:%02x:%02x",
                 event->aid,
                 event->mac[0],
                 event->mac[1],
                 event->mac[2],
                 event->mac[3],
                 event->mac[4],
                 event->mac[5]);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        auto *event = static_cast<wifi_event_ap_stadisconnected_t *>(event_data);
        ESP_LOGI(kTag,
                 "Station left, aid=%d, mac=%02x:%02x:%02x:%02x:%02x:%02x",
                 event->aid,
                 event->mac[0],
                 event->mac[1],
                 event->mac[2],
                 event->mac[3],
                 event->mac[4],
                 event->mac[5]);
    }
}

esp_err_t init_nvs()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t init_network_stack()
{
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    if (g_ap_netif == nullptr) {
        g_ap_netif = esp_netif_create_default_wifi_ap();
        if (g_ap_netif == nullptr) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

} // namespace

namespace smart_bin {

esp_err_t start_softap(esp_netif_t **out_ap_netif)
{
    ESP_RETURN_ON_ERROR(init_nvs(), kTag, "NVS init failed");
    ESP_RETURN_ON_ERROR(init_network_stack(), kTag, "Network stack init failed");

    if (!g_wifi_started) {
        wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_config), kTag, "esp_wifi_init failed");
        ESP_RETURN_ON_ERROR(
            esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr),
            kTag,
            "Failed to register Wi-Fi event handler");
    }

    wifi_config_t ap_config = {};
    std::strncpy(reinterpret_cast<char *>(ap_config.ap.ssid), CONFIG_SMART_SOFTAP_SSID, sizeof(ap_config.ap.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(ap_config.ap.password),
                 CONFIG_SMART_SOFTAP_PASSWORD,
                 sizeof(ap_config.ap.password) - 1);
    ap_config.ap.ssid_len = std::strlen(CONFIG_SMART_SOFTAP_SSID);
    ap_config.ap.max_connection = 4;
    ap_config.ap.channel = CONFIG_SMART_SOFTAP_CHANNEL;
    ap_config.ap.pmf_cfg.required = false;
    ap_config.ap.authmode =
        (std::strlen(CONFIG_SMART_SOFTAP_PASSWORD) >= 8) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), kTag, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), kTag, "esp_wifi_set_config failed");

    if (!g_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "esp_wifi_start failed");
        g_wifi_started = true;
    }

    esp_netif_ip_info_t ip_info = {};
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(g_ap_netif, &ip_info), kTag, "esp_netif_get_ip_info failed");

    ESP_LOGI(kTag,
             "SoftAP started: ssid=%s channel=%d auth=%s",
             CONFIG_SMART_SOFTAP_SSID,
             CONFIG_SMART_SOFTAP_CHANNEL,
             ap_config.ap.authmode == WIFI_AUTH_OPEN ? "open" : "wpa/wpa2-psk");
    ESP_LOGI(kTag, "SoftAP password: %s", CONFIG_SMART_SOFTAP_PASSWORD);
    ESP_LOGI(kTag, "SoftAP IP: " IPSTR, IP2STR(&ip_info.ip));

    if (out_ap_netif != nullptr) {
        *out_ap_netif = g_ap_netif;
    }
    return ESP_OK;
}

} // namespace smart_bin
