#include "download_manager.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb_storage.h"

static const char *TAG = "download_mgr";

#define NUM_THREADS 4
#define CHUNK_SIZE (32 * 1024)

typedef struct {
    int thread_id;
    int64_t start_byte;
    int64_t end_byte;
    int64_t downloaded_bytes;
    char url[1024];
    char temp_path[384];
    bool finished;
    bool success;
} dl_worker_t;

static dl_progress_t s_progress;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_cancel_requested = false;
static bool s_is_running = false;
static dl_worker_t s_workers[NUM_THREADS];
static TaskHandle_t s_coord_task_handle = NULL;

static void sanitize_filename(char *name)
{
    for (int i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c == '&') {
            name[i] = '_';
        }
    }
}

static void resolve_google_drive(const char *input_url, char *output_url, size_t max_len)
{
    // Match https://drive.google.com/file/d/<ID>/... or ?id=<ID>
    const char *id_start = NULL;
    char id_buf[64] = {0};

    const char *d_pos = strstr(input_url, "/file/d/");
    if (d_pos) {
        id_start = d_pos + 8;
    } else {
        const char *param_pos = strstr(input_url, "id=");
        if (param_pos) {
            id_start = param_pos + 3;
        }
    }

    if (id_start) {
        size_t idx = 0;
        while (*id_start && (isalnum((int)*id_start) || *id_start == '-' || *id_start == '_') && idx < sizeof(id_buf) - 1) {
            id_buf[idx++] = *id_start++;
        }
        id_buf[idx] = '\0';

        if (strlen(id_buf) > 10) {
            snprintf(output_url, max_len, "https://drive.usercontent.google.com/download?id=%s&export=download&confirm=t", id_buf);
            ESP_LOGI(TAG, "Google Drive URL resolved to direct link: %s", output_url);
            return;
        }
    }

    strlcpy(output_url, input_url, max_len);
}

static void extract_filename_from_url(const char *url, char *dest, size_t max_len)
{
    const char *last_slash = strrchr(url, '/');
    const char *name_part = last_slash ? last_slash + 1 : url;

    char temp[128];
    strlcpy(temp, name_part, sizeof(temp));

    char *query = strchr(temp, '?');
    if (query) *query = '\0';

    if (strlen(temp) == 0 || strlen(temp) > 100) {
        snprintf(dest, max_len, "download_%lld.bin", esp_timer_get_time() / 1000);
    } else {
        sanitize_filename(temp);
        snprintf(dest, max_len, "%s", temp);
    }
}

static void worker_task(void *arg)
{
    dl_worker_t *w = (dl_worker_t *)arg;
    ESP_LOGI(TAG, "Worker %d started for range %lld - %lld", w->thread_id, w->start_byte, w->end_byte);

    esp_http_client_config_t config = {
        .url = w->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 16384,
        .buffer_size_tx = 4096,
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Worker %d failed to init http client", w->thread_id);
        w->finished = true;
        w->success = false;
        vTaskDelete(NULL);
        return;
    }

    char range_header[64];
    snprintf(range_header, sizeof(range_header), "bytes=%lld-%lld", w->start_byte, w->end_byte);
    esp_http_client_set_header(client, "Range", range_header);
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (ESP32-P4 HighSpeed NAS)");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Worker %d failed to open connection: %s", w->thread_id, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        w->finished = true;
        w->success = false;
        vTaskDelete(NULL);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Worker %d connected. Status %d, Length %d", w->thread_id, status_code, content_len);

    if (status_code != 206 && status_code != 200) {
        ESP_LOGE(TAG, "Worker %d invalid HTTP status %d", w->thread_id, status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        w->finished = true;
        w->success = false;
        vTaskDelete(NULL);
        return;
    }

    FILE *f = fopen(w->temp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Worker %d failed to create %s", w->thread_id, w->temp_path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        w->finished = true;
        w->success = false;
        vTaskDelete(NULL);
        return;
    }

    char *buf = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(CHUNK_SIZE);
    }
    if (!buf) {
        ESP_LOGE(TAG, "Worker %d failed to allocate buffer", w->thread_id);
        fclose(f);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        w->finished = true;
        w->success = false;
        vTaskDelete(NULL);
        return;
    }

    int64_t total_expected = w->end_byte - w->start_byte + 1;
    while (!s_cancel_requested && w->downloaded_bytes < total_expected) {
        int to_read = (int)((total_expected - w->downloaded_bytes) > CHUNK_SIZE ? CHUNK_SIZE : (total_expected - w->downloaded_bytes));
        int read_bytes = esp_http_client_read(client, buf, to_read);
        if (read_bytes <= 0) {
            if (read_bytes == 0 && w->downloaded_bytes >= total_expected) {
                break;
            }
            ESP_LOGW(TAG, "Worker %d read returned %d", w->thread_id, read_bytes);
            break;
        }

        size_t written = fwrite(buf, 1, read_bytes, f);
        if (written != (size_t)read_bytes) {
            ESP_LOGE(TAG, "Worker %d disk write failed", w->thread_id);
            break;
        }

        w->downloaded_bytes += read_bytes;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.downloaded_bytes += read_bytes;
        xSemaphoreGive(s_mutex);
    }

    free(buf);
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    w->success = (!s_cancel_requested && w->downloaded_bytes >= total_expected);
    w->finished = true;
    ESP_LOGI(TAG, "Worker %d finished. Success: %s (%lld / %lld bytes)",
             w->thread_id, w->success ? "YES" : "NO", w->downloaded_bytes, total_expected);
    vTaskDelete(NULL);
}

