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

#ifndef CONFIG_SMART_SAMPLE_DUMP_OUTPUT_SIDE
#define CONFIG_SMART_SAMPLE_DUMP_OUTPUT_SIDE 128
#endif

namespace {

constexpr char kTag[] = "sample_dump";
constexpr size_t kChunkBytes = 32;
constexpr uint8_t kJpegQuality = 80;
constexpr uint16_t kSampleDumpSide = CONFIG_SMART_SAMPLE_DUMP_OUTPUT_SIDE;

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

esp_err_t downscale_rgb888_center_square(const uint8_t *rgb,
                                         uint16_t src_width,
                                         uint16_t src_height,
                                         uint16_t out_side,
                                         uint8_t **out_rgb)
{
    if (out_rgb == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t out_bytes = static_cast<size_t>(out_side) * static_cast<size_t>(out_side) * 3U;
    uint8_t *dst = static_cast<uint8_t *>(heap_caps_malloc(out_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (dst == nullptr) {
        dst = static_cast<uint8_t *>(heap_caps_malloc(out_bytes, MALLOC_CAP_8BIT));
    }
    if (dst == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const uint16_t crop_side = src_width < src_height ? src_width : src_height;
    const uint16_t crop_x0 = static_cast<uint16_t>((src_width - crop_side) / 2U);
    const uint16_t crop_y0 = static_cast<uint16_t>((src_height - crop_side) / 2U);

    for (uint16_t y = 0; y < out_side; ++y) {
        const uint16_t src_y =
            static_cast<uint16_t>(crop_y0 + (static_cast<uint32_t>(y) * static_cast<uint32_t>(crop_side)) / out_side);
        for (uint16_t x = 0; x < out_side; ++x) {
            const uint16_t src_x =
                static_cast<uint16_t>(crop_x0 + (static_cast<uint32_t>(x) * static_cast<uint32_t>(crop_side)) / out_side);
            const size_t src_offset = (static_cast<size_t>(src_y) * static_cast<size_t>(src_width) +
                                       static_cast<size_t>(src_x)) *
                                      3U;
            const size_t dst_offset = (static_cast<size_t>(y) * static_cast<size_t>(out_side) +
                                       static_cast<size_t>(x)) *
                                      3U;

            dst[dst_offset + 0] = rgb[src_offset + 0];
            dst[dst_offset + 1] = rgb[src_offset + 1];
            dst[dst_offset + 2] = rgb[src_offset + 2];
        }
    }

    *out_rgb = dst;
    return ESP_OK;
}

} // namespace

namespace smart_bin {

esp_err_t dump_rgb888_sample_to_log(const uint8_t *rgb, uint16_t width, uint16_t height)
{
    if (rgb == nullptr || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t out_width = kSampleDumpSide;
    const uint16_t out_height = kSampleDumpSide;
    ++g_sample_id;

    uint8_t *downscaled_rgb = nullptr;
    const esp_err_t downscale_ret = downscale_rgb888_center_square(rgb, width, height, kSampleDumpSide, &downscaled_rgb);
    if (downscale_ret != ESP_OK) {
        ESP_LOGE(kTag, "Downscale failed: 0x%x", downscale_ret);
        return downscale_ret;
    }

    uint8_t *jpeg_buf = nullptr;
    size_t jpeg_len = 0;
    const size_t downscaled_bytes = static_cast<size_t>(out_width) * static_cast<size_t>(out_height) * 3U;
    const bool encode_ok =
        fmt2jpg(downscaled_rgb, downscaled_bytes, out_width, out_height, PIXFORMAT_RGB888, kJpegQuality, &jpeg_buf, &jpeg_len);
    heap_caps_free(downscaled_rgb);

    if (!encode_ok || jpeg_buf == nullptr || jpeg_len == 0) {
        ESP_LOGE(kTag, "fmt2jpg failed for sample dump");
        if (jpeg_buf != nullptr) {
            free(jpeg_buf);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGE(kTag,
             "SAMPLE_BEGIN id=%u format=jpeg width=%u height=%u original_width=%u original_height=%u bytes=%u",
             static_cast<unsigned>(g_sample_id),
             static_cast<unsigned>(out_width),
             static_cast<unsigned>(out_height),
             static_cast<unsigned>(width),
             static_cast<unsigned>(height),
             static_cast<unsigned>(jpeg_len));

    log_bytes_as_hex(jpeg_buf, jpeg_len);

    ESP_LOGE(kTag, "SAMPLE_END id=%u", static_cast<unsigned>(g_sample_id));
    free(jpeg_buf);
    return ESP_OK;
}

} // namespace smart_bin
