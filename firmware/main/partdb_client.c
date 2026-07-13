#include "partdb_client.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "board_config.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "storage_sd.h"

static const char *TAG = "partdb";
static app_config_t s_cfg;
static bool s_configured;
static bool s_last_ok;
static bool s_last_cache_hit;
static bool s_last_cache_sd;
static int s_last_http_status;
static size_t s_last_response_len;
static const char *s_last_source = "none";
static SemaphoreHandle_t s_request_mutex;

#define PARTDB_RAM_CACHE_MAX 8192

static bool s_ram_cache_valid;
static uint32_t s_ram_cache_key;
static char s_ram_cache_body[PARTDB_RAM_CACHE_MAX];
static size_t s_ram_cache_len;

esp_err_t partdb_client_init(const app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_request_mutex) {
        s_request_mutex = xSemaphoreCreateMutex();
        if (!s_request_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_request_mutex, portMAX_DELAY);
    s_cfg = *cfg;
    s_configured = app_config_has_partdb(&s_cfg);
    s_last_cache_hit = false;
    s_last_cache_sd = false;
    s_last_response_len = 0;
    s_last_source = "none";
    xSemaphoreGive(s_request_mutex);
    return ESP_OK;
}

partdb_client_status_t partdb_client_get_status(void)
{
    return (partdb_client_status_t) {
        .configured = s_configured,
        .last_ok = s_last_ok,
        .last_cache_hit = s_last_cache_hit,
        .last_cache_sd = s_last_cache_sd,
        .last_http_status = s_last_http_status,
        .last_response_len = s_last_response_len,
    };
}

const char *partdb_client_last_source(void)
{
    return s_last_source;
}

