# smart-trash-firmware

Все команды запускать из корня проекта.
ESP-IDF должен быть уже установлен локально.

## 1. Подготовка

1. Создайте файл `.env` на основе `.env.example`.
2. Заполните значения:

| Переменная | Что указать | Пример |
| --- | --- | --- |
| `ESP_IDF_VENV_PATH` | Путь к скрипту активации ESP-IDF | `C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1` |
| `ESP_IDF_PROJECT_PATH` | Путь к этому проекту | `C:\Dev\Projects\smart-trash-firmware` |
| `ESP_IDF_TARGET` | Целевой чип | `esp32` |
| `ESP_IDF_PORT` | COM-порт платы | `COM3` |
| `ESP_IDF_BAUD` | Скорость прошивки | `115200` |

3. Установите Python-зависимости проекта:

```bash
uv sync
```

## 2. Ручная работа с ESP-IDF

Если нужно просто открыть окружение ESP-IDF и дальше вводить команды вручную:

```bash
uv run python scripts/env_activation.py
```

Скрипт активирует окружение ESP-IDF и открывает консоль, в которой можно запускать обычные команды, например:

```bash
idf.py build
idf.py flash
idf.py monitor
```

## 3. Сборка и прошивка ESP32

Если нужно собрать проект и сразу прошить плату:

```bash
uv run python scripts/idf_deploy.py
```

Скрипт сам:

1. активирует окружение ESP-IDF;
2. переходит в папку проекта;
3. при необходимости выставляет `ESP_IDF_TARGET`;
4. выполняет `idf.py build`;
5. выполняет прошивку через `idf.py -p <PORT> -b <BAUD> flash`.
