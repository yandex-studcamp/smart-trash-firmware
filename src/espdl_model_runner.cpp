#include "espdl_model_runner.hpp"

#include <new>

#include "dl_model_base.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr const char *TAG = "espdl_boot";

void log_heap(const char *stage) {
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);

    ESP_LOGI(TAG,
             "[heap:%s] free_internal=%u, largest_internal=%u, free_8bit=%u",
             stage,
             static_cast<unsigned>(free_internal),
             static_cast<unsigned>(largest_internal),
             static_cast<unsigned>(free_8bit));

#if CONFIG_SPIRAM
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "[heap:%s] free_psram=%u", stage, static_cast<unsigned>(free_psram));
#endif
}

void log_psram_status() {
#if CONFIG_SPIRAM
    ESP_LOGI(TAG, "PSRAM support is enabled in firmware");
#else
    ESP_LOGW(TAG, "PSRAM support is disabled in firmware");
#endif

    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG,
             "[psram] free=%u, largest_block=%u",
             static_cast<unsigned>(free_psram),
             static_cast<unsigned>(largest_psram));
}

dl::Model *load_model_from_partition(int64_t &load_time_us) {
    ESP_LOGI(TAG, "Loading ESP-DL model from partition 'model'...");
    const int64_t load_start_us = esp_timer_get_time();
    auto *model = new (std::nothrow)
        dl::Model("model", fbs::MODEL_LOCATION_IN_FLASH_PARTITION, 0, dl::MEMORY_MANAGER_GREEDY, nullptr, false);
    load_time_us = esp_timer_get_time() - load_start_us;
    return model;
}

void run_model_test(dl::Model &model) {
    ESP_LOGI(TAG, "Running model->test()...");
    const int64_t test_start_us = esp_timer_get_time();
    const esp_err_t test_ret = model.test();
    const int64_t test_time_us = esp_timer_get_time() - test_start_us;

    if (test_ret == ESP_OK) {
        ESP_LOGI(TAG, "model->test() PASSED in %.2f ms", test_time_us / 1000.0f);
    } else {
        ESP_LOGE(TAG, "model->test() FAILED in %.2f ms, err=0x%x", test_time_us / 1000.0f, test_ret);
    }
}

void run_model_profiling(dl::Model &model) {
    ESP_LOGI(TAG, "Profiling memory usage...");
    model.profile_memory();

    ESP_LOGI(TAG, "Profiling inference latency (sorted)...");
    model.profile_module(true);
}

}  // namespace

namespace espdl_boot {

void run_model_boot_flow() {
    ESP_LOGI(TAG, "Application start");
    log_psram_status();
    log_heap("startup");

    int64_t load_time_us = 0;
    dl::Model *model = load_model_from_partition(load_time_us);
    if (model == nullptr) {
        ESP_LOGE(TAG, "Model allocation/load failed");
        return;
    }

    ESP_LOGI(TAG, "Model loaded successfully in %.2f ms", load_time_us / 1000.0f);
    log_heap("after_load");

    run_model_test(*model);
    run_model_profiling(*model);
    log_heap("after_profile");

    delete model;
    ESP_LOGI(TAG, "Done");
}

}  // namespace espdl_boot
