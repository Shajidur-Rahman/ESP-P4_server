#include "http_server.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include <sys/socket.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "ethernet.h"
#include "usb_storage.h"
#include "wifi_manager.h"
#include "web_ui.h"
#include "download_manager.h"
#include "cJSON.h"

static const char *TAG = "http_server";
static httpd_handle_t s_server = NULL;

#define CHUNK_BUFFER_SIZE (64 * 1024)

static void url_decode(char *dst, const char *src, size_t dst_len)
{
    char a, b;
    size_t out = 0;
    while (*src && out + 1 < dst_len) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit((int)a) && isxdigit((int)b))) {
            if (a >= 'a' && a <= 'f') a -= ('a' - 10);
            else if (a >= 'A' && a <= 'F') a -= ('A' - 10);
            else if (a >= '0' && a <= '9') a -= '0';

            if (b >= 'a' && b <= 'f') b -= ('a' - 10);
            else if (b >= 'A' && b <= 'F') b -= ('A' - 10);
            else if (b >= '0' && b <= '9') b -= '0';

            dst[out++] = (char)(16 * a + b);
            src += 3;
        } else if (*src == '+') {
            dst[out++] = ' ';
            src++;
        } else {
            dst[out++] = *src++;
        }
    }
    dst[out] = '\0';
}

static void build_fs_path(char *full_path, size_t max_len, const char *rel_path)
{
    const char *mount = usb_storage_get_mount_point();
    if (!rel_path || strlen(rel_path) == 0 || strcmp(rel_path, "/") == 0) {
        snprintf(full_path, max_len, "%s", mount);
        return;
    }
    if (rel_path[0] == '/') {
        snprintf(full_path, max_len, "%s%s", mount, rel_path);
    } else {
        snprintf(full_path, max_len, "%s/%s", mount, rel_path);
    }
}

static const char *get_mime_type(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot) return "application/octet-stream";

    // Video
    if (strcasecmp(dot, ".mp4") == 0) return "video/mp4";
    if (strcasecmp(dot, ".mkv") == 0) return "video/x-matroska";
    if (strcasecmp(dot, ".webm") == 0) return "video/webm";
    if (strcasecmp(dot, ".avi") == 0) return "video/x-msvideo";
    if (strcasecmp(dot, ".mov") == 0) return "video/quicktime";
    if (strcasecmp(dot, ".ts") == 0) return "video/mp2t";

    // Audio
    if (strcasecmp(dot, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp(dot, ".wav") == 0) return "audio/wav";
    if (strcasecmp(dot, ".ogg") == 0) return "audio/ogg";
    if (strcasecmp(dot, ".flac") == 0) return "audio/flac";
    if (strcasecmp(dot, ".aac") == 0) return "audio/aac";

    // Images & Documents
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    if (strcasecmp(dot, ".webp") == 0) return "image/webp";
    if (strcasecmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(dot, ".pdf") == 0) return "application/pdf";
    if (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".log") == 0) return "text/plain";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) return "text/html";
    return "application/octet-stream";
}

/* GET / */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* GET /api/status */
static esp_err_t status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    uint64_t total_bytes = 0, free_bytes = 0;
    bool mounted = usb_storage_is_mounted();
    bool connected = usb_storage_is_connected();
    if (mounted) {
        usb_storage_get_info(&total_bytes, &free_bytes);
    }

    uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int64_t uptime_sec = esp_timer_get_time() / 1000000;

    char buf[768];
    snprintf(buf, sizeof(buf),
             "{"
             "\"ip\":\"%s\","
             "\"mac\":\"%s\","
             "\"wifi_ip\":\"%s\","
             "\"wifi_ssid\":\"%s\","
             "\"wifi_connected\":%s,"
             "\"hostname\":\"localserver.local\","
             "\"free_heap\":%lu,"
             "\"free_psram\":%lu,"
             "\"uptime\":%lld,"
             "\"usb_connected\":%s,"
             "\"usb_mounted\":%s,"
             "\"mount_path\":\"%s\","
             "\"total_bytes\":%llu,"
             "\"free_bytes\":%llu"
             "}",
             ethernet_get_ip_str(),
             ethernet_get_mac_str(),
             wifi_manager_get_ip_str(),
             wifi_manager_get_ssid(),
             wifi_manager_is_connected() ? "true" : "false",
             (unsigned long)free_heap,
             (unsigned long)free_psram,
             (long long)uptime_sec,
             connected ? "true" : "false",
             mounted ? "true" : "false",
             usb_storage_get_mount_point(),
             (unsigned long long)total_bytes,
             (unsigned long long)free_bytes);

    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* GET /api/wifi - Get Wi-Fi Settings */
