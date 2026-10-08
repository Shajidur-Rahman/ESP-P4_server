#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_STORAGE_MOUNT_POINT "/usb0"

esp_err_t usb_storage_init(void);
bool usb_storage_is_connected(void);
bool usb_storage_is_mounted(void);
const char *usb_storage_get_mount_point(void);
esp_err_t usb_storage_get_info(uint64_t *total_bytes, uint64_t *free_bytes);
esp_err_t usb_storage_eject(void);
esp_err_t usb_storage_mount(void);

#ifdef __cplusplus
}
#endif
