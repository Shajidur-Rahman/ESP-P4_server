#include "mdns_manager.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "mdns_mgr";

esp_err_t mdns_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing mDNS Service...");
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Hostname: http://nas.local or http://localserver.local
    // Let's set primary hostname to "localserver" (resolves to localserver.local)
    err = mdns_hostname_set("localserver");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
        return err;
    }

    // Instance name
    mdns_instance_name_set("ESP32-P4 High Speed NAS");

    // Add HTTP service advertisement
    mdns_service_add("ESP32-P4 Web NAS", "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " mDNS Active: Access via http://localserver.local ");
    ESP_LOGI(TAG, "==================================================");

    return ESP_OK;
}