static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    wifi_settings_t cfg;
    wifi_manager_load_settings(&cfg);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"ssid\":\"%s\",\"connected\":%s,\"ip\":\"%s\"}",
             cfg.ssid,
             wifi_manager_is_connected() ? "true" : "false",
             wifi_manager_get_ip_str());
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/wifi - Update Wi-Fi SSID & Password */
static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char content[256] = {0};
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No payload");
        return ESP_OK;
    }

    char ssid[64] = {0};
    char pass[64] = {0};

    char *ssid_p = strstr(content, "\"ssid\":\"");
    if (ssid_p) {
        ssid_p += 8;
        char *end_p = strchr(ssid_p, '"');
        if (end_p) {
            size_t len = end_p - ssid_p;
            if (len < sizeof(ssid)) strncpy(ssid, ssid_p, len);
        }
    }

    char *pass_p = strstr(content, "\"password\":\"");
    if (pass_p) {
        pass_p += 12;
        char *end_p = strchr(pass_p, '"');
        if (end_p) {
            size_t len = end_p - pass_p;
            if (len < sizeof(pass)) strncpy(pass, pass_p, len);
        }
    }

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Saving new Wi-Fi credentials: SSID='%s'", ssid);
    wifi_manager_save_settings(ssid, pass);
    wifi_manager_reconnect();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Wi-Fi settings saved. Reconnecting...\"}");
}

/* GET /api/files?path=... */
static esp_err_t files_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (!usb_storage_is_mounted()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Storage unmounted");
        return ESP_OK;
    }

    char query[256] = {0};
    char raw_rel[256] = "/";
    char rel_path[256] = "/";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "path", raw_rel, sizeof(raw_rel)) == ESP_OK) {
            url_decode(rel_path, raw_rel, sizeof(rel_path));
        }
    }

    char full_path[512];
    build_fs_path(full_path, sizeof(full_path), rel_path);

    DIR *dir = opendir(full_path);
    if (!dir) {
        ESP_LOGW(TAG, "opendir failed for '%s' (raw: '%s')", full_path, raw_rel);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Folder not found");
        return ESP_OK;
    }

    httpd_resp_sendstr_chunk(req, "[");
    struct dirent *entry;
    bool first = true;
    char item_path[1024];
    struct stat st;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(item_path, sizeof(item_path), "%s/%s", full_path, entry->d_name);
        bool is_dir = (entry->d_type == DT_DIR);
        size_t size = 0;
        if (stat(item_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) is_dir = true;
            size = st.st_size;
        }

        char item_buf[1024];
        snprintf(item_buf, sizeof(item_buf),
                 "%s{\"name\":\"%s\",\"is_dir\":%s,\"size\":%zu}",
                 first ? "" : ",",
                 entry->d_name,
                 is_dir ? "true" : "false",
                 size);

        httpd_resp_sendstr_chunk(req, item_buf);
        first = false;
    }
    closedir(dir);

    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* GET /api/stream?file=... - Standard HTTP 206 / 200 Partial Content Streaming */
