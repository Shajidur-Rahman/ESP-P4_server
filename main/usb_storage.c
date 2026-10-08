#include "usb_storage.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "usb/usb_host.h"
#include "usb/msc_host_vfs.h"

static const char *TAG = "usb_storage";

typedef enum {
    MSG_DEVICE_CONNECTED,
    MSG_DEVICE_DISCONNECTED,
} usb_msg_id_t;

typedef struct {
    usb_msg_id_t id;
    union {
        uint8_t new_dev_address;
        msc_host_device_handle_t device_handle;
    } data;
} usb_app_msg_t;

static QueueHandle_t s_usb_queue = NULL;
static SemaphoreHandle_t s_storage_mutex = NULL;

static bool s_is_connected = false;
static bool s_is_mounted = false;
static uint8_t s_current_dev_addr = 0;
static msc_host_device_handle_t s_msc_device = NULL;
static msc_host_vfs_handle_t s_vfs_handle = NULL;

static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    if (!s_usb_queue) return;

    if (event->event == MSC_DEVICE_CONNECTED) {
        ESP_LOGI(TAG, "USB MSC device connected at address %d", event->device.address);
        usb_app_msg_t msg = {
            .id = MSG_DEVICE_CONNECTED,
            .data.new_dev_address = event->device.address,
        };
        xQueueSend(s_usb_queue, &msg, portMAX_DELAY);
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        ESP_LOGI(TAG, "USB MSC device disconnected");
        usb_app_msg_t msg = {
            .id = MSG_DEVICE_DISCONNECTED,
            .data.device_handle = event->device.handle,
        };
        xQueueSend(s_usb_queue, &msg, portMAX_DELAY);
    }
}

static void usb_host_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

static esp_err_t do_mount_device(uint8_t dev_addr)
{
    if (s_is_mounted) {
        return ESP_OK;
    }

    esp_err_t err;
    if (s_msc_device == NULL) {
        ESP_LOGI(TAG, "Installing MSC device at address %d...", dev_addr);
        err = msc_host_install_device(dev_addr, &s_msc_device);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "msc_host_install_device failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16384,
    };

    ESP_LOGI(TAG, "Registering VFS at %s...", USB_STORAGE_MOUNT_POINT);
    err = msc_host_vfs_register(s_msc_device, USB_STORAGE_MOUNT_POINT, &mount_config, &s_vfs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_vfs_register failed: %s", esp_err_to_name(err));
        if (s_msc_device) {
            msc_host_uninstall_device(s_msc_device);
            s_msc_device = NULL;
        }
        return err;
    }

    s_is_mounted = true;
    s_is_connected = true;
    s_current_dev_addr = dev_addr;

    uint64_t total = 0, free = 0;
    if (esp_vfs_fat_info(USB_STORAGE_MOUNT_POINT, &total, &free) == ESP_OK) {
        ESP_LOGI(TAG, "Storage mounted successfully! Total: %llu MB, Free: %llu MB",
                 total / (1024 * 1024), free / (1024 * 1024));
    } else {
        ESP_LOGI(TAG, "Storage mounted at %s", USB_STORAGE_MOUNT_POINT);
    }

    return ESP_OK;
}

static void do_unmount_device(bool device_disconnected)
{
    if (s_vfs_handle) {
        ESP_LOGI(TAG, "Unmounting VFS %s...", USB_STORAGE_MOUNT_POINT);
        msc_host_vfs_unregister(s_vfs_handle);
        s_vfs_handle = NULL;
    }

    if (device_disconnected && s_msc_device) {
        ESP_LOGI(TAG, "Uninstalling MSC device...");
        msc_host_uninstall_device(s_msc_device);
        s_msc_device = NULL;
    }

    s_is_mounted = false;
    ESP_LOGI(TAG, "USB storage unmounted cleanly.");
}

static void usb_msc_manager_task(void *arg)
{
    usb_app_msg_t msg;
    while (1) {
        if (xQueueReceive(s_usb_queue, &msg, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
            if (msg.id == MSG_DEVICE_CONNECTED) {
                s_is_connected = true;
                s_current_dev_addr = msg.data.new_dev_address;
                do_mount_device(msg.data.new_dev_address);
            } else if (msg.id == MSG_DEVICE_DISCONNECTED) {
                do_unmount_device(true);
                s_is_connected = false;
                s_current_dev_addr = 0;
            }
            xSemaphoreGive(s_storage_mutex);
        }
    }
}

esp_err_t usb_storage_init(void)
{
    s_storage_mutex = xSemaphoreCreateMutex();
    assert(s_storage_mutex);

    s_usb_queue = xQueueCreate(10, sizeof(usb_app_msg_t));
    assert(s_usb_queue);

    const usb_host_config_t host_config = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
    };
    err = msc_host_install(&msc_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    xTaskCreate(usb_host_lib_task, "usb_host_lib", 4096, NULL, 10, NULL);
    xTaskCreate(usb_msc_manager_task, "usb_msc_mgr", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "USB Host MSC initialized. Waiting for USB flash drives...");
    return ESP_OK;
}

bool usb_storage_is_connected(void)
{
    return s_is_connected;
}

bool usb_storage_is_mounted(void)
{
    return s_is_mounted;
}

const char *usb_storage_get_mount_point(void)
{
    return USB_STORAGE_MOUNT_POINT;
}

esp_err_t usb_storage_get_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_is_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_vfs_fat_info(USB_STORAGE_MOUNT_POINT, total_bytes, free_bytes);
}

esp_err_t usb_storage_eject(void)
{
    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
    do_unmount_device(false);
    xSemaphoreGive(s_storage_mutex);
    return ESP_OK;
}

esp_err_t usb_storage_mount(void)
{
    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
    esp_err_t ret = ESP_FAIL;
    if (s_is_connected && !s_is_mounted && s_current_dev_addr > 0) {
        ret = do_mount_device(s_current_dev_addr);
    }
    xSemaphoreGive(s_storage_mutex);
    return ret;
}
