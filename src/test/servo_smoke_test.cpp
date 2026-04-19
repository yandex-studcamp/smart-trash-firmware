#include "servo_smoke_test.hpp"

#include <array>
#include <cstdint>

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "servo_test";
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
constexpr ledc_timer_bit_t kDutyResolution = LEDC_TIMER_16_BIT;
constexpr uint32_t kPwmFrequencyHz = 50;
constexpr uint32_t kServoPeriodUs = 20000;
constexpr ledc_channel_t kServo1Channel = LEDC_CHANNEL_0;
constexpr ledc_channel_t kServo2Channel = LEDC_CHANNEL_1;

uint32_t angle_to_pulse_us(uint16_t angle_deg)
{
    const uint16_t clamped = angle_deg > 180 ? 180 : angle_deg;
    const uint32_t range = CONFIG_SMART_SERVO_MAX_PULSE_US - CONFIG_SMART_SERVO_MIN_PULSE_US;
    return CONFIG_SMART_SERVO_MIN_PULSE_US + (range * clamped) / 180;
}

uint32_t pulse_to_duty(uint32_t pulse_us)
{
    const uint32_t max_duty = (1u << kDutyResolution) - 1u;
    return (pulse_us * max_duty) / kServoPeriodUs;
}

esp_err_t set_servo_angle(ledc_channel_t channel, uint16_t angle_deg)
{
    const uint32_t pulse_us = angle_to_pulse_us(angle_deg);
    const uint32_t duty = pulse_to_duty(pulse_us);

    ESP_RETURN_ON_ERROR(ledc_set_duty(kSpeedMode, channel, duty), kTag, "ledc_set_duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(kSpeedMode, channel), kTag, "ledc_update_duty failed");
    return ESP_OK;
}

esp_err_t init_ledc()
{
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = kSpeedMode;
    timer_cfg.timer_num = kTimer;
    timer_cfg.duty_resolution = kDutyResolution;
    timer_cfg.freq_hz = kPwmFrequencyHz;
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), kTag, "ledc_timer_config failed");

    ledc_channel_config_t ch1_cfg = {};
    ch1_cfg.gpio_num = CONFIG_SMART_SERVO1_GPIO;
    ch1_cfg.speed_mode = kSpeedMode;
    ch1_cfg.channel = kServo1Channel;
    ch1_cfg.intr_type = LEDC_INTR_DISABLE;
    ch1_cfg.timer_sel = kTimer;
    ch1_cfg.duty = 0;
    ch1_cfg.hpoint = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch1_cfg), kTag, "servo1 ledc_channel_config failed");

    ledc_channel_config_t ch2_cfg = {};
    ch2_cfg.gpio_num = CONFIG_SMART_SERVO2_GPIO;
    ch2_cfg.speed_mode = kSpeedMode;
    ch2_cfg.channel = kServo2Channel;
    ch2_cfg.intr_type = LEDC_INTR_DISABLE;
    ch2_cfg.timer_sel = kTimer;
    ch2_cfg.duty = 0;
    ch2_cfg.hpoint = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch2_cfg), kTag, "servo2 ledc_channel_config failed");

    return ESP_OK;
}

} // namespace

namespace smart_bin {

void run_servo_smoke_test()
{
    if (init_ledc() != ESP_OK) {
        ESP_LOGE(kTag, "LEDC init failed. Check pin mapping and board config.");
        return;
    }

    ESP_LOGI(kTag,
             "Servo smoke test started: GPIO%d (servo1), GPIO%d (servo2)",
             CONFIG_SMART_SERVO1_GPIO,
             CONFIG_SMART_SERVO2_GPIO);
    ESP_LOGI(kTag, "Power servos from stable +5V, keep common GND with ESP32-CAM");

    const std::array<uint16_t, 4> pattern = {0, 90, 180, 90};
    while (true) {
        for (const uint16_t servo1_angle : pattern) {
            const uint16_t servo2_angle = static_cast<uint16_t>(180 - servo1_angle);

            if (set_servo_angle(kServo1Channel, servo1_angle) != ESP_OK ||
                set_servo_angle(kServo2Channel, servo2_angle) != ESP_OK) {
                ESP_LOGE(kTag, "Failed to update servo duty");
                return;
            }

            ESP_LOGI(kTag,
                     "servo1=%u deg (GPIO%d), servo2=%u deg (GPIO%d)",
                     servo1_angle,
                     CONFIG_SMART_SERVO1_GPIO,
                     servo2_angle,
                     CONFIG_SMART_SERVO2_GPIO);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_SMART_SERVO_DWELL_MS));
        }
    }
}

} // namespace smart_bin