static void single_stream_download(const char *url, const char *dest_path, int64_t total_expected)
{
    ESP_LOGI(TAG, "Starting single-stream download to %s...", dest_path);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 25000,
        .buffer_size = 32768,
        .buffer_size_tx = 4096,
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = DL_STATUS_FAILED;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg), "HTTP client init failed");
        xSemaphoreGive(s_mutex);
        return;
    }

    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (ESP32-P4 HighSpeed NAS)");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = DL_STATUS_FAILED;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg), "Connect failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        esp_http_client_cleanup(client);
        return;
    }

    int header_len = esp_http_client_fetch_headers(client);
    (void)header_len;
    int status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 400) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = DL_STATUS_FAILED;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg), "Server returned HTTP %d", status_code);
        xSemaphoreGive(s_mutex);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = DL_STATUS_FAILED;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg), "Could not open target file");
        xSemaphoreGive(s_mutex);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    char *buf = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(CHUNK_SIZE);
    if (!buf) {
        fclose(f);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = DL_STATUS_FAILED;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg), "Out of memory for buffer");
        xSemaphoreGive(s_mutex);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_progress.status = DL_STATUS_DOWNLOADING;
    s_progress.active_threads = 1;
    xSemaphoreGive(s_mutex);

    int64_t last_calc_time = esp_timer_get_time();
    int64_t last_calc_bytes = 0;

    while (!s_cancel_requested) {
        int read_bytes = esp_http_client_read(client, buf, CHUNK_SIZE);
        if (read_bytes < 0) {
            ESP_LOGE(TAG, "Socket read error: %d", read_bytes);
            break;
        }
        if (read_bytes == 0) {
            // End of stream
            break;
        }

        if (fwrite(buf, 1, read_bytes, f) != (size_t)read_bytes) {
            ESP_LOGE(TAG, "Disk write error");
            break;
        }

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.downloaded_bytes += read_bytes;

        int64_t now = esp_timer_get_time();
        if ((now - last_calc_time) >= 1000000) {
            int64_t diff = s_progress.downloaded_bytes - last_calc_bytes;
            s_progress.speed_bytes_per_sec = (uint32_t)(diff * 1000000 / (now - last_calc_time));
            last_calc_time = now;
            last_calc_bytes = s_progress.downloaded_bytes;
            s_progress.elapsed_time_sec = (now - s_progress.start_time_ms * 1000) / 1000000;
        }
        xSemaphoreGive(s_mutex);
    }

    free(buf);
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_cancel_requested) {
        s_progress.status = DL_STATUS_CANCELLED;
        unlink(dest_path);
    } else if (total_expected > 0 && s_progress.downloaded_bytes < total_expected) {
        s_progress.status = DL_STATUS_FAILED;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg), "Connection closed prematurely");
        unlink(dest_path);
    } else {
        s_progress.status = DL_STATUS_COMPLETED;
    }
    s_progress.speed_bytes_per_sec = 0;
    xSemaphoreGive(s_mutex);
}

