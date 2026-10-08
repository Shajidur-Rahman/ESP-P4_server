#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "ethernet.h"
#include "usb_storage.h"
#include "http_server.h"
#include "wifi_manager.h"
#include "mdns_manager.h"
#include "download_manager.h"

static const char *TAG = "main";

void app_main(void)
{
    printf("\n");
    printf("====================================================\n");
    printf("  Waveshare ESP32-P4 Network Storage Server (NAS)  \n");
    printf("====================================================\n");

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("SoC Target       : %s (Dual-Core RISC-V @ 400MHz)\n", CONFIG_IDF_TARGET);
    printf("Silicon Revision : v%d.%d\n", chip_info.revision / 100, chip_info.revision % 100);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("Flash Size       : %lu MB\n", (unsigned long)(flash_size / (1024 * 1024)));
    }

    if (esp_psram_is_initialized()) {
        printf("PSRAM            : Initialized (32 MB Total, %u bytes free)\n",
               heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    printf("Internal SRAM    : %u bytes free\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("====================================================\n\n");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize USB Host Storage Subsystem
    ESP_LOGI(TAG, "Initializing USB Host Storage Subsystem...");
    ret = usb_storage_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize USB storage: %s", esp_err_to_name(ret));
    }

    // Initialize Ethernet (IP101 PHY)
    ESP_LOGI(TAG, "Initializing Ethernet (IP101 PHY)...");
    ret = ethernet_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Ethernet: %s", esp_err_to_name(ret));
    }

    // Initialize Wi-Fi via ESP32-C6 Hosted Co-processor
    ESP_LOGI(TAG, "Initializing Wi-Fi (ESP32-C6)...");
    ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi initialization reported: %s", esp_err_to_name(ret));
    }

    // Initialize mDNS (http://localserver.local)
    ret = mdns_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS initialization failed: %s", esp_err_to_name(ret));
    }

    // Initialize Background Multi-threaded Download Engine
    ESP_LOGI(TAG, "Initializing Download Engine...");
    download_manager_init();

    // Start HTTP Server
    ESP_LOGI(TAG, "Starting HTTP Server...");
    ret = http_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Server initialization complete. Entering health monitor loop.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "Status: ETH IP=%s | WiFi IP=%s (SSID: %s) | Hostname=http://localserver.local | USB=%s (Mounted: %s)",
                 ethernet_get_ip_str(),
                 wifi_manager_get_ip_str(),
                 wifi_manager_get_ssid(),
                 usb_storage_is_connected() ? "Connected" : "Disconnected",
                 usb_storage_is_mounted() ? "YES" : "NO");
    }
}
