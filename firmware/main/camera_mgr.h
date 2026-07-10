#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_camera.h"

esp_err_t camera_mgr_init(void);
esp_err_t camera_mgr_prewarm_async(void);
void camera_mgr_deinit(void);
esp_err_t camera_mgr_capture_jpeg(camera_fb_t **fb);
esp_err_t camera_mgr_capture_scan_jpeg(camera_fb_t **fb);
esp_err_t camera_mgr_capture_grayscale(camera_fb_t **fb);
void camera_mgr_release_frame(camera_fb_t *fb);
bool camera_mgr_is_active(void);