static void coordinator_task(void *arg)
{
    char active_url[1024];
    char target_filename[128];

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_progress.status = DL_STATUS_RESOLVING;
    s_progress.downloaded_bytes = 0;
    s_progress.total_bytes = 0;
    s_progress.speed_bytes_per_sec = 0;
    s_progress.active_threads = 0;
    s_progress.error_msg[0] = '\0';
    s_progress.start_time_ms = esp_timer_get_time() / 1000;
    strlcpy(active_url, s_progress.original_url, sizeof(active_url));
    strlcpy(target_filename, s_progress.filename, sizeof(target_filename));
    xSemaphoreGive(s_mutex);

    if (!usb_storage_is_mounted()) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = DL_STATUS_FAILED;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg), "USB storage not mounted");
        s_is_running = false;
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }

    // Resolve Google Drive if applicable
    char resolved_url[1024];
    resolve_google_drive(active_url, resolved_url, sizeof(resolved_url));

    // Probe server for Content-Length and Range support
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_progress.status = DL_STATUS_CONNECTING;
    xSemaphoreGive(s_mutex);

    esp_http_client_config_t probe_cfg = {
        .url = resolved_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t probe_client = esp_http_client_init(&probe_cfg);

    int64_t total_size = -1;
    bool supports_ranges = false;

    if (probe_client) {
        esp_http_client_set_header(probe_client, "Range", "bytes=0-0");
        esp_http_client_set_header(probe_client, "User-Agent", "Mozilla/5.0 (ESP32-P4 HighSpeed NAS)");

        if (esp_http_client_open(probe_client, 0) == ESP_OK) {
            esp_http_client_fetch_headers(probe_client);
            int code = esp_http_client_get_status_code(probe_client);

            if (code == 206) {
                supports_ranges = true;
                // Parse Content-Range: bytes 0-0/TOTAL
                char range_val[128] = {0};
                if (esp_http_client_get_header(probe_client, "Content-Range", (char **)&range_val) == ESP_OK || true) {
                    char cr_buf[128] = {0};
                    int len = esp_http_client_read(probe_client, cr_buf, 1);
                    (void)len;
                }
            }
            total_size = esp_http_client_get_content_length(probe_client);
            esp_http_client_close(probe_client);
        }
        esp_http_client_cleanup(probe_client);
    }

    if (strlen(target_filename) == 0) {
        extract_filename_from_url(resolved_url, target_filename, sizeof(target_filename));
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        strlcpy(s_progress.filename, target_filename, sizeof(s_progress.filename));
        xSemaphoreGive(s_mutex);
    }

    char final_dest_path[384];
    snprintf(final_dest_path, sizeof(final_dest_path), "%s/%s", usb_storage_get_mount_point(), target_filename);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_progress.total_bytes = total_size;
    xSemaphoreGive(s_mutex);

    // Multi-threaded segmented download if size > 4MB and Range supported
    if (supports_ranges && total_size > (4 * 1024 * 1024)) {
        ESP_LOGI(TAG, "Multi-threaded download enabled: %lld bytes across %d threads", total_size, NUM_THREADS);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_progress.status = DL_STATUS_DOWNLOADING;
        s_progress.active_threads = NUM_THREADS;
        xSemaphoreGive(s_mutex);

        int64_t seg_size = total_size / NUM_THREADS;
        for (int i = 0; i < NUM_THREADS; i++) {
            s_workers[i].thread_id = i;
            s_workers[i].start_byte = i * seg_size;
            s_workers[i].end_byte = (i == NUM_THREADS - 1) ? (total_size - 1) : ((i + 1) * seg_size - 1);
            s_workers[i].downloaded_bytes = 0;
            s_workers[i].finished = false;
            s_workers[i].success = false;
            strlcpy(s_workers[i].url, resolved_url, sizeof(s_workers[i].url));
            snprintf(s_workers[i].temp_path, sizeof(s_workers[i].temp_path), "%s/%s.part%d", usb_storage_get_mount_point(), target_filename, i);

            char task_name[16];
            snprintf(task_name, sizeof(task_name), "dl_seg_%d", i);
            xTaskCreate(worker_task, task_name, 6144, &s_workers[i], 5, NULL);
        }

        // Monitor workers
        int64_t last_calc_time = esp_timer_get_time();
        int64_t last_calc_bytes = 0;

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(500));

            int64_t now = esp_timer_get_time();
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if ((now - last_calc_time) >= 1000000) {
                int64_t diff = s_progress.downloaded_bytes - last_calc_bytes;
                s_progress.speed_bytes_per_sec = (uint32_t)(diff * 1000000 / (now - last_calc_time));
                last_calc_time = now;
                last_calc_bytes = s_progress.downloaded_bytes;
                s_progress.elapsed_time_sec = (now - s_progress.start_time_ms * 1000) / 1000000;
            }
            xSemaphoreGive(s_mutex);

            bool all_done = true;
            for (int i = 0; i < NUM_THREADS; i++) {
                if (!s_workers[i].finished) {
                    all_done = false;
                    break;
                }
            }
            if (all_done || s_cancel_requested) break;
        }

        // Check if all succeeded
        bool all_ok = true;
        for (int i = 0; i < NUM_THREADS; i++) {
            if (!s_workers[i].success) all_ok = false;
        }

        if (all_ok && !s_cancel_requested) {
            ESP_LOGI(TAG, "All parts downloaded. Assembling into %s...", final_dest_path);
            FILE *f_dest = fopen(final_dest_path, "wb");
            if (f_dest) {
                char *merge_buf = malloc(64 * 1024);
                if (!merge_buf) merge_buf = malloc(16 * 1024);

                for (int i = 0; i < NUM_THREADS; i++) {
                    FILE *f_part = fopen(s_workers[i].temp_path, "rb");
                    if (f_part && merge_buf) {
                        size_t r;
                        while ((r = fread(merge_buf, 1, 64 * 1024, f_part)) > 0) {
                            fwrite(merge_buf, 1, r, f_dest);
                        }
                        fclose(f_part);
                    }
                    unlink(s_workers[i].temp_path);
                }
                if (merge_buf) free(merge_buf);
                fclose(f_dest);

                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_progress.status = DL_STATUS_COMPLETED;
                s_progress.speed_bytes_per_sec = 0;
                xSemaphoreGive(s_mutex);
                ESP_LOGI(TAG, "Download assembled successfully!");
            } else {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_progress.status = DL_STATUS_FAILED;
                snprintf(s_progress.error_msg, sizeof(s_progress.error_msg), "Failed to assemble final file");
                xSemaphoreGive(s_mutex);
            }
        } else {
            for (int i = 0; i < NUM_THREADS; i++) {
                unlink(s_workers[i].temp_path);
            }
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (s_cancel_requested) {
                s_progress.status = DL_STATUS_CANCELLED;
            } else {
                s_progress.status = DL_STATUS_FAILED;
                snprintf(s_progress.error_msg, sizeof(s_progress.error_msg), "One or more segments failed");
            }
            s_progress.speed_bytes_per_sec = 0;
            xSemaphoreGive(s_mutex);
        }
    } else {
        // Single stream download
        single_stream_download(resolved_url, final_dest_path, total_size);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_is_running = false;
    xSemaphoreGive(s_mutex);

    vTaskDelete(NULL);
}

