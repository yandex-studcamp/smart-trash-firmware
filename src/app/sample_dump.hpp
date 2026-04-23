#pragma once

#include <stdint.h>

#include "esp_err.h"

namespace smart_bin {

esp_err_t dump_rgb888_sample_to_log(const uint8_t *rgb, uint16_t width, uint16_t height);
esp_err_t dump_rgb888_sample_with_meta_to_log(const uint8_t *rgb,
                                              uint16_t width,
                                              uint16_t height,
                                              const char *kind,
                                              uint32_t inference_id,
                                              uint16_t original_width,
                                              uint16_t original_height);

} // namespace smart_bin
