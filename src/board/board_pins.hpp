#pragma once

#include "sdkconfig.h"

#ifndef CONFIG_SMART_CAMERA_PIN_PWDN
#define CONFIG_SMART_CAMERA_PIN_PWDN 32
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_RESET
#define CONFIG_SMART_CAMERA_PIN_RESET -1
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_XCLK
#define CONFIG_SMART_CAMERA_PIN_XCLK 0
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_SIOD
#define CONFIG_SMART_CAMERA_PIN_SIOD 26
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_SIOC
#define CONFIG_SMART_CAMERA_PIN_SIOC 27
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_D7
#define CONFIG_SMART_CAMERA_PIN_D7 35
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_D6
#define CONFIG_SMART_CAMERA_PIN_D6 34
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_D5
#define CONFIG_SMART_CAMERA_PIN_D5 39
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_D4
#define CONFIG_SMART_CAMERA_PIN_D4 36
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_D3
#define CONFIG_SMART_CAMERA_PIN_D3 21
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_D2
#define CONFIG_SMART_CAMERA_PIN_D2 19
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_D1
#define CONFIG_SMART_CAMERA_PIN_D1 18
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_D0
#define CONFIG_SMART_CAMERA_PIN_D0 5
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_VSYNC
#define CONFIG_SMART_CAMERA_PIN_VSYNC 25
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_HREF
#define CONFIG_SMART_CAMERA_PIN_HREF 23
#endif
#ifndef CONFIG_SMART_CAMERA_PIN_PCLK
#define CONFIG_SMART_CAMERA_PIN_PCLK 22
#endif

namespace smart_bin::board {

constexpr int kServo1Gpio = 13;
constexpr int kServo2Gpio = 14;

constexpr int kCameraPinPwdn = CONFIG_SMART_CAMERA_PIN_PWDN;
constexpr int kCameraPinReset = CONFIG_SMART_CAMERA_PIN_RESET;
constexpr int kCameraPinXclk = CONFIG_SMART_CAMERA_PIN_XCLK;
constexpr int kCameraPinSiod = CONFIG_SMART_CAMERA_PIN_SIOD;
constexpr int kCameraPinSioc = CONFIG_SMART_CAMERA_PIN_SIOC;
constexpr int kCameraPinD7 = CONFIG_SMART_CAMERA_PIN_D7;
constexpr int kCameraPinD6 = CONFIG_SMART_CAMERA_PIN_D6;
constexpr int kCameraPinD5 = CONFIG_SMART_CAMERA_PIN_D5;
constexpr int kCameraPinD4 = CONFIG_SMART_CAMERA_PIN_D4;
constexpr int kCameraPinD3 = CONFIG_SMART_CAMERA_PIN_D3;
constexpr int kCameraPinD2 = CONFIG_SMART_CAMERA_PIN_D2;
constexpr int kCameraPinD1 = CONFIG_SMART_CAMERA_PIN_D1;
constexpr int kCameraPinD0 = CONFIG_SMART_CAMERA_PIN_D0;
constexpr int kCameraPinVsync = CONFIG_SMART_CAMERA_PIN_VSYNC;
constexpr int kCameraPinHref = CONFIG_SMART_CAMERA_PIN_HREF;
constexpr int kCameraPinPclk = CONFIG_SMART_CAMERA_PIN_PCLK;

} // namespace smart_bin::board
