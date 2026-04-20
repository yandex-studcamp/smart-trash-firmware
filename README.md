# smart-trash-firmware

Прошивка для `ESP32-CAM` умной корзины. Сейчас проект находится на этапе bring-up: проверяем железо, камеру, модель и минимальную интеграцию без лишнего раздувания прошивки под flash 4MB.

## Текущий функционал

Есть два основных boot-профиля:

- `Servo smoke test` — проверка схемы с двумя SG90 на `GPIO13` и `GPIO14`.
- `Inference service` — загрузка модели из partition `model`, захват кадра с камеры, инференс и вывод результата в логи.

Сетевой слой `SoftAP + HTTP` оставлен как debug-инструмент и по умолчанию выключен, чтобы не занимать flash.

## Структура проекта

```text
src/
├─ app/        # app_main и сценарий запуска
├─ inference/  # inference.cpp/.h, загрузка модели и C API инференса
├─ camera/     # camera.cpp/.hpp, ESP32-CAM capture
├─ network/    # softap.cpp/.hpp, http_server.cpp/.hpp, только для debug API
├─ test/       # servo_smoke_test.cpp/.hpp
├─ board/      # board_pins.hpp, board_config.hpp, board_profile.hpp
├─ CMakeLists.txt
└─ Kconfig.projbuild
```

- `inference/inference.h`, `inference/inference.cpp`
- `camera/camera.hpp`, `camera/camera.cpp`
- `network/softap.hpp`, `network/softap.cpp`
- `network/http_server.hpp`, `network/http_server.cpp`


- `app/` управляет сценарием запуска.
- `inference/` отвечает только за модель и предсказание.
- `camera/` отвечает только за кадр.
- `test/` проверяет железо.
- `network/` остаётся debug-слоем.
- `board/` централизует железные константы.

## Board Layer

`src/board/` содержит compile-time константы платы:

- `board_pins.hpp` — пины сервоприводов и камеры.
- `board_config.hpp` — углы серв, PWM-тайминги, базовые настройки камеры.
- `board_profile.hpp` — compile-time признаки выбранного профиля.

Важно: это тонкий слой над `Kconfig`, без runtime-аллоцирования и без дополнительных задач. Он нужен только чтобы не размазывать pin mapping и board-level значения по `camera/`, `test/` и `app/`.

## Inference API

Публичный C API для будущих модулей:

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

Классы сейчас фиксированы в firmware в порядке:

- `other = 0`
- `paper = 1`
- `plastic = 2`

Этот порядок должен совпадать с актуальным экспортом модели. Прошивка не читает имена классов из `.espdl` автоматически.

Текущая модель имеет вход `128x128 RGB` и несколько выходов. Для классификации выбирается output-тензор размером `3`, сейчас это `main_logits`.

При следующем обновлении модели обычно нужно проверить только три вещи:

- порядок классов в `inference/inference.cpp` (`kClassLabels`);
- что размер `models/model.espdl` влезает в partition `model`;
- при необходимости изменить `Sample dump output side (square, pixels)` в `menuconfig`.

## Servo Smoke Test

Профиль нужен для проверки схемы ESP32-CAM + 2x SG90:

- `GPIO13` -> сигнал Servo #1
- `GPIO14` -> сигнал Servo #2
- общий `GND` для ESP32-CAM и серв
- стабильное питание `5V`
- желательно поставить электролитический конденсатор `100-470 мкФ` между `5V` и `GND`

Поведение:

- servo1: `0 -> 90 -> 180 -> 90`
- servo2: зеркально
- задержка между позициями задаётся через `SMART_SERVO_DWELL_MS`

## Inference Service

При старте или нажатии `RST` в inference-профиле:

1. Загружается модель из partition `model`.
2. Печатаются input/output тензоры модели.
3. Инициализируется камера.
4. Захватывается кадр.
5. Кадр передаётся в `inference_run_rgb888`.
6. В логи выводятся все output-тензоры и итоговое предсказание.

Пример ожидаемых строк:

