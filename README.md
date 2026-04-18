# smart-trash-firmware

интеграция ESP-DL для ESP32-CAM с загрузкой модели только из отдельного flash-раздела `model`

<!--
## Что настроено

- кастомная таблица разделов в `partitions.csv` с отдельным разделом `model`
- прошивка `.espdl` в `model` через CMake (`esptool_py_flash_to_partition`)
- минимальный поток в `app_main()`:
  - лог старта приложения
  - загрузка модели из partition (`dl::Model("model", fbs::MODEL_LOCATION_IN_FLASH_PARTITION)`)
  - вызов `test()` с логом результата
  - логи профилирования памяти и задержки (`profile_memory()`, `profile_module(true)`)
  - логи heap/времени -->

## Куда класть файл модели

`models/model.espdl`

## Сборка и прошивка

1. Настройте окружение ESP-IDF.
2. Соберите проект:

```bash
idf.py build
````

3. Прошейте приложение и модель:

```bash
idf.py -p <PORT> flash monitor
```

## Обновить только код приложения

```bash
idf.py -p <PORT> app-flash monitor
```

`app-flash` не перезаписывает раздел `model`.

## Перепрошить модель после её замены

1. Замените `models/model.espdl`.
2. Снова выполните прошивку:

```bash
idf.py -p <PORT> flash
```

Это обновит и приложение, и раздел модели одновременно.

Опционально можно прошить только модель через `esptool.py` (`model` offset — `0x200000` из `partitions.csv`):

(не проверялось)

```bash
python $IDF_PATH/components/esptool_py/esptool/esptool.py --chip esp32 --port <PORT> --baud 921600 write_flash 0x200000 models/model.espdl
```
