#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_config.h"
#include "esp_err.h"

typedef struct {
    bool configured;
    bool last_ok;
    bool last_cache_hit;
    bool last_cache_sd;
    int last_http_status;
    size_t last_response_len;
} partdb_client_status_t;

typedef enum {
    PARTDB_HTTP_GET = 0,
    PARTDB_HTTP_POST,
    PARTDB_HTTP_PATCH,
} partdb_http_method_t;

esp_err_t partdb_client_init(const app_config_t *cfg);
partdb_client_status_t partdb_client_get_status(void);
esp_err_t partdb_client_request_json(partdb_http_method_t method, const char *path,
                                     const char *body, const char *content_type,
                                     char *out, size_t out_len, int *http_status);
esp_err_t partdb_client_get_json(const char *path, char *out, size_t out_len, int *http_status);
esp_err_t partdb_client_patch_json(const char *path, const char *body,
                                   char *out, size_t out_len, int *http_status);
const char *partdb_client_last_source(void);
