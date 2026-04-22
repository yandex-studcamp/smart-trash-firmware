#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int predicted_class;
    const char *predicted_label;
    bool predicted_nothing;
    float confidence;
    float scores[4];
    uint8_t score_count;
    uint16_t input_width;
    uint16_t input_height;
    float decode_ms;
    float preprocess_ms;
    float infer_ms;
    float total_ms;
} inference_result_t;

esp_err_t inference_init(void);
bool inference_is_ready(void);
esp_err_t inference_run_jpeg(const uint8_t *data, size_t len, inference_result_t *out);
esp_err_t inference_run_rgb888(const uint8_t *rgb, uint16_t width, uint16_t height, inference_result_t *out);

#ifdef __cplusplus
}
#endif
