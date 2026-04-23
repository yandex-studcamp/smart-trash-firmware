#include "board/servo_control.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdint>

#include "board/board_config.hpp"
#include "board/board_pins.hpp"
#include "board/board_profile.hpp"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

namespace board = smart_bin::board;

constexpr char kTag[] = "servo";
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_1;
constexpr ledc_timer_bit_t kDutyResolution = LEDC_TIMER_16_BIT;
constexpr ledc_channel_t kServo1Channel = LEDC_CHANNEL_2;
constexpr ledc_channel_t kServo2Channel = LEDC_CHANNEL_3;

bool g_ready = false;
uint16_t g_servo1_angle = board::kServoSafeAngleDeg;
uint16_t g_servo2_angle = board::kServoSafeAngleDeg;
bool g_servo1_angle_known = false;
bool g_servo2_angle_known = false;

uint16_t clamp_angle(uint16_t angle_deg)
{
    return std::min<uint16_t>(board::kServoMaxAngleDeg, angle_deg);
}

ledc_channel_t channel_for_servo(smart_bin::servo_id_t id)
{
    return (id == smart_bin::servo_id_t::kServo1) ? kServo1Channel : kServo2Channel;
}

int gpio_for_servo(smart_bin::servo_id_t id)
{
    return (id == smart_bin::servo_id_t::kServo1) ? board::kServo1Gpio : board::kServo2Gpio;
}

uint16_t home_angle_for_servo(smart_bin::servo_id_t id)
{
    return (id == smart_bin::servo_id_t::kServo1) ? board::kServo1HomeAngleDeg : board::kServo2HomeAngleDeg;
}

uint16_t &stored_angle_for_servo(smart_bin::servo_id_t id)
{
    return (id == smart_bin::servo_id_t::kServo1) ? g_servo1_angle : g_servo2_angle;
}

bool &angle_known_for_servo(smart_bin::servo_id_t id)
{
    return (id == smart_bin::servo_id_t::kServo1) ? g_servo1_angle_known : g_servo2_angle_known;
}

uint32_t angle_to_pulse_us(uint16_t angle_deg)
{
    const uint16_t clamped = clamp_angle(angle_deg);
    const uint32_t range = board::kServoMaxPulseUs - board::kServoMinPulseUs;
    return board::kServoMinPulseUs + (range * clamped) / board::kServoMaxAngleDeg;
}

uint32_t pulse_to_duty(uint32_t pulse_us)
{
    const uint32_t max_duty = (1u << kDutyResolution) - 1u;
    return (pulse_us * max_duty) / board::kServoPwmPeriodUs;
}