static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!usb_storage_is_mounted()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Storage unmounted");
        return ESP_OK;
    }

    char query[256] = {0};
    char raw_file[256] = {0};
    char file_name[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", raw_file, sizeof(raw_file)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File parameter missing");
        return ESP_OK;
    }
    url_decode(file_name, raw_file, sizeof(file_name));

    char full_path[512];
    build_fs_path(full_path, sizeof(full_path), file_name);

    struct stat st;
    if (stat(full_path, &st) != 0) {
        ESP_LOGW(TAG, "stream stat failed for '%s' (raw '%s')", full_path, raw_file);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_OK;
    }

    size_t file_size = st.st_size;
    size_t start = 0;
    size_t end = file_size > 0 ? (file_size - 1) : 0;
    bool is_range = false;

    char range_hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK) {
        if (strncmp(range_hdr, "bytes=", 6) == 0) {
            char *dash = strchr(range_hdr + 6, '-');
            if (dash) {
                start = strtoull(range_hdr + 6, NULL, 10);
                if (*(dash + 1) != '\0') {
                    end = strtoull(dash + 1, NULL, 10);
                }
                if (end >= file_size) {
                    end = file_size > 0 ? file_size - 1 : 0;
                }
                if (start <= end) {
                    is_range = true;
                }
            }
        }
    }

    size_t length = (end >= start) ? (end - start + 1) : 0;
    int sockfd = httpd_req_to_sockfd(req);

    // Format HTTP headers directly so we don't produce Transfer-Encoding: chunked on 206
    char hdr_buf[512];
    int hdr_len = 0;
    const char *mime = get_mime_type(file_name);

    if (is_range) {
        hdr_len = snprintf(hdr_buf, sizeof(hdr_buf),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: %s\r\n"
            "Content-Range: bytes %zu-%zu/%zu\r\n"
            "Content-Length: %zu\r\n"
            "Accept-Ranges: bytes\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: keep-alive\r\n\r\n",
            mime, start, end, file_size, length);
    } else {
        hdr_len = snprintf(hdr_buf, sizeof(hdr_buf),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Accept-Ranges: bytes\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: keep-alive\r\n\r\n",
            mime, file_size);
    }

    if (send(sockfd, hdr_buf, hdr_len, 0) < 0) {
        fclose(f);
        return ESP_FAIL;
    }

    fseek(f, start, SEEK_SET);

    char *chunk = malloc(CHUNK_BUFFER_SIZE);
    if (!chunk) {
        fclose(f);
        return ESP_FAIL;
    }

    size_t remaining = length;
    while (remaining > 0) {
        size_t to_read = (remaining > CHUNK_BUFFER_SIZE) ? CHUNK_BUFFER_SIZE : remaining;
        size_t read_bytes = fread(chunk, 1, to_read, f);
        if (read_bytes == 0) break;
        int sent = send(sockfd, chunk, read_bytes, 0);
        if (sent <= 0) {
            break;
        }
        remaining -= sent;
    }

    free(chunk);
    fclose(f);
    return ESP_OK;
}

