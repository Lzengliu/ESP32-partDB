#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_camera.h"

esp_err_t camera_mgr_init(void);
esp_err_t camera_mgr_prewarm_async(void);
void camera_mgr_deinit(void);
esp_err_t camera_mgr_capture_jpeg(camera_fb_t **fb);
esp_err_t camera_mgr_capture_scan_jpeg(camera_fb_t **fb);
esp_err_t camera_mgr_capture_jpeg_bytes(uint8_t **out, size_t *out_len);
void camera_mgr_free_jpeg_bytes(uint8_t *buf);
esp_err_t camera_mgr_capture_scan_grayscale(camera_fb_t **fb);
esp_err_t camera_mgr_capture_scan_rgb565(camera_fb_t **fb);
esp_err_t camera_mgr_capture_still_rgb565(camera_fb_t **fb);
esp_err_t camera_mgr_capture_preview_lowmem(camera_fb_t **fb);
esp_err_t camera_mgr_capture_preview_rgb565(camera_fb_t **fb);
esp_err_t camera_mgr_capture_preview_rgb565_lowmem(camera_fb_t **fb);
void camera_mgr_release_frame(camera_fb_t *fb);
void camera_mgr_set_keep_online(bool keep_online);
bool camera_mgr_should_keep_online(void);
bool camera_mgr_is_active(void);