esp_err_t set_servo_angle_raw(smart_bin::servo_id_t id, uint16_t angle_deg)
{
    const uint32_t pulse_us = angle_to_pulse_us(angle_deg);
    const uint32_t duty = pulse_to_duty(pulse_us);
    const ledc_channel_t channel = channel_for_servo(id);

    ESP_RETURN_ON_ERROR(ledc_set_duty(kSpeedMode, channel, duty), kTag, "ledc_set_duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(kSpeedMode, channel), kTag, "ledc_update_duty failed");
    return ESP_OK;
}

void log_servo_micro_step(smart_bin::servo_id_t id, uint16_t current_angle, uint16_t target_angle)
{
    if (!board::kBootProfileServoTest) {
        return;
    }

    const uint32_t pulse_us = angle_to_pulse_us(current_angle);
    const uint32_t duty = pulse_to_duty(pulse_us);
    ESP_LOGI(kTag,
             "servo=%u step gpio=%d channel=%d angle=%u target=%u pulse_us=%" PRIu32 " duty=%" PRIu32,
             static_cast<unsigned>(id == smart_bin::servo_id_t::kServo1 ? 1 : 2),
             gpio_for_servo(id),
             static_cast<int>(channel_for_servo(id)),
             static_cast<unsigned>(current_angle),
             static_cast<unsigned>(target_angle),
             pulse_us,
             duty);
}

esp_err_t move_servo_smooth(smart_bin::servo_id_t id, uint16_t target_angle)
{
    uint16_t &current_angle = stored_angle_for_servo(id);
    bool &angle_known = angle_known_for_servo(id);

    if (!angle_known) {
        ESP_RETURN_ON_ERROR(set_servo_angle_raw(id, target_angle), kTag, "Failed to set initial servo angle");
        current_angle = target_angle;
        angle_known = true;
        log_servo_micro_step(id, current_angle, target_angle);
        return ESP_OK;
    }

    const uint16_t step_deg = std::max<uint16_t>(1, board::kServoSlewStepDeg);
    if (current_angle == target_angle || step_deg == 0) {
        ESP_RETURN_ON_ERROR(set_servo_angle_raw(id, target_angle), kTag, "Failed to set servo angle");
        current_angle = target_angle;
        log_servo_micro_step(id, current_angle, target_angle);
        return ESP_OK;
    }

    while (current_angle != target_angle) {
        if (current_angle < target_angle) {
            const uint16_t remaining = static_cast<uint16_t>(target_angle - current_angle);
            current_angle = static_cast<uint16_t>(current_angle + std::min<uint16_t>(step_deg, remaining));
        } else {
            const uint16_t remaining = static_cast<uint16_t>(current_angle - target_angle);
            current_angle = static_cast<uint16_t>(current_angle - std::min<uint16_t>(step_deg, remaining));
        }

        ESP_RETURN_ON_ERROR(set_servo_angle_raw(id, current_angle), kTag, "Failed to update servo micro-step");
        log_servo_micro_step(id, current_angle, target_angle);
        if (board::kServoSlewStepDelayMs > 0) {
            vTaskDelay(pdMS_TO_TICKS(board::kServoSlewStepDelayMs));
        }
    }

    return ESP_OK;
}

esp_err_t configure_ledc()
{
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = kSpeedMode;
    timer_cfg.timer_num = kTimer;
    timer_cfg.duty_resolution = kDutyResolution;
    timer_cfg.freq_hz = board::kServoPwmFrequencyHz;
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), kTag, "ledc_timer_config failed");

    ledc_channel_config_t servo1_cfg = {};
    servo1_cfg.gpio_num = board::kServo1Gpio;
    servo1_cfg.speed_mode = kSpeedMode;
    servo1_cfg.channel = kServo1Channel;
    servo1_cfg.timer_sel = kTimer;
    servo1_cfg.duty = 0;
    servo1_cfg.hpoint = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&servo1_cfg), kTag, "servo1 ledc_channel_config failed");

    ledc_channel_config_t servo2_cfg = {};
    servo2_cfg.gpio_num = board::kServo2Gpio;
    servo2_cfg.speed_mode = kSpeedMode;
    servo2_cfg.channel = kServo2Channel;
    servo2_cfg.timer_sel = kTimer;
    servo2_cfg.duty = 0;
    servo2_cfg.hpoint = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&servo2_cfg), kTag, "servo2 ledc_channel_config failed");

    return ESP_OK;
}

void step_delay()
{
    vTaskDelay(pdMS_TO_TICKS(board::kServoStepDelayMs));
}

void log_heap(const char *stage)
{
    const size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(kTag,
             "[heap:%s] free_8bit=%u free_internal=%u",
             stage,
             static_cast<unsigned>(free_8bit),
             static_cast<unsigned>(free_internal));
}

} // namespace

