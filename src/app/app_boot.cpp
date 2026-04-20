#include "app_boot.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if CONFIG_SMART_BOOT_PROFILE_INFERENCE_SERVICE
#include "camera/camera.hpp"
#include "inference/inference.h"
#if CONFIG_SMART_SAMPLE_DUMP_ENABLE
#include "app/sample_dump.hpp"
#endif
#if CONFIG_SMART_ENABLE_NETWORK_DEBUG_API
#include "esp_netif.h"
#include "network/http_server.hpp"
#include "network/softap.hpp"
#endif
#endif

#if CONFIG_SMART_BOOT_PROFILE_SERVO_TEST
#include "test/servo_smoke_test.hpp"
#endif

namespace {

constexpr char kTag[] = "boot";

#if CONFIG_SMART_BOOT_PROFILE_INFERENCE_SERVICE
void log_heap(const char *stage)
{
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_LOGI(kTag,
             "[heap:%s] internal_free=%u internal_largest=%u 8bit_free=%u psram_free=%u psram_largest=%u",
             stage,
             static_cast<unsigned>(free_internal),
             static_cast<unsigned>(largest_internal),
             static_cast<unsigned>(free_8bit),
             static_cast<unsigned>(free_psram),
             static_cast<unsigned>(largest_psram));
}

void run_boot_camera_inference()
{
    log_heap("startup");

    const esp_err_t infer_init_ret = inference_init();
    if (infer_init_ret != ESP_OK) {
        ESP_LOGE(kTag, "Inference init failed: 0x%x", infer_init_ret);
        return;
    }
    log_heap("after_inference_init");

#if CONFIG_SMART_BOOT_CAPTURE_AND_INFER
    const esp_err_t cam_init_ret = smart_bin::camera_init();
    if (cam_init_ret != ESP_OK) {
        ESP_LOGE(kTag, "Camera init failed: 0x%x", cam_init_ret);
        return;
    }

    uint8_t *rgb_data = nullptr;
    uint16_t rgb_width = 0;
    uint16_t rgb_height = 0;
    float capture_ms = 0.0f;
    const esp_err_t capture_ret =
        smart_bin::camera_capture_rgb888(&rgb_data, &rgb_width, &rgb_height, &capture_ms);
    if (capture_ret != ESP_OK) {
        ESP_LOGE(kTag, "Camera capture failed: 0x%x", capture_ret);
        smart_bin::camera_deinit();
        return;
    }

    ESP_LOGI(kTag,
             "Captured RGB frame: %ux%u in %.2f ms",
             static_cast<unsigned>(rgb_width),
             static_cast<unsigned>(rgb_height),
             capture_ms);
    log_heap("after_capture");

#if CONFIG_SMART_SAMPLE_DUMP_ENABLE
    const esp_err_t sample_dump_ret = smart_bin::dump_rgb888_sample_to_log(rgb_data, rgb_width, rgb_height);
    if (sample_dump_ret != ESP_OK) {
        ESP_LOGE(kTag, "Sample dump failed: 0x%x", sample_dump_ret);
    }
#endif

    inference_result_t result = {};
    const int64_t infer_call_start_us = esp_timer_get_time();
    bool readd_task_wdt = false;
#if CONFIG_ESP_TASK_WDT_EN
    const esp_err_t wdt_delete_ret = esp_task_wdt_delete(nullptr);
    if (wdt_delete_ret == ESP_OK) {
        readd_task_wdt = true;
        ESP_LOGW(kTag, "Temporarily removed current task from task_wdt for boot inference");
    } else if (wdt_delete_ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "esp_task_wdt_delete failed: 0x%x", wdt_delete_ret);
    }
#endif

    const esp_err_t infer_ret = inference_run_rgb888(rgb_data, rgb_width, rgb_height, &result);

#if CONFIG_ESP_TASK_WDT_EN
    if (readd_task_wdt) {
        const esp_err_t wdt_add_ret = esp_task_wdt_add(nullptr);
        if (wdt_add_ret != ESP_OK) {
            ESP_LOGE(kTag, "Failed to re-add current task to task_wdt: 0x%x", wdt_add_ret);
        }
    }
#endif
    const float infer_call_ms = static_cast<float>(esp_timer_get_time() - infer_call_start_us) / 1000.0f;

    smart_bin::camera_free_rgb888(rgb_data);
    smart_bin::camera_deinit();

    if (infer_ret != ESP_OK) {
        ESP_LOGE(kTag, "Boot inference failed: 0x%x", infer_ret);
        return;
    }

    ESP_LOGI(kTag,
             "Prediction: class=%d label=%s confidence=%.3f",
             result.predicted_class,
             result.predicted_label,
             result.confidence);
    ESP_LOGI(kTag, "Scores: other=%.4f paper=%.4f plastic=%.4f", result.scores[0], result.scores[1], result.scores[2]);
    ESP_LOGI(kTag,
             "Timing ms: capture=%.2f decode=%.2f preprocess=%.2f infer=%.2f total=%.2f api_call=%.2f",
             capture_ms,
             result.decode_ms,
             result.preprocess_ms,
             result.infer_ms,
             result.total_ms,
             infer_call_ms);
    log_heap("after_boot_inference");
#else
    ESP_LOGI(kTag, "Boot camera inference is disabled by Kconfig");
#endif
}

void run_inference_boot_flow()
{
    run_boot_camera_inference();

#if CONFIG_SMART_ENABLE_NETWORK_DEBUG_API
    esp_netif_t *ap_netif = nullptr;
    const esp_err_t ap_ret = smart_bin::start_softap(&ap_netif);
    if (ap_ret != ESP_OK) {
        ESP_LOGE(kTag, "SoftAP start failed: 0x%x", ap_ret);
        return;
    }

    const esp_err_t http_ret = smart_bin::start_http_server();
    if (http_ret != ESP_OK) {
        ESP_LOGE(kTag, "HTTP server start failed: 0x%x", http_ret);
        return;
    }

    esp_netif_ip_info_t ip_info = {};
    if (ap_netif != nullptr && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
        ESP_LOGI(kTag, "Smoke test command:");
        ESP_LOGI(kTag,
                 "curl -X POST -H \"Content-Type: image/jpeg\" --data-binary @image.jpg http://" IPSTR ":%d/infer",
                 IP2STR(&ip_info.ip),
                 CONFIG_SMART_HTTP_SERVER_PORT);
    }
#else
    ESP_LOGI(kTag, "Network debug API is disabled. SoftAP/HTTP not started.");
#endif
}
#endif

} // namespace

namespace smart_bin {

void run_boot_flow()
{
    ESP_LOGI(kTag, "Application start");

#if CONFIG_SMART_BOOT_PROFILE_SERVO_TEST
    ESP_LOGI(kTag, "Boot profile: servo smoke test");
    run_servo_smoke_test();
#elif CONFIG_SMART_BOOT_PROFILE_INFERENCE_SERVICE
    ESP_LOGI(kTag, "Boot profile: inference service");
    run_inference_boot_flow();
#else
    ESP_LOGE(kTag, "No boot profile selected");
#endif
}

} // namespace smart_bin
