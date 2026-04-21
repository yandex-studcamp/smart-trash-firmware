#pragma once

#include <cstdint>

#include "sdkconfig.h"

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

constexpr uint16_t kServoSafeAngleDeg = 90;
constexpr uint16_t kServo1HomeAngleDeg = 90;
constexpr uint16_t kServo2HomeAngleDeg = 90;
constexpr uint16_t kServo1TestAngleDeg = 35;
constexpr uint16_t kServo2TestAngleDeg = 145;
constexpr uint16_t kServo1Class1AngleDeg = 150;
constexpr uint16_t kServo1Class2AngleDeg = 30;
constexpr uint16_t kServo2AnyClassOffsetDeg = 30;
constexpr uint16_t kServo2AnyClassAngleDeg = kServo2HomeAngleDeg + kServo2AnyClassOffsetDeg;
constexpr uint16_t kServoMinAngleDeg = 0;
constexpr uint16_t kServoMaxAngleDeg = 180;
constexpr uint32_t kServoMinPulseUs = 500;
constexpr uint32_t kServoMaxPulseUs = 2400;
constexpr uint32_t kServoStartupSettleMs = 450;
constexpr uint32_t kServoStepDelayMs = 900;
constexpr uint32_t kServoSecondaryDelayMs = 300;
constexpr uint32_t kServoActionHoldMs = 700;
constexpr uint32_t kServoActionReturnDelayMs = 350;
constexpr uint32_t kServoPwmFrequencyHz = 50;
constexpr uint32_t kServoPwmPeriodUs = 20000;

constexpr int kCameraXclkHz = CONFIG_SMART_CAMERA_XCLK_HZ;
constexpr int kCameraJpegQuality = CONFIG_SMART_CAMERA_JPEG_QUALITY;
constexpr int kCameraFbCount = CONFIG_SMART_CAMERA_FB_COUNT;

} // namespace smart_bin::board
