# smart-trash-firmware

Прошивка для ESP32-CAM с двумя сервоприводами SG90 и ESP-DL моделью классификации мусора.

Текущая цель проекта: быстро проверять железо, запускать инференс по кадру с камеры и по результату двигать сервоприводы. Прошивка рассчитана на ESP32-CAM с 4 MB flash, поэтому лишние debug-функции по умолчанию выключены.

## Возможности

- Проверка двух SG90 через отдельный `Servo smoke test` профиль.
- Захват кадра с ESP32-CAM.
- Инференс ESP-DL модели из отдельного flash-раздела `model`.
- Логирование результата классификации, scores и времени этапов.
- Поддержка `Prediction policy`, включая `nothing` по confidence threshold и через premodel (MSE threshold).
- Управление сервами после инференса.
- Режим запуска инференса по кнопке или периодически раз в N секунд.
- Отдельный camera-capture профиль для просмотра последнего JPEG кадра по Wi-Fi.
- Опциональные helper-скрипты для активации ESP-IDF окружения и деплоя.

## Железо

Основная целевая схема соответствует картинке `ESP32-CAM + 2x SG90`:

| Узел | Подключение |
| --- | --- |
| ESP32-CAM | модуль с PSRAM и 4 MB flash |
| Servo SG90 #1 orange / signal | GPIO13 |
| Servo SG90 #2 orange / signal | GPIO14 |
| Servo SG90 red | +5V |
| Servo SG90 brown | GND |
| USB-UART TX | U0R / RX0 на ESP32-CAM |
| USB-UART RX | U0T / TX0 на ESP32-CAM |
| USB-UART GND | общий GND |
| Flash jumper | GPIO0 -> GND только на время прошивки |
| Electrolytic capacitor | 100-470 uF между +5V и GND, ближе к ESP32-CAM/сервам |

Важно: у ESP32-CAM и сервоприводов должна быть общая земля. Сервам лучше дать отдельное стабильное питание 5V, потому что питание от USB-UART часто просаживается при движении SG90.

Для прошивки замкните GPIO0 на GND, запустите `idf.py flash`, после успешной прошивки уберите перемычку GPIO0-GND и нажмите RST. Не оставляйте GPIO0 замкнутым при обычной загрузке, иначе ESP32-CAM уйдёт в download/bootloader mode.

В button-trigger режиме прошивка по умолчанию тоже использует GPIO0 как кнопку инференса. Это удобно для быстрых тестов, но критично помнить: кнопка должна быть отпущена во время reset/boot. Если нужно стабильное устройство без риска случайного входа в режим прошивки, лучше переназначить `SMART_INFERENCE_BUTTON_GPIO` на другой свободный GPIO под вашу финальную плату.

## Структура проекта

```text
smart-trash-firmware/
├─ CMakeLists.txt
├─ partitions.csv
├─ sdkconfig.defaults
├─ models/
│  ├─ model.espdl
│  └─ premodel.espdl
├─ main/
│  └─ main.cpp
├─ src/
│  ├─ app/        # boot-flow и сценарии запуска
│  ├─ board/      # пины, board-константы, servo API
│  ├─ camera/     # инициализация камеры, JPEG/RGB захват
│  ├─ inference/  # ESP-DL модель, preprocessing, prediction API
│  ├─ network/    # SoftAP и HTTP-серверы
│  └─ test/       # servo smoke test
├─ scripts/       # helper-скрипты для ESP-IDF окружения
└─ tools/         # host-side debug tools
```

Entrypoint находится в `main/main.cpp` и вызывает `smart_bin::run_boot_flow()`. Конкретный сценарий выбирается через Kconfig.

## Режимы прошивки

Режим выбирается в:

```text
idf.py menuconfig
Smart Trash Firmware -> Boot Profile
```

### Servo smoke test

Профиль `SMART_BOOT_PROFILE_SERVO_TEST`.

Используется для проверки схемы без камеры и модели. Прошивка инициализирует PWM, ставит обе сервы в safe/home положение и циклически двигает их по очереди. Одновременного движения двух серв нет, чтобы снизить риск просадки питания.

Количество циклов задаётся через `SMART_SERVO_TEST_CYCLES`. Значение `0` означает бесконечный тест.

### Inference service

Профиль `SMART_BOOT_PROFILE_INFERENCE_SERVICE`.

