#include "wifi_manager.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_manager";

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define DEFAULT_WIFI_SSID  "Saad"
#define DEFAULT_WIFI_PASS  "AndroiD@mekalil.123,./"

static esp_netif_t *s_sta_netif = NULL;
static bool s_wifi_connected = false;
static char s_wifi_ip[32] = "0.0.0.0";
static char s_current_ssid[64] = "";

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t slen = strlen(src);
    if (slen >= dst_size) {
        slen = dst_size - 1;
    }
    memcpy(dst, src, slen);
    dst[slen] = '\0';
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi Station started, connecting to %s...", s_current_ssid);
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "Wi-Fi Station connected to AP: %s", s_current_ssid);
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_wifi_connected = false;
            strcpy(s_wifi_ip, "0.0.0.0");
            ESP_LOGW(TAG, "Wi-Fi disconnected. Reconnecting in 3s...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_wifi_connect();
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_wifi_ip, sizeof(s_wifi_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, " Wi-Fi (ESP32-C6) Connected!");
        ESP_LOGI(TAG, " SSID        : %s", s_current_ssid);
        ESP_LOGI(TAG, " Wi-Fi IP    : %s", s_wifi_ip);
        ESP_LOGI(TAG, " Hostname    : http://localserver.local");
        ESP_LOGI(TAG, "========================================");
    }
}

esp_err_t wifi_manager_load_settings(wifi_settings_t *out_settings)
{
    if (!out_settings) return ESP_ERR_INVALID_ARG;
    memset(out_settings, 0, sizeof(wifi_settings_t));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t len = sizeof(out_settings->ssid);
        nvs_get_str(handle, "ssid", out_settings->ssid, &len);
        len = sizeof(out_settings->password);
        nvs_get_str(handle, "pass", out_settings->password, &len);
        nvs_close(handle);
    }

    if (strlen(out_settings->ssid) == 0) {
        safe_copy(out_settings->ssid, sizeof(out_settings->ssid), DEFAULT_WIFI_SSID);
        safe_copy(out_settings->password, sizeof(out_settings->password), DEFAULT_WIFI_PASS);
    }
    out_settings->enabled = true;
    return ESP_OK;
}

esp_err_t wifi_manager_save_settings(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for Wi-Fi save: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_str(handle, "ssid", ssid);
    if (password) {
        nvs_set_str(handle, "pass", password);
    } else {
        nvs_set_str(handle, "pass", "");
    }
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "New Wi-Fi credentials saved: SSID '%s'", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing Wi-Fi via ESP32-C6 Hosted Co-processor...");

    wifi_settings_t cfg_data;
    wifi_manager_load_settings(&cfg_data);
    safe_copy(s_current_ssid, sizeof(s_current_ssid), cfg_data.ssid);

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "Failed to create default wifi sta netif");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = { 0 };
    safe_copy((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), cfg_data.ssid);
    safe_copy((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), cfg_data.password);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi initialization started successfully for SSID: %s", cfg_data.ssid);
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_wifi_connected;
}

const char *wifi_manager_get_ip_str(void)
{
    return s_wifi_ip;
}

const char *wifi_manager_get_ssid(void)
{
    return s_current_ssid;
}

esp_err_t wifi_manager_reconnect(void)
{
    wifi_settings_t cfg_data;
    wifi_manager_load_settings(&cfg_data);
    safe_copy(s_current_ssid, sizeof(s_current_ssid), cfg_data.ssid);

    wifi_config_t wifi_config = { 0 };
    safe_copy((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), cfg_data.ssid);
    safe_copy((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), cfg_data.password);

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    return esp_wifi_connect();
}