esp_err_t download_manager_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress.status = DL_STATUS_IDLE;
    return ESP_OK;
}

esp_err_t download_manager_start(const char *url, const char *custom_filename)
{
    if (!url || strlen(url) == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_is_running) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_is_running = true;
    s_cancel_requested = false;
    memset(&s_progress, 0, sizeof(s_progress));
    strlcpy(s_progress.original_url, url, sizeof(s_progress.original_url));
    if (custom_filename && strlen(custom_filename) > 0) {
        strlcpy(s_progress.filename, custom_filename, sizeof(s_progress.filename));
        sanitize_filename(s_progress.filename);
    }
    xSemaphoreGive(s_mutex);

    BaseType_t ret = xTaskCreate(coordinator_task, "dl_coord", 8192, NULL, 5, &s_coord_task_handle);
    if (ret != pdPASS) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_is_running = false;
        s_progress.status = DL_STATUS_FAILED;
        snprintf(s_progress.error_msg, sizeof(s_progress.error_msg), "Task creation failed");
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t download_manager_cancel(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_is_running) {
        s_cancel_requested = true;
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void download_manager_get_progress(dl_progress_t *progress)
{
    if (!progress) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(progress, &s_progress, sizeof(dl_progress_t));
    xSemaphoreGive(s_mutex);
}

bool download_manager_is_busy(void)
{
    bool busy;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    busy = s_is_running;
    xSemaphoreGive(s_mutex);
    return busy;
}