Основной рабочий профиль:

1. Инициализирует модель из flash-раздела `model`.
2. Инициализирует камеру.
3. Делает снимок.
4. Запускает preprocessing и инференс.
5. Печатает prediction, scores и timing в лог.
6. При включённом `SMART_INFERENCE_SERVO_ACTIONS` двигает сервы по результату.

Инференс можно запускать двумя способами:

- `SMART_INFERENCE_TRIGGER_MODE_PERIODIC`: каждые `SMART_INFERENCE_PERIOD_SEC` секунд, по умолчанию 5.
- `SMART_INFERENCE_TRIGGER_MODE_BUTTON`: по нажатию кнопки, по умолчанию GPIO0 active-low.

HTTP debug API можно включить через `SMART_ENABLE_HTTP_DEBUG_API`, но для обычной прошивки его лучше держать выключенным: он увеличивает размер бинарника и включает JPEG-путь инференса.

### Camera capture service

Профиль `SMART_BOOT_PROFILE_CAMERA_CAPTURE_SERVICE`.

Отладочный профиль для камеры. Он поднимает SoftAP, периодически делает JPEG-снимок и публикует последний кадр через HTTP. Интервал задаётся `SMART_CAMERA_CAPTURE_INTERVAL_SEC`.

## Инференс

Основная модель лежит в:

```text
models/<SMART_MODEL_PRIMARY_FILENAME>
```

При policy `SMART_PREDICTION_POLICY_PRESENCE_PREMODEL` дополнительно используется:

```text
models/<SMART_MODEL_GATE_FILENAME>
```

При `SMART_FLASH_MODEL_WITH_APP=y` CMake автоматически добавляет модели в flash-команду:
- основную в partition `model`;
- gate/premodel в partition `premodel` (только для `PRESENCE_PREMODEL` policy).

Базовый порядок классов в классификаторе:

| Class id | Label |
| --- | --- |
| 0 | `other` |
| 1 | `paper` |
| 2 | `plastic` |

Прошивка не “угадывает” смысл классов из модели. Порядок задан в `src/inference/inference.cpp`, поэтому экспорт модели должен использовать тот же порядок классов.

Доступные `Prediction policy`:
- `SMART_PREDICTION_POLICY_LEGACY_3_CLASS`: обычная 3-классовая классификация.
- `SMART_PREDICTION_POLICY_EXPLICIT_NOTHING_CLASS`: `nothing` приходит как отдельный класс из модели.
- `SMART_PREDICTION_POLICY_CONFIDENCE_THRESHOLD`: если confidence ниже `SMART_CONFIDENCE_THRESHOLD_PERCENT`, выдаётся synthetic `nothing`.
- `SMART_PREDICTION_POLICY_PRESENCE_PREMODEL`: сначала запускается premodel (один MSE-like output); если `gate_mse <= SMART_PRESENCE_MSE_THRESHOLD`, выдаётся synthetic `nothing`, иначе запускается основной классификатор.

На старте `inference_init()`:

- ищет partition `model`;
- загружает ESP-DL модель;
- при `PRESENCE_PREMODEL` policy загружает вторую ESP-DL модель из partition `premodel`;
- печатает input/output tensors;
- выбирает output tensor классификатора (по active policy);
- создаёт `ImagePreprocessor`;
- проверяет input shape `[1,H,W,3]`.

На текущей модели ожидается вход RGB с размером, который берётся из input tensor модели. Если модель обновилась с другим размером входа, код обычно не нужно править: размер читается из модели. Важно, чтобы классификатор имел совместимый RGB input и число классов, соответствующее выбранной policy (3 или 4).

### Preprocessing и scores

Кадр приводится к RGB888 и передаётся в ESP-DL `ImagePreprocessor` с:

```text
mean = [0, 0, 0]
std  = [255, 255, 255]
```

Выходные значения читаются с учётом quantization exponent. Если output выглядит как logits, прошивка применяет softmax. Если output уже похож на probabilities, значения нормализуются.

### Профилирование

В `inference_result_t` логируются:

- `decode_ms`
- `preprocess_ms`
- `infer_ms`
- `total_ms`

Для более глубокого ESP-DL профиля можно включить:

```text
SMART_INFERENCE_ENABLE_SELF_TEST=y
```

Тогда при старте вызываются `model->test()` и `profile_module(true)`. Это debug-режим: он может заметно увеличить время старта и объём логов.

