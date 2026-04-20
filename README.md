# smart-trash-firmware

Прошивка для `ESP32-CAM` умной корзины. Сейчас проект в стадии bring-up: проверяем железо, камеру, модель и базовый inference-flow на плате с 4MB flash.

## Текущий функционал

Есть два boot-профиля:

- `Servo smoke test` — проверка 2x SG90 на `GPIO13` и `GPIO14`.
- `Inference service` — загрузка модели из partition `model`, захват кадра, инференс, логи предсказания и таймингов.

Сетевой слой `SoftAP + HTTP` оставлен как debug-инструмент и по умолчанию выключен, чтобы не раздувать образ.

## Структура проекта

```text
src/
├─ app/        # boot flow, sample dump
├─ inference/  # inference.cpp/.h, C API инференса
├─ camera/     # camera.cpp/.hpp, работа с ESP32-CAM
├─ network/    # softap/http debug adapter
├─ test/       # servo smoke test
├─ board/      # board-level константы (pins/config/profile)
├─ CMakeLists.txt
└─ Kconfig.projbuild
```

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

Порядок классов в прошивке сейчас фиксирован:
- `other = 0`
- `paper = 1`
- `plastic = 2`

Текущая модель: вход `128x128 RGB`, выход классификации размером `3` (`main_logits`).

## Sample dump (JPEG)

Для отладки можно сохранять sample-кадры из serial-лога в `logs/samples`.
На плате кадр кодируется в JPEG и печатается hex-блоками (`SAMPLE_BEGIN/SAMPLE_DATA/SAMPLE_END`), на хосте скрипт собирает файлы.

Включить в `menuconfig`:
- `Boot Profile = Inference service`
- `Dump captured boot samples to serial log for host-side saving = y`
- `Sample dump output side (square, pixels) = 128`

Из лога сохранять так:

```bash
python tools/save_serial_samples.py logs/monitor.log --out logs/samples
```

## Flash layout (4MB)

```text
nvs       0x009000  0x006000
phy_init  0x00f000  0x001000
factory   0x010000  0x270000
model     0x280000  0x180000
```

Если нужно прошить модель вручную:

```bash
python $env:IDF_PATH\components\esptool_py\esptool\esptool.py --chip esp32 --port COM3 --baud 460800 write_flash 0x280000 models/model.espdl
```

## Удобные скрипты (из main)

В ветке `main` добавлены helper-скрипты для локальной работы с ESP-IDF.

1. Подготовка окружения:
   - создать `.env` из `.env.example`
   - установить зависимости:

```bash
uv sync
```

2. Открыть консоль с активированным ESP-IDF:

```bash
uv run python scripts/env_activation.py
```

3. Сборка и прошивка одной командой:

```bash
uv run python scripts/idf_deploy.py
```

## Стандартные команды

Обычная сборка:

```bash
idf.py build
```

Прошить только приложение:

```bash
idf.py -p COM3 app-flash monitor
```

Полная прошивка (включая table/model если настроено):

```bash
idf.py -p COM3 flash monitor
```

## При обновлении модели

Обычно нужно проверить три вещи:

1. Порядок классов в `src/inference/inference.cpp` (`kClassLabels`).
2. Что размер `models/model.espdl` влезает в `model` partition.
3. Нужен ли другой размер sample dump (`Sample dump output side`).
