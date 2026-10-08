#ifndef DOWNLOAD_MANAGER_H
#define DOWNLOAD_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    DL_STATUS_IDLE = 0,
    DL_STATUS_RESOLVING,
    DL_STATUS_CONNECTING,
    DL_STATUS_DOWNLOADING,
    DL_STATUS_COMPLETED,
    DL_STATUS_FAILED,
    DL_STATUS_CANCELLED
} dl_status_code_t;

typedef struct {
    dl_status_code_t status;
    char original_url[512];
    char filename[128];
    int64_t total_bytes;
    int64_t downloaded_bytes;
    uint32_t speed_bytes_per_sec;
    int active_threads;
    char error_msg[128];
    int64_t elapsed_time_sec;
    int64_t start_time_ms;
} dl_progress_t;

esp_err_t download_manager_init(void);
esp_err_t download_manager_start(const char *url, const char *custom_filename);
esp_err_t download_manager_cancel(void);
void download_manager_get_progress(dl_progress_t *progress);
bool download_manager_is_busy(void);

#endif // DOWNLOAD_MANAGER_H
