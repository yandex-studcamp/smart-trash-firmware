#include "servo_smoke_test.hpp"

#include "board/board_config.hpp"
#include "board/board_pins.hpp"
#include "board/servo_control.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

namespace board = smart_bin::board;

constexpr char kTag[] = "servo_test";

} // namespace

namespace smart_bin {

void run_servo_smoke_test()
{
    ESP_LOGI(kTag, "Boot profile: servo smoke bring-up");
    servo_log_profile_config();
    ESP_LOGI(kTag,
             "Smoke sequence: safe -> servo1 test/home -> servo2 test/home (no simultaneous moves)");
    ESP_LOGI(kTag,
             "Power note: use stable +5V for SG90 and share GND with ESP32-CAM");

    if (servo_init_all() != ESP_OK) {
        ESP_LOGE(kTag, "Servo init failed.");
        return;
    }

    const uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t largest_heap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(kTag,
             "Heap before cycles: free=%u largest=%u",
             static_cast<unsigned>(free_heap),
             static_cast<unsigned>(largest_heap));

    const uint32_t max_cycles = CONFIG_SMART_SERVO_TEST_CYCLES;
    ESP_LOGI(kTag,
             "Configured cycles=%u (0 means infinite)",
             static_cast<unsigned>(max_cycles));

    uint32_t cycle = 0;
    while (max_cycles == 0 || cycle < max_cycles) {
        ++cycle;
        const esp_err_t cycle_ret = servo_run_test_cycle(cycle);
        if (cycle_ret != ESP_OK) {
            ESP_LOGE(kTag, "Cycle #%u failed: 0x%x", static_cast<unsigned>(cycle), cycle_ret);
            return;
        }
    }

    ESP_LOGI(kTag, "Servo smoke test finished after %u cycle(s)", static_cast<unsigned>(cycle));
    (void)servo_set_safe();
    vTaskDelay(pdMS_TO_TICKS(board::kServoActionReturnDelayMs));
    (void)servo_detach(servo_id_t::kServo1);
    (void)servo_detach(servo_id_t::kServo2);
}

} // namespace smart_bin