```text
E (...) inference: Inference service is ready (3 classes, 128x128 input, output=main_logits)
E (...) inference: output values: name=main_logits shape=[1,3] size=3
E (...) inference: Prediction via main_logits: class=... label=... confidence=... scores=[... ... ...]
```

## Sample Artifacts

Для отладки можно сохранять последние кадры с ESP32-CAM на компьютере в `logs/samples`.

Критичный момент: ESP32-CAM с flash 4MB сейчас не имеет свободного partition под фото. Поэтому кадры не сохраняются во flash платы. Вместо этого debug-режим печатает кадр в serial log как hex-encoded JPEG, а host-скрипт собирает из этого нумерованные файлы.

По умолчанию сохраняется JPEG preview `128x128` (center-crop + resize), а не полный QVGA кадр. Это сделано специально: полный RGB кадр через UART печатается слишком долго и может вызвать Task Watchdog reset.

Включить в `menuconfig`:

- `Boot Profile = Inference service`
- `Dump captured boot samples to serial log for host-side saving = y`
- `Sample dump output side (square, pixels) = 128`

После этого можно запустить монитор через скрипт:

```bash
idf.py -p COM3 monitor | python tools/save_serial_samples.py
```

Скрипт создаёт:

```text
logs/samples/
├─ sample_0001.jpg
├─ sample_0001.log
├─ sample_0002.jpg
└─ sample_0002.log
```

`JPEG` выбран специально: он компактнее по UART и сразу открывается в стандартных просмотрщиках без дополнительной конвертации.

Если уже есть сохранённый monitor log, можно разобрать его отдельно:

```bash
python tools/save_serial_samples.py build_full.log
```

Для обычной прошивки эту опцию держите выключенной: дамп большого кадра через UART медленный и нужен только для диагностики.

## Network Debug API

`network/` не является основой продукта, это debug-адаптер поверх того же inference API.

Если включить `SMART_ENABLE_NETWORK_DEBUG_API`, доступны:

- `GET /health` — возвращает `ok` или `not_ready`.
- `POST /infer` — принимает raw JPEG и возвращает `label confidence`.

По умолчанию network debug API выключен, потому что `esp_wifi`, `esp_http_server`, `esp_netif` заметно увеличивают app binary.

## Разметка Flash

Текущий layout под flash 4MB:

```text
nvs       0x009000  0x006000
phy_init  0x00f000  0x001000
factory   0x010000  0x270000
model     0x280000  0x180000
```

Модель лежит отдельно в partition `model`, чтобы её можно было прошивать отдельно от приложения.

Ручная прошивка модели:

```bash
python $env:IDF_PATH\components\esptool_py\esptool\esptool.py --chip esp32 --port COM3 --baud 460800 write_flash 0x280000 models/model.espdl
```

Если нужно очистить только model-раздел:

```bash
python $env:IDF_PATH\components\esptool_py\esptool\esptool.py --chip esp32 --port COM3 --baud 460800 erase_region 0x280000 0x180000
```

## Сборка и прошивка

Обычная сборка:

```bash
idf.py build
```

Прошить только приложение:

```bash
idf.py -p COM3 app-flash monitor
```

Полная прошивка:

```bash
idf.py -p COM3 flash monitor
```

После изменения `partitions.csv`, `sdkconfig`, зависимостей или крупных CMake-условий лучше выполнить:

```bash
idf.py fullclean
idf.py reconfigure
idf.py build
```

После обычных правок `.cpp/.hpp` обычно достаточно:

```bash
idf.py build
idf.py -p COM3 app-flash monitor
```

## Рекомендуемые настройки для 4MB Flash

Для локального boot-inference без лишнего веса:

- `Boot Profile = Inference service`
- `Capture camera image and run inference on every boot/reset = y`
- `Enable JPEG input decode path in inference core = n`
- `Enable SoftAP + HTTP debug API = n`
- `Flash models/model.espdl together with app = y`

Для проверки серв:

- `Boot Profile = Servo smoke test`
