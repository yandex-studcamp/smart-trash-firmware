# smart-trash-firmware

Проект прошивки для `ESP32-CAM` с двумя профилями запуска:

- `Servo smoke test` (по умолчанию)
- `Inference service` (камера + модель, с опциональным SoftAP/HTTP)

## Что оптимизировано сейчас

Чтобы прошивка была легче и стабильнее для текущей отладки:

- сетевой debug API (`SoftAP + HTTP`) вынесен в отдельный флаг и по умолчанию выключен;
- автопрошивка `models/model.espdl` вместе с `idf.py flash` вынесена в отдельный флаг и по умолчанию выключена.

Это позволяет не упираться в переполнение flash при большой модели и быстрее прошивать/тестировать код.

## Профили загрузки

### 1) Servo smoke test

Проверка схемы с двумя `SG90`:

- PWM на `GPIO13` и `GPIO14`;
- цикл углов: `0 -> 90 -> 180 -> 90`;
- подробные логи в `monitor`.

### 2) Inference service

При старте/`RST`:

1. инициализируется модель;
2. инициализируется камера;
3. делается кадр с камеры (облегченный RGB-путь без JPEG decode по умолчанию);
4. запускается инференс по кадру;
5. в лог печатаются:
   - класс/метка/confidence;
   - вектор score;
   - тайминги (`capture`, `decode`, `preprocess`, `infer`, `total`);
   - снимки heap/psram по этапам.

Если включен сетевой debug API, дополнительно поднимаются:

- `GET /health`
- `POST /infer`

## Структура `src/`

- `src/app/` — `app_main`, boot flow, выбор профиля
- `src/test/` — тесты железа (сервоприводы)
- `src/inference/` — ядро инференса, C API
- `src/camera/` — захват кадров ESP32-CAM
- `src/network/` — SoftAP и HTTP адаптер

## Сборка и прошивка

```bash
idf.py fullclean
idf.py menuconfig
idf.py build
idf.py -p <PORT> flash monitor
```

## Рекомендуемый режим на сейчас (облегченный)

В `menuconfig`:

`Smart Trash Firmware -> Boot Profile -> SoftAP + HTTP inference service`

и внутри этого профиля:

- `Capture camera image and run inference on every boot/reset = y`
- `Enable JPEG input decode path in inference core = n`
- `Enable SoftAP + HTTP debug API = n`
- `Flash models/model.espdl together with app = n`

Так вы тестируете камеру/логики/инференс boot-flow без тяжелого сетевого слоя и без фейла `esptool` из-за oversized модели.

## Когда нужен HTTP debug API

Включите:

- `Enable SoftAP + HTTP debug API = y`

И тогда будут доступны endpoints:

- `GET /health`
- `POST /infer`

## Работа с моделью

Файл модели:

- `models/model.espdl`

Если включен флаг `Flash models/model.espdl together with app`, модель прошивается автоматически во время `idf.py flash`.

Текущий offset раздела `model`:

- `0x19e000`

Ручная прошивка модели:

```bash
python $IDF_PATH/components/esptool_py/esptool/esptool.py --chip esp32 --port <PORT> --baud 921600 write_flash 0x19e000 models/model.espdl
```

## Важно про flash 4MB

При больших `app` и `model` одновременно можно уткнуться в лимит 4MB.
Если видите ошибку `will not fit in ... bytes of flash`, временно держите:

- `Enable SoftAP + HTTP debug API = n`
- `Flash models/model.espdl together with app = n`

и прошивайте только приложение, пока не уменьшите модель или не перейдете на плату/конфиг с большим flash.