/* GET /api/download?file=... */
static esp_err_t download_handler(httpd_req_t *req)
{
    if (!usb_storage_is_mounted()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Storage unmounted");
        return ESP_OK;
    }

    char query[256] = {0};
    char raw_file[256] = {0};
    char file_name[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", raw_file, sizeof(raw_file)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File parameter missing");
        return ESP_OK;
    }
    url_decode(file_name, raw_file, sizeof(file_name));

    char full_path[512];
    build_fs_path(full_path, sizeof(full_path), file_name);

    struct stat st;
    if (stat(full_path, &st) != 0) {
        ESP_LOGW(TAG, "download stat failed for '%s' (raw '%s')", full_path, raw_file);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_OK;
    }

    const char *base_name = strrchr(file_name, '/');
    base_name = base_name ? base_name + 1 : file_name;

    char disp_hdr[300];
    snprintf(disp_hdr, sizeof(disp_hdr), "attachment; filename=\"%s\"", base_name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp_hdr);
    httpd_resp_set_type(req, get_mime_type(file_name));

    char len_str[32];
    snprintf(len_str, sizeof(len_str), "%zu", (size_t)st.st_size);
    httpd_resp_set_hdr(req, "Content-Length", len_str);

    char *chunk = malloc(CHUNK_BUFFER_SIZE);
    if (!chunk) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, CHUNK_BUFFER_SIZE, f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            ESP_LOGE(TAG, "File transfer interrupted");
            break;
        }
    }
    free(chunk);
    fclose(f);

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* POST /api/upload?path=... */
static esp_err_t upload_handler(httpd_req_t *req)
{
    if (!usb_storage_is_mounted()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Storage unmounted");
        return ESP_OK;
    }

    char query[256] = {0};
    char raw_path[256] = {0};
    char target_path[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", raw_path, sizeof(raw_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path parameter missing");
        return ESP_OK;
    }
    url_decode(target_path, raw_path, sizeof(target_path));

    char full_path[512];
    build_fs_path(full_path, sizeof(full_path), target_path);

    ESP_LOGI(TAG, "Starting file upload to: %s (Total size: %d bytes)", full_path, req->content_len);

    FILE *f = fopen(full_path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create target file");
        return ESP_OK;
    }

    char *chunk = malloc(CHUNK_BUFFER_SIZE);
    if (!chunk) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_recv = (remaining > CHUNK_BUFFER_SIZE) ? CHUNK_BUFFER_SIZE : remaining;
        int received = httpd_req_recv(req, chunk, to_recv);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "File upload socket recv error: %d", received);
            free(chunk);
            fclose(f);
            unlink(full_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Transfer socket failed");
            return ESP_FAIL;
        }

        if (fwrite(chunk, 1, received, f) != (size_t)received) {
            ESP_LOGE(TAG, "File disk write failed. Disk full?");
            free(chunk);
            fclose(f);
            unlink(full_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Disk write error");
            return ESP_FAIL;
        }
        remaining -= received;
    }

    free(chunk);
    fclose(f);
    ESP_LOGI(TAG, "File upload finished successfully: %s", full_path);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

/* POST /api/delete?file=... */
static esp_err_t delete_handler(httpd_req_t *req)
{
    if (!usb_storage_is_mounted()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Storage unmounted");
        return ESP_OK;
    }

    char query[256] = {0};
    char raw_target[256] = {0};
    char target[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", raw_target, sizeof(raw_target)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File parameter missing");
        return ESP_OK;
    }
    url_decode(target, raw_target, sizeof(target));

    char full_path[512];
    build_fs_path(full_path, sizeof(full_path), target);

    struct stat st;
    if (stat(full_path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Target does not exist");
        return ESP_OK;
    }

    int res = 0;
    if (S_ISDIR(st.st_mode)) {
        res = rmdir(full_path);
    } else {
        res = unlink(full_path);
    }

    if (res == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed (folder not empty?)");
        return ESP_OK;
    }
}

/* POST /api/mkdir?path=... */
static esp_err_t mkdir_handler(httpd_req_t *req)
{
    if (!usb_storage_is_mounted()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Storage unmounted");
        return ESP_OK;
    }

    char query[256] = {0};
    char raw_path[256] = {0};
    char path[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", raw_path, sizeof(raw_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path parameter missing");
        return ESP_OK;
    }
    url_decode(path, raw_path, sizeof(path));

    char full_path[512];
    build_fs_path(full_path, sizeof(full_path), path);

    if (mkdir(full_path, 0755) == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Mkdir failed");
        return ESP_OK;
    }
}

/* POST /api/eject */
static esp_err_t eject_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Eject requested from web interface");
    usb_storage_eject();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

