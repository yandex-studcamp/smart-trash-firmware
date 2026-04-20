#pragma once

#include <cstdint>

#include "sdkconfig.h"

#ifndef CONFIG_SMART_SERVO_MIN_PULSE_US
#define CONFIG_SMART_SERVO_MIN_PULSE_US 500
#endif
#ifndef CONFIG_SMART_SERVO_MAX_PULSE_US
#define CONFIG_SMART_SERVO_MAX_PULSE_US 2400
#endif
#ifndef CONFIG_SMART_SERVO_DWELL_MS
#define CONFIG_SMART_SERVO_DWELL_MS 900
#endif
#ifndef CONFIG_SMART_CAMERA_XCLK_HZ
#define CONFIG_SMART_CAMERA_XCLK_HZ 20000000
#endif
#ifndef CONFIG_SMART_CAMERA_JPEG_QUALITY
#define CONFIG_SMART_CAMERA_JPEG_QUALITY 12
#endif
#ifndef CONFIG_SMART_CAMERA_FB_COUNT
#define CONFIG_SMART_CAMERA_FB_COUNT 1
#endif

namespace smart_bin::board {

constexpr uint16_t kServoHomeAngleDeg = 90;
constexpr uint16_t kServoMinAngleDeg = 0;
constexpr uint16_t kServoMaxAngleDeg = 180;
constexpr uint32_t kServoMinPulseUs = CONFIG_SMART_SERVO_MIN_PULSE_US;
constexpr uint32_t kServoMaxPulseUs = CONFIG_SMART_SERVO_MAX_PULSE_US;
constexpr uint32_t kServoDwellMs = CONFIG_SMART_SERVO_DWELL_MS;

constexpr int kCameraXclkHz = CONFIG_SMART_CAMERA_XCLK_HZ;
constexpr int kCameraJpegQuality = CONFIG_SMART_CAMERA_JPEG_QUALITY;
constexpr int kCameraFbCount = CONFIG_SMART_CAMERA_FB_COUNT;

} // namespace smart_bin::board
