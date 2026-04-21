# smart-trash-firmware

Прошивка для ESP32-CAM (4MB flash) с двумя профилями запуска:

- `Servo smoke test` для bring-up механики (2x SG90).
- `Inference service` для захвата кадра, инференса и опционального действия сервами.

## Что реализовано на этом этапе

1. Добавлен общий переиспользуемый servo API (`src/board/servo_control.cpp/.hpp`):
- `servo_init_all()`
- `servo_set_angle()`
- `servo_set_home()`
- `servo_set_safe()`
- `servo_detach()`
- `servo_run_test_cycle()`
- `servo_log_profile_config()`

2. `servo_smoke_test` переведен на общий API:
- последовательность без одновременного движения двух серв
- подробные логи по шагам цикла
- лог памяти (heap) на цикл
- Kconfig-параметр количества циклов (`SMART_SERVO_TEST_CYCLES`, `0 = бесконечно`)

3. Интеграция с inference:
- после предсказания выполняется servo action через общий API
- маппинг классов:
  - `other(0)` -> серва #1 не двигается
  - `paper(1)` -> серва #1 идет в угол `150°`
  - `plastic(2)` -> серва #1 идет в угол `30°`
  - при любом классе серва #2 через `300 мс` идет в `120°`
- управление включается/выключается через `SMART_INFERENCE_SERVO_ACTIONS`

4. HTTP servo debug (опционально, по умолчанию выключен):
- `POST /servo/home`
- `POST /servo/test`
- `POST /servo/set?id=1&angle=90`
- включается флагом `SMART_HTTP_DEBUG_ENABLE_SERVO_ENDPOINTS` (только при `SMART_ENABLE_HTTP_DEBUG_API=y`)

## Структура проекта

```text
src/
├─ app/        # boot flow, sample dump
├─ inference/  # инференс и C API
├─ camera/     # захват RGB из ESP32-CAM
├─ network/    # SoftAP + HTTP debug адаптер
├─ test/       # servo smoke profile
├─ board/      # пины, board-константы, servo_control
├─ CMakeLists.txt
└─ Kconfig.projbuild
```

## Важные Kconfig-флаги

Общие:
- `SMART_BOOT_PROFILE_SERVO_TEST`
- `SMART_BOOT_PROFILE_INFERENCE_SERVICE`

Servo bring-up:
- `SMART_SERVO_TEST_CYCLES` (`0` = бесконечный цикл)
- `SMART_SERVO_SAFE_ANGLE_DEG`
- `SMART_SERVO1_HOME_ANGLE_DEG`
- `SMART_SERVO2_HOME_ANGLE_DEG`
- `SMART_SERVO1_TEST_ANGLE_DEG`
- `SMART_SERVO2_TEST_ANGLE_DEG`
- `SMART_SERVO_STEP_DELAY_MS`

Inference:
- `SMART_BOOT_CAPTURE_AND_INFER`
- `SMART_INFERENCE_SERVO_ACTIONS`
- `SMART_ENABLE_HTTP_DEBUG_API` включает путь JPEG decode для `/infer`
- `SMART_SERVO1_CLASS1_ANGLE_DEG`
- `SMART_SERVO1_CLASS2_ANGLE_DEG`
- `SMART_SERVO2_ANY_CLASS_ANGLE_DEG`
- `SMART_SERVO_SECONDARY_DELAY_MS`
- `SMART_SERVO_ACTION_HOLD_MS`
- `SMART_SERVO_ACTION_RETURN_DELAY_MS`

Сеть:
- `SMART_ENABLE_HTTP_DEBUG_API`
- `SMART_HTTP_DEBUG_ENABLE_SERVO_ENDPOINTS`
- `SMART_SOFTAP_SSID`
- `SMART_SOFTAP_PASSWORD`
- `SMART_HTTP_SERVER_PORT`

Отладка sample dump:
- `SMART_SAMPLE_DUMP_ENABLE`
- `SMART_SAMPLE_DUMP_OUTPUT_SIDE`

## Board-константы (servo)

Хранятся в `src/board/board_config.hpp` и `src/board/board_pins.hpp`:
- GPIO: servo1=`13`, servo2=`14`
- pulse range: `500..2400 us`
- углы: safe/home/test/action
- задержки: settle/step/action hold/return

Углы и задержки настраиваются через `idf.py menuconfig` в разделе `Smart Trash Firmware -> Servo settings`.
В `board/` остаются только дефолты и единая точка чтения этих настроек для smoke-test и inference-профиля.

## Inference API

```c
esp_err_t inference_init(void);
bool inference_is_ready(void);
esp_err_t inference_run_jpeg(const uint8_t *data, size_t len, inference_result_t *out);
esp_err_t inference_run_rgb888(const uint8_t *rgb, uint16_t width, uint16_t height, inference_result_t *out);
```

`inference_result_t` содержит:
- `predicted_class`
- `predicted_label`
- `confidence`
- `scores[3]`
- `input_width`, `input_height`
- `decode_ms`, `preprocess_ms`, `infer_ms`, `total_ms`

Порядок классов в прошивке:
- `other = 0`
- `paper = 1`
- `plastic = 2`

## Flash layout (4MB)

См. `partitions.csv`:

```text
nvs       0x009000  0x006000
phy_init  0x00f000  0x001000
factory   0x010000  0x270000
model     0x280000  0x180000
```

## Сборка и прошивка

В PowerShell сначала активировать ESP-IDF окружение:

```powershell
. "C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1"
```

Потом:

```powershell
idf.py fullclean
idf.py build
idf.py -p COM3 -b 115200 flash monitor
```

Если нужна только перепрошивка приложения:

```powershell
idf.py -p COM3 -b 115200 app-flash monitor
```

## Host-side сохранение sample dump

Если включен `SMART_SAMPLE_DUMP_ENABLE`, сохранять кадры из monitor log:

```powershell
python tools\save_serial_samples.py logs\monitor.log --out logs\samples
```
