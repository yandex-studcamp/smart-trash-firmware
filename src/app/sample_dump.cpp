#include "sample_dump.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "sample_dump";
constexpr size_t kChunkBytes = 32;
constexpr uint8_t kJpegQuality = 80;

uint32_t g_sample_id = 0;

char hex_digit(uint8_t value)
{
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10);
}

void log_hex_chunk(const uint8_t *data, size_t len)
{
    char hex[(kChunkBytes * 2) + 1] = {};
    for (size_t i = 0; i < len; ++i) {
        hex[2 * i] = hex_digit(static_cast<uint8_t>(data[i] >> 4));
        hex[(2 * i) + 1] = hex_digit(static_cast<uint8_t>(data[i] & 0x0F));
    }
    hex[len * 2] = '\0';
    ESP_LOGE(kTag, "SAMPLE_DATA %s", hex);
    vTaskDelay(1);
}

void log_bytes_as_hex(const uint8_t *data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        const size_t chunk = (len - offset) > kChunkBytes ? kChunkBytes : (len - offset);
        log_hex_chunk(data + offset, chunk);
        offset += chunk;
    }
}

const char *sanitize_kind(const char *kind)
{
    if (kind == nullptr || kind[0] == '\0') {
        return "unknown";
    }
    return kind;
}

} // namespace

namespace smart_bin {

esp_err_t dump_rgb888_sample_with_meta_to_log(const uint8_t *rgb,
                                              uint16_t width,
                                              uint16_t height,
                                              const char *kind,
                                              uint32_t inference_id,
                                              uint16_t original_width,
                                              uint16_t original_height)
{
    if (rgb == nullptr || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ++g_sample_id;

    uint8_t *jpeg_buf = nullptr;
    size_t jpeg_len = 0;
    const size_t rgb_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 3U;
    const bool encode_ok = fmt2jpg(const_cast<uint8_t *>(rgb),
                                   rgb_bytes,
                                   width,
                                   height,
                                   PIXFORMAT_RGB888,
                                   kJpegQuality,
                                   &jpeg_buf,
                                   &jpeg_len);

    if (!encode_ok || jpeg_buf == nullptr || jpeg_len == 0) {
        ESP_LOGE(kTag, "fmt2jpg failed for sample dump");
        if (jpeg_buf != nullptr) {
            free(jpeg_buf);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGE(kTag,
             "SAMPLE_BEGIN id=%u format=jpeg width=%u height=%u kind=%s inference_id=%u original_width=%u original_height=%u bytes=%u",
             static_cast<unsigned>(g_sample_id),
             static_cast<unsigned>(width),
             static_cast<unsigned>(height),
             sanitize_kind(kind),
             static_cast<unsigned>(inference_id),
             static_cast<unsigned>(original_width),
             static_cast<unsigned>(original_height),
             static_cast<unsigned>(jpeg_len));

    log_bytes_as_hex(jpeg_buf, jpeg_len);

    ESP_LOGE(kTag, "SAMPLE_END id=%u", static_cast<unsigned>(g_sample_id));
    free(jpeg_buf);
    return ESP_OK;
}

esp_err_t dump_rgb888_sample_to_log(const uint8_t *rgb, uint16_t width, uint16_t height)
{
    return dump_rgb888_sample_with_meta_to_log(rgb, width, height, "sample", 0, width, height);
}

} // namespace smart_bin
