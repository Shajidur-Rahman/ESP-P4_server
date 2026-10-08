#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];
    char password[65];
    bool enabled;
} wifi_settings_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_load_settings(wifi_settings_t *out_settings);
esp_err_t wifi_manager_save_settings(const char *ssid, const char *password);
bool wifi_manager_is_connected(void);
const char *wifi_manager_get_ip_str(void);
const char *wifi_manager_get_ssid(void);
esp_err_t wifi_manager_reconnect(void);

#ifdef __cplusplus
}
#endif