/* POST /api/mount */
static esp_err_t mount_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Mount requested from web interface");
    esp_err_t ret = usb_storage_mount();
    httpd_resp_set_type(req, "application/json");
    if (ret == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    } else {
        return httpd_resp_sendstr(req, "{\"status\":\"failed\"}");
    }
}

/* POST /api/url-download/start */
static esp_err_t url_download_start_handler(httpd_req_t *req)
{
    char content[1024] = {0};
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing payload");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(content);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    cJSON *url_item = cJSON_GetObjectItem(root, "url");
    if (!url_item || !cJSON_IsString(url_item) || strlen(url_item->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing URL");
        return ESP_OK;
    }

    cJSON *fn_item = cJSON_GetObjectItem(root, "filename");
    const char *custom_fn = (fn_item && cJSON_IsString(fn_item)) ? fn_item->valuestring : NULL;

    esp_err_t err = download_manager_start(url_item->valuestring, custom_fn);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    } else if (err == ESP_ERR_INVALID_STATE) {
        return httpd_resp_sendstr(req, "{\"status\":\"busy\",\"message\":\"Another download is already in progress\"}");
    } else {
        return httpd_resp_sendstr(req, "{\"status\":\"failed\"}");
    }
}

/* GET /api/url-download/status */
static esp_err_t url_download_status_handler(httpd_req_t *req)
{
    dl_progress_t p;
    download_manager_get_progress(&p);

    const char *status_str = "idle";
    switch (p.status) {
        case DL_STATUS_RESOLVING: status_str = "resolving"; break;
        case DL_STATUS_CONNECTING: status_str = "connecting"; break;
        case DL_STATUS_DOWNLOADING: status_str = "downloading"; break;
        case DL_STATUS_COMPLETED: status_str = "completed"; break;
        case DL_STATUS_FAILED: status_str = "failed"; break;
        case DL_STATUS_CANCELLED: status_str = "cancelled"; break;
        default: status_str = "idle"; break;
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{"
             "\"status\":\"%s\","
             "\"filename\":\"%s\","
             "\"total_bytes\":%lld,"
             "\"downloaded_bytes\":%lld,"
             "\"speed_bytes_sec\":%lu,"
             "\"active_threads\":%d,"
             "\"error_msg\":\"%s\","
             "\"elapsed_sec\":%lld"
             "}",
             status_str,
             p.filename,
             (long long)p.total_bytes,
             (long long)p.downloaded_bytes,
             (unsigned long)p.speed_bytes_per_sec,
             p.active_threads,
             p.error_msg,
             (long long)p.elapsed_time_sec);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/url-download/cancel */
static esp_err_t url_download_cancel_handler(httpd_req_t *req)
{
    download_manager_cancel();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

esp_err_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.stack_size = 12288;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d...", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get_handler },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler },
        { .uri = "/api/files", .method = HTTP_GET, .handler = files_handler },
        { .uri = "/api/stream", .method = HTTP_GET, .handler = stream_handler },
        { .uri = "/api/download", .method = HTTP_GET, .handler = download_handler },
        { .uri = "/api/upload", .method = HTTP_POST, .handler = upload_handler },
        { .uri = "/api/mkdir", .method = HTTP_POST, .handler = mkdir_handler },
        { .uri = "/api/delete", .method = HTTP_POST, .handler = delete_handler },
        { .uri = "/api/eject", .method = HTTP_POST, .handler = eject_handler },
        { .uri = "/api/mount", .method = HTTP_POST, .handler = mount_handler },
        { .uri = "/api/url-download/start", .method = HTTP_POST, .handler = url_download_start_handler },
        { .uri = "/api/url-download/status", .method = HTTP_GET, .handler = url_download_status_handler },
        { .uri = "/api/url-download/cancel", .method = HTTP_POST, .handler = url_download_cancel_handler },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP Server started successfully with Wi-Fi Settings & mDNS support.");
    return ESP_OK;
}

void http_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
