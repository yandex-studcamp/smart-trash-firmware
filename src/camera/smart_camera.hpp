#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_camera.h"

namespace smart_bin {

esp_err_t smart_camera_init();
esp_err_t smart_camera_capture_jpeg(const uint8_t **jpeg_data, size_t *jpeg_len, camera_fb_t **out_fb, float *capture_ms);
esp_err_t smart_camera_capture_rgb888(uint8_t **rgb_data, uint16_t *width, uint16_t *height, float *capture_ms);
void smart_camera_release(camera_fb_t *fb);
void smart_camera_free_rgb888(uint8_t *rgb_data);
void smart_camera_deinit();

} // namespace smart_bin