## Действия серв после инференса

Сервы включаются через:

```text
SMART_INFERENCE_SERVO_ACTIONS=y
```

Текущий mapping:

| Prediction | Действие |
| --- | --- |
| `other` / class 0 | servo #1 остаётся в home/safe |
| `paper` / class 1 | servo #1 идёт в `SMART_SERVO1_CLASS1_ANGLE_DEG` |
| `plastic` / class 2 | servo #1 идёт в `SMART_SERVO1_CLASS2_ANGLE_DEG` |
| любой класс | servo #2 после задержки идёт в `SMART_SERVO2_ANY_CLASS_ANGLE_DEG` |

После hold-delay прошивка возвращает сервы в safe положение.

## Flash layout

Разметка находится в `partitions.csv`:

```text
nvs       data  nvs      0x9000    0x6000
phy_init  data  phy      0xf000    0x1000
factory   app   factory  0x10000   0x23e000
model     data  0x82     0x24e000  0x180000
premodel  data  0x83     0x3ce000  0x032000
```

Смысл разделов:

- `nvs`: настройки Wi-Fi/ESP-IDF.
- `phy_init`: RF calibration data.
- `factory`: основная прошивка.
- `model`: основная ESP-DL модель классификатора.
- `premodel`: gate/presence ESP-DL модель (для policy с premodel).

Лимиты сейчас:
- `factory`: `0x23e000` байт.
- `model`: `0x180000` байт.
- `premodel`: `0x032000` байт.

Эти лимиты проверяются в `src/CMakeLists.txt`, чтобы сборка падала заранее, если модель(и) не помещаются.

## Сборка и прошивка

В PowerShell сначала активируйте ESP-IDF окружение:

```powershell
. "C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1"
```

Затем из папки проекта:

```powershell
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p COM3 flash monitor
```

Если менялся только application binary, иногда достаточно:

```powershell
idf.py -p COM3 app-flash monitor
```

Если менялись `partitions.csv`, target, flash size или важные compile-time опции, безопаснее сделать:

```powershell
idf.py fullclean
idf.py build
idf.py -p COM3 flash monitor
```

## Helper-скрипты

В проекте есть Python-скрипты для удобства работы с ESP-IDF окружением.

Сначала создайте `.env` по примеру `.env.example`:

```text
ESP_IDF_VENV_PATH=
ESP_IDF_PROJECT_PATH=
ESP_IDF_PORT=COM3
ESP_IDF_BAUD=115200
ESP_IDF_TARGET=esp32
```

`ESP_IDF_VENV_PATH` должен указывать на ESP-IDF activation script, например PowerShell profile от Espressif.

Открыть shell с активированным ESP-IDF окружением:

```powershell
python scripts\env_activation.py
```

Собрать и прошить через `.env`:

```powershell
python scripts\idf_deploy.py
```

Скрипт `idf_deploy.py`:

- читает `.env`;
- при необходимости делает `idf.py set-target`;
- запускает `idf.py build`;
- запускает `idf.py -p <port> -b <baud> flash`.

## Размер прошивки

Проект рассчитан на 4 MB flash. Базовые оптимизации лежат в `sdkconfig.defaults`:

```text
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE=y
CONFIG_LOG_DEFAULT_LEVEL_ERROR=y
CONFIG_LOG_DYNAMIC_LEVEL_CONTROL=n
CONFIG_ESP_ERR_TO_NAME_LOOKUP=n
```

Быстро посмотреть размер приложения:

```powershell
idf.py size
```

При ошибке вида `app partition is too small` нужно либо уменьшать функциональность, либо менять размер `factory` в `partitions.csv`. При ошибке размера модели нужно уменьшать `models/<SMART_MODEL_PRIMARY_FILENAME>` (и/или `models/<SMART_MODEL_GATE_FILENAME>` для premodel-policy) или пересобирать разметку partition'ов в пределах 4 MB flash.

## Быстрая проверка после прошивки

В `monitor` для inference-профиля должны появиться строки примерно такого вида:

```text
inference: input count=1
inference: output count=1
inference: Inference service is ready
inference: Prediction via logits: class=... label=... confidence=...
boot: Timing ms: capture=... decode=... preprocess=... infer=... total=...
```

Если включены servo actions, после prediction должны быть логи `Servo action mapping` и движения серв.