static esp_err_t build_url(const char *path, char *out, size_t out_len)
{
    if (!s_configured || !path || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        int n = snprintf(out, out_len, "%s", path);
        return n > 0 && n < (int)out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    char base[APP_CONFIG_URL_LEN];
    snprintf(base, sizeof(base), "%s", s_cfg.partdb_url);
    size_t base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1] == '/') {
        base[--base_len] = '\0';
    }

    bool base_is_api = base_len >= 4 && strcmp(base + base_len - 4, "/api") == 0;
    const char *endpoint = path;
    if (strncmp(endpoint, "/api/", 5) == 0) {
        endpoint = base_is_api ? endpoint + 4 : endpoint;
    } else if (strcmp(endpoint, "/api") == 0) {
        endpoint = base_is_api ? "" : "/api";
    } else if (!base_is_api) {
        char normalized[512];
        const char *slash = endpoint[0] == '/' ? "" : "/";
        int n = snprintf(normalized, sizeof(normalized), "/api%s%s", slash, endpoint);
        if (n < 0 || n >= (int)sizeof(normalized)) {
            return ESP_ERR_INVALID_SIZE;
        }
        n = snprintf(out, out_len, "%s%s", base, normalized);
        return n > 0 && n < (int)out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    const char *slash = endpoint[0] == '/' || endpoint[0] == '\0' ? "" : "/";
    int n = snprintf(out, out_len, "%s%s%s", base, slash, endpoint);
    return n > 0 && n < (int)out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static uint32_t fnv1a_update(uint32_t hash, const char *s)
{
    while (s && *s) {
        hash ^= (uint8_t)*s++;
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t cache_key_for_path(const char *path)
{
    uint32_t hash = 2166136261u;
    hash = fnv1a_update(hash, s_cfg.partdb_url);
    hash = fnv1a_update(hash, "|");
    hash = fnv1a_update(hash, path);
    return hash;
}

static esp_err_t cache_file_for_key(uint32_t key, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(out, out_len, "%s/pdb_%08lx.json",
                           BOARD_SD_PARTDB_CACHE_DIR, (unsigned long)key);
    return written > 0 && written < (int)out_len ? ESP_OK : ESP_ERR_NO_MEM;
}

static void write_ram_cache(uint32_t key, const char *body)
{
    size_t len = body ? strlen(body) : 0;
    if (!body || len == 0 || len >= sizeof(s_ram_cache_body)) {
        return;
    }
    s_ram_cache_key = key;
    memcpy(s_ram_cache_body, body, len + 1);
    s_ram_cache_len = len;
    s_ram_cache_valid = true;
}

static bool read_ram_cache(uint32_t key, char *out, size_t out_len)
{
    if (!out || out_len == 0 || !s_ram_cache_valid || s_ram_cache_key != key ||
        s_ram_cache_len >= out_len) {
        return false;
    }
    memcpy(out, s_ram_cache_body, s_ram_cache_len + 1);
    return true;
}

static esp_err_t write_sd_cache(uint32_t key, const char *body)
{
    if (!body || body[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = storage_sd_prepare_paths();
    if (err != ESP_OK) {
        return err;
    }
    char file[160];
    err = cache_file_for_key(key, file, sizeof(file));
    if (err != ESP_OK) {
        return err;
    }
    FILE *f = fopen(file, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    size_t len = strlen(body);
    bool ok = fwrite(body, 1, len, f) == len;
    fclose(f);
    return ok ? ESP_OK : ESP_FAIL;
}

static bool read_sd_cache(uint32_t key, char *out, size_t out_len)
{
    if (!out || out_len == 0 || storage_sd_prepare_paths() != ESP_OK) {
        return false;
    }
    char file[160];
    if (cache_file_for_key(key, file, sizeof(file)) != ESP_OK) {
        return false;
    }
    struct stat st;
    if (stat(file, &st) != 0 || S_ISDIR(st.st_mode) || st.st_size <= 0 ||
        st.st_size >= (off_t)out_len) {
        return false;
    }
    FILE *f = fopen(file, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(out, 1, (size_t)st.st_size, f);
    fclose(f);
    if (n != (size_t)st.st_size) {
        return false;
    }
    out[n] = '\0';
    return true;
}

static void store_response_cache(uint32_t key, const char *body)
{
    if (write_sd_cache(key, body) == ESP_OK) {
        s_last_cache_sd = true;
        return;
    }
    s_last_cache_sd = false;
    write_ram_cache(key, body);
}

static bool read_cached_response(uint32_t key, char *out, size_t out_len)
{
    if (read_sd_cache(key, out, out_len)) {
        s_last_cache_hit = true;
        s_last_cache_sd = true;
        s_last_source = "sd";
        return true;
    }
    if (read_ram_cache(key, out, out_len)) {
        s_last_cache_hit = true;
        s_last_cache_sd = false;
        s_last_source = "ram";
        return true;
    }
    return false;
}

static esp_http_client_method_t to_esp_method(partdb_http_method_t method)
{
    switch (method) {
    case PARTDB_HTTP_POST:
        return HTTP_METHOD_POST;
    case PARTDB_HTTP_PATCH:
        return HTTP_METHOD_PATCH;
    case PARTDB_HTTP_GET:
    default:
        return HTTP_METHOD_GET;
    }
}

static esp_err_t request_json_unlocked(partdb_http_method_t method, const char *path,
                                       const char *body, const char *content_type,
                                       char *out, size_t out_len, int *http_status)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    s_last_cache_hit = false;
    s_last_cache_sd = false;
    s_last_http_status = 0;
    s_last_response_len = 0;
    s_last_source = "network";
    if (http_status) {
        *http_status = 0;
    }

    char url[APP_CONFIG_URL_LEN + 1536];
    ESP_RETURN_ON_ERROR(build_url(path, url, sizeof(url)), TAG, "url build failed");

    esp_http_client_config_t config = {
        .url = url,
        .method = to_esp_method(method),
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    char auth[APP_CONFIG_TOKEN_LEN + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", s_cfg.partdb_token);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "application/ld+json, application/json");
    if (body && body[0]) {
        esp_http_client_set_header(client, "Content-Type",
                                   content_type && content_type[0] ? content_type : "application/json");
    }

    int body_len = body && body[0] ? (int)strlen(body) : 0;
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err == ESP_OK && body_len > 0) {
        int written = esp_http_client_write(client, body, body_len);
        if (written != body_len) {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        int content_length = esp_http_client_fetch_headers(client);
        (void)content_length;
        int read_total = 0;
        while (read_total < (int)out_len - 1) {
            int r = esp_http_client_read(client, out + read_total, out_len - 1 - read_total);
            if (r <= 0) {
                break;
            }
            read_total += r;
        }
        out[read_total] = '\0';
        s_last_http_status = esp_http_client_get_status_code(client);
        s_last_ok = s_last_http_status >= 200 && s_last_http_status < 300;
        s_last_response_len = (size_t)read_total;
        if (s_last_ok && read_total > 0) {
            if (method == PARTDB_HTTP_GET) {
                store_response_cache(cache_key_for_path(path), out);
            }
        }
        if (http_status) {
            *http_status = s_last_http_status;
        }
    } else {
        s_last_ok = false;
        ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return err;
}

esp_err_t partdb_client_request_json(partdb_http_method_t method, const char *path,
                                     const char *body, const char *content_type,
                                     char *out, size_t out_len, int *http_status)
{
    if (!s_request_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_request_mutex, portMAX_DELAY);
    esp_err_t err = request_json_unlocked(method, path, body, content_type,
                                          out, out_len, http_status);
    xSemaphoreGive(s_request_mutex);
    return err;
}

static esp_err_t post_binary_unlocked(const char *path, const char *content_type,
                                      const uint8_t *data, size_t data_len,
                                      char *out, size_t out_len, int *http_status)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    s_last_cache_hit = false;
    s_last_cache_sd = false;
    s_last_http_status = 0;
    s_last_response_len = 0;
    s_last_source = "network";
    if (http_status) {
        *http_status = 0;
    }
    if (!s_configured || !path || !data || data_len == 0 || data_len > INT_MAX) {
        s_last_ok = false;
        return ESP_ERR_INVALID_ARG;
    }

    char url[APP_CONFIG_URL_LEN + 1536];
    ESP_RETURN_ON_ERROR(build_url(path, url, sizeof(url)), TAG, "url build failed");

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        s_last_ok = false;
        return ESP_ERR_NO_MEM;
    }

    char auth[APP_CONFIG_TOKEN_LEN + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", s_cfg.partdb_token);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type",
                               content_type && content_type[0] ? content_type : "application/octet-stream");
    esp_http_client_set_header(client, "X-PartDB-Terminal-Frame", "camera");

    esp_err_t err = esp_http_client_open(client, (int)data_len);
    size_t written_total = 0;
    while (err == ESP_OK && written_total < data_len) {
        size_t remaining = data_len - written_total;
        int chunk = remaining > 1024 ? 1024 : (int)remaining;
        int written = esp_http_client_write(client, (const char *)data + written_total, chunk);
        if (written <= 0) {
            err = ESP_FAIL;
            break;
        }
        written_total += (size_t)written;
    }

    if (err == ESP_OK && written_total != data_len) {
        err = ESP_FAIL;
    }

    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        int read_total = 0;
        while (read_total < (int)out_len - 1) {
            int r = esp_http_client_read(client, out + read_total, out_len - 1 - read_total);
            if (r <= 0) {
                break;
            }
            read_total += r;
        }
        out[read_total] = '\0';
        s_last_http_status = esp_http_client_get_status_code(client);
        s_last_ok = s_last_http_status >= 200 && s_last_http_status < 300;
        s_last_response_len = (size_t)read_total;
        if (http_status) {
            *http_status = s_last_http_status;
        }
        ESP_LOGI(TAG, "POST %s binary len=%u status=%d response=%u",
                 url, (unsigned)data_len, s_last_http_status, (unsigned)s_last_response_len);
    } else {
        s_last_ok = false;
        ESP_LOGW(TAG, "POST %s binary failed after %u/%u bytes: %s",
                 url, (unsigned)written_total, (unsigned)data_len, esp_err_to_name(err));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return err;
}

esp_err_t partdb_client_post_binary(const char *path, const char *content_type,
                                    const uint8_t *data, size_t data_len,
                                    char *out, size_t out_len, int *http_status)
{
    if (!s_request_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_request_mutex, portMAX_DELAY);
    esp_err_t err = post_binary_unlocked(path, content_type, data, data_len,
                                         out, out_len, http_status);
    xSemaphoreGive(s_request_mutex);
    return err;
}

esp_err_t partdb_client_get_json(const char *path, char *out, size_t out_len, int *http_status)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_configured || !path) {
        out[0] = '\0';
        if (http_status) {
            *http_status = 0;
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_request_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_request_mutex, portMAX_DELAY);
    uint32_t cache_key = cache_key_for_path(path);
    esp_err_t err = request_json_unlocked(PARTDB_HTTP_GET, path, NULL, NULL,
                                          out, out_len, http_status);

    if ((err != ESP_OK || s_last_http_status < 200 || s_last_http_status >= 300) &&
        read_cached_response(cache_key, out, out_len)) {
        s_last_http_status = 200;
        s_last_ok = true;
        s_last_response_len = strlen(out);
        if (http_status) {
            *http_status = s_last_http_status;
        }
        ESP_LOGI(TAG, "GET %s served from %s cache", path, s_last_source);
        xSemaphoreGive(s_request_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(s_request_mutex);
    return err;
}

esp_err_t partdb_client_patch_json(const char *path, const char *body,
                                   char *out, size_t out_len, int *http_status)
{
    return partdb_client_request_json(PARTDB_HTTP_PATCH, path, body,
                                      "application/merge-patch+json",
                                      out, out_len, http_status);
}