namespace smart_bin {

esp_err_t servo_init_all()
{
    if (g_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(configure_ledc(), kTag, "LEDC init failed");
    g_ready = true;

    ESP_RETURN_ON_ERROR(servo_set_safe(), kTag, "Failed to set safe angles");
    vTaskDelay(pdMS_TO_TICKS(board::kServoStartupSettleMs));
    ESP_LOGI(kTag, "Servo API is ready");
    return ESP_OK;
}

bool servo_is_ready()
{
    return g_ready;
}

esp_err_t servo_set_angle(servo_id_t id, uint16_t degrees)
{
    if (!g_ready) {
        ESP_LOGE(kTag, "servo_set_angle called before servo_init_all");
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t angle = clamp_angle(degrees);
    ESP_RETURN_ON_ERROR(move_servo_smooth(id, angle), kTag, "Failed to set servo angle");
    ESP_LOGI(kTag,
             "servo=%u gpio=%d channel=%d angle=%u slew_step=%u step_delay_ms=%" PRIu32,
             static_cast<unsigned>(id == servo_id_t::kServo1 ? 1 : 2),
             gpio_for_servo(id),
             static_cast<int>(channel_for_servo(id)),
             static_cast<unsigned>(angle),
             static_cast<unsigned>(board::kServoSlewStepDeg),
             board::kServoSlewStepDelayMs);
    return ESP_OK;
}

esp_err_t servo_set_home(servo_id_t id)
{
    return servo_set_angle(id, home_angle_for_servo(id));
}

esp_err_t servo_set_safe()
{
    ESP_RETURN_ON_ERROR(servo_set_angle(servo_id_t::kServo1, board::kServoSafeAngleDeg), kTag, "servo1 safe failed");
    vTaskDelay(pdMS_TO_TICKS(board::kServoActionReturnDelayMs));
    ESP_RETURN_ON_ERROR(servo_set_angle(servo_id_t::kServo2, board::kServoSafeAngleDeg), kTag, "servo2 safe failed");
    vTaskDelay(pdMS_TO_TICKS(board::kServoActionReturnDelayMs));
    return ESP_OK;
}

esp_err_t servo_detach(servo_id_t id)
{
    if (!g_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return ledc_stop(kSpeedMode, channel_for_servo(id), 0);
}

esp_err_t servo_detach_all()
{
    if (!g_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(servo_detach(servo_id_t::kServo1), kTag, "servo1 detach failed");
    ESP_RETURN_ON_ERROR(servo_detach(servo_id_t::kServo2), kTag, "servo2 detach failed");
    ESP_LOGI(kTag, "All servos detached");
    return ESP_OK;
}

void servo_log_profile_config()
{
    ESP_LOGI(kTag, "Servo profile config:");
    ESP_LOGI(kTag,
             "pins: servo1=GPIO%d servo2=GPIO%d channels=[%d,%d] timer=%d",
             board::kServo1Gpio,
             board::kServo2Gpio,
             static_cast<int>(kServo1Channel),
             static_cast<int>(kServo2Channel),
             static_cast<int>(kTimer));
    ESP_LOGI(kTag,
             "angles: safe=%u home=[%u,%u] test=[%u,%u] limits=[%u..%u]",
             static_cast<unsigned>(board::kServoSafeAngleDeg),
             static_cast<unsigned>(board::kServo1HomeAngleDeg),
             static_cast<unsigned>(board::kServo2HomeAngleDeg),
             static_cast<unsigned>(board::kServo1TestAngleDeg),
             static_cast<unsigned>(board::kServo2TestAngleDeg),
             static_cast<unsigned>(board::kServoMinAngleDeg),
             static_cast<unsigned>(board::kServoMaxAngleDeg));
    ESP_LOGI(kTag,
             "timing_ms: settle=%" PRIu32 " step=%" PRIu32 " action_hold=%" PRIu32 " action_return=%" PRIu32,
             board::kServoStartupSettleMs,
             board::kServoStepDelayMs,
             board::kServoActionHoldMs,
             board::kServoActionReturnDelayMs);
    ESP_LOGI(kTag,
             "slew: step_deg=%u step_delay_ms=%" PRIu32,
             static_cast<unsigned>(board::kServoSlewStepDeg),
             board::kServoSlewStepDelayMs);
    ESP_LOGI(kTag,
             "detach_at_idle: test=%s inference=%s",
             board::kServoTestDetachAtIdle ? "on" : "off",
             board::kServoInferenceDetachAtIdle ? "on" : "off");
}

esp_err_t servo_run_test_cycle(uint32_t cycle_index)
{
    if (!g_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(kTag, "Cycle #%u start", static_cast<unsigned>(cycle_index));
    log_heap("cycle_start");

    ESP_RETURN_ON_ERROR(servo_set_safe(), kTag, "set_safe failed");
    step_delay();

    ESP_LOGI(kTag, "Cycle #%u step: servo1 -> test angle", static_cast<unsigned>(cycle_index));
    ESP_RETURN_ON_ERROR(servo_set_angle(servo_id_t::kServo1, board::kServo1TestAngleDeg), kTag, "servo1 test failed");
    step_delay();

    ESP_LOGI(kTag, "Cycle #%u step: servo1 -> home", static_cast<unsigned>(cycle_index));
    ESP_RETURN_ON_ERROR(servo_set_home(servo_id_t::kServo1), kTag, "servo1 home failed");
    step_delay();

    ESP_LOGI(kTag, "Cycle #%u step: servo2 -> test angle", static_cast<unsigned>(cycle_index));
    ESP_RETURN_ON_ERROR(servo_set_angle(servo_id_t::kServo2, board::kServo2TestAngleDeg), kTag, "servo2 test failed");
    step_delay();

    ESP_LOGI(kTag, "Cycle #%u step: servo2 -> home", static_cast<unsigned>(cycle_index));
    ESP_RETURN_ON_ERROR(servo_set_home(servo_id_t::kServo2), kTag, "servo2 home failed");
    step_delay();

    log_heap("cycle_done");
    ESP_LOGI(kTag, "Cycle #%u complete", static_cast<unsigned>(cycle_index));
    return ESP_OK;
}

} // namespace smart_bin
