#include "camera.hpp"

#include "board/board_config.hpp"
#include "board/board_pins.hpp"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "sdkconfig.h"

namespace {

namespace board = smart_bin::board;

constexpr char kTag[] = "camera";
bool g_camera_ready = false;
bool g_first_frame_discarded = false;

framesize_t select_frame_size()
{
#if CONFIG_SMART_CAMERA_FRAME_SIZE_QQVGA
    return FRAMESIZE_QQVGA;
#elif CONFIG_SMART_CAMERA_FRAME_SIZE_QVGA
    return FRAMESIZE_QVGA;
#elif CONFIG_SMART_CAMERA_FRAME_SIZE_VGA
    return FRAMESIZE_VGA;
#else
    return FRAMESIZE_QVGA;
#endif
}

} // namespace

namespace smart_bin {

esp_err_t camera_init()
{
    if (g_camera_ready) {
        return ESP_OK;
    }

    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = board::kCameraPinD0;
    config.pin_d1 = board::kCameraPinD1;
    config.pin_d2 = board::kCameraPinD2;
    config.pin_d3 = board::kCameraPinD3;
    config.pin_d4 = board::kCameraPinD4;
    config.pin_d5 = board::kCameraPinD5;
    config.pin_d6 = board::kCameraPinD6;
    config.pin_d7 = board::kCameraPinD7;
    config.pin_xclk = board::kCameraPinXclk;
    config.pin_pclk = board::kCameraPinPclk;
    config.pin_vsync = board::kCameraPinVsync;
    config.pin_href = board::kCameraPinHref;
    config.pin_sccb_sda = board::kCameraPinSiod;
    config.pin_sccb_scl = board::kCameraPinSioc;
    config.pin_pwdn = board::kCameraPinPwdn;
    config.pin_reset = board::kCameraPinReset;
    config.xclk_freq_hz = board::kCameraXclkHz;
    config.frame_size = select_frame_size();
#if CONFIG_SMART_BOOT_PROFILE_CAMERA_CAPTURE_SERVICE || CONFIG_SMART_ENABLE_HTTP_DEBUG_API
    config.pixel_format = PIXFORMAT_JPEG;
#else
    config.pixel_format = PIXFORMAT_RGB565;
#endif
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = board::kCameraJpegQuality;
    config.fb_count = board::kCameraFbCount;

    ESP_LOGI(kTag,
             "Init camera pins: xclk=%d pclk=%d vsync=%d href=%d pwdn=%d reset=%d",
             config.pin_xclk,
             config.pin_pclk,
             config.pin_vsync,
             config.pin_href,
             config.pin_pwdn,
             config.pin_reset);

    const esp_err_t ret = esp_camera_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "esp_camera_init failed: 0x%x", ret);
        return ret;
    }

    g_camera_ready = true;
    g_first_frame_discarded = false;
    ESP_LOGI(kTag, "Camera initialized");
    return ESP_OK;
}

esp_err_t camera_capture_jpeg(const uint8_t **jpeg_data, size_t *jpeg_len, camera_fb_t **out_fb, float *capture_ms)
{
    if (!g_camera_ready || jpeg_data == nullptr || jpeg_len == nullptr || out_fb == nullptr || capture_ms == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_first_frame_discarded) {
        camera_fb_t *warmup = esp_camera_fb_get();
        if (warmup != nullptr) {
            esp_camera_fb_return(warmup);
        }
        g_first_frame_discarded = true;
    }

    const int64_t start_us = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    *capture_ms = static_cast<float>(esp_timer_get_time() - start_us) / 1000.0f;

    if (fb == nullptr) {
        ESP_LOGE(kTag, "esp_camera_fb_get returned null");
        return ESP_FAIL;
    }

    if (fb->format != PIXFORMAT_JPEG || fb->buf == nullptr || fb->len == 0) {
        ESP_LOGE(kTag, "Unexpected frame format=%d len=%u", static_cast<int>(fb->format), static_cast<unsigned>(fb->len));
        esp_camera_fb_return(fb);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *jpeg_data = fb->buf;
    *jpeg_len = fb->len;
    *out_fb = fb;
    return ESP_OK;
}

esp_err_t camera_capture_rgb888(uint8_t **rgb_data, uint16_t *width, uint16_t *height, float *capture_ms)
{
    if (!g_camera_ready || rgb_data == nullptr || width == nullptr || height == nullptr || capture_ms == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_first_frame_discarded) {
        camera_fb_t *warmup = esp_camera_fb_get();
        if (warmup != nullptr) {
            esp_camera_fb_return(warmup);
        }
        g_first_frame_discarded = true;
    }

    const int64_t start_us = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    *capture_ms = static_cast<float>(esp_timer_get_time() - start_us) / 1000.0f;

    if (fb == nullptr) {
        ESP_LOGE(kTag, "esp_camera_fb_get returned null");
        return ESP_FAIL;
    }

    if (fb->format != PIXFORMAT_RGB565 || fb->buf == nullptr || fb->len == 0) {
        ESP_LOGE(kTag,
                 "Unexpected frame format=%d for RGB capture, len=%u",
                 static_cast<int>(fb->format),
                 static_cast<unsigned>(fb->len));
        esp_camera_fb_return(fb);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t pixel_count = static_cast<size_t>(fb->width) * static_cast<size_t>(fb->height);
    uint8_t *dst = static_cast<uint8_t *>(heap_caps_malloc(pixel_count * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (dst == nullptr) {
        ESP_LOGE(kTag, "Failed to allocate RGB888 buffer");
        esp_camera_fb_return(fb);
        return ESP_ERR_NO_MEM;
    }

    if (!fmt2rgb888(fb->buf, fb->len, fb->format, dst)) {
        ESP_LOGE(kTag,
                 "fmt2rgb888 failed: format=%d len=%u width=%u height=%u",
                 static_cast<int>(fb->format),
                 static_cast<unsigned>(fb->len),
                 static_cast<unsigned>(fb->width),
                 static_cast<unsigned>(fb->height));
        heap_caps_free(dst);
        esp_camera_fb_return(fb);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *rgb_data = dst;
    *width = static_cast<uint16_t>(fb->width);
    *height = static_cast<uint16_t>(fb->height);
    esp_camera_fb_return(fb);
    return ESP_OK;
}

void camera_release(camera_fb_t *fb)
{
    if (fb != nullptr) {
        esp_camera_fb_return(fb);
    }
}

void camera_free_rgb888(uint8_t *rgb_data)
{
    if (rgb_data != nullptr) {
        heap_caps_free(rgb_data);
    }
}

void camera_deinit()
{
    if (!g_camera_ready) {
        return;
    }

    esp_camera_deinit();
    g_camera_ready = false;
    g_first_frame_discarded = false;
    ESP_LOGI(kTag, "Camera deinitialized");
}

} // namespace smart_bin
