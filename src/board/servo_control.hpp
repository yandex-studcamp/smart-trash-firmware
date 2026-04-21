#pragma once

#include <cstdint>

#include "esp_err.h"

namespace smart_bin {

enum class servo_id_t : uint8_t {
    kServo1 = 1,
    kServo2 = 2,
};

esp_err_t servo_init_all();
bool servo_is_ready();
esp_err_t servo_set_angle(servo_id_t id, uint16_t degrees);
esp_err_t servo_set_home(servo_id_t id);
esp_err_t servo_set_safe();
esp_err_t servo_detach(servo_id_t id);
esp_err_t servo_run_test_cycle(uint32_t cycle_index);
void servo_log_profile_config();

} // namespace smart_bin

