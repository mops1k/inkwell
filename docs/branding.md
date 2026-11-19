# Брендинг Inkwell

Прошивка форка называется **Inkwell**. Название и иконка утверждены 18.11.2026.

## Откуда берётся имя

Единственный источник — `platformio.ini`:

```ini
[crosspoint]
name = "Inkwell"
version = 1.0.0
```

Версия форка — самостоятельная нумерация, не совпадающая с апстримом: первый публичный релиз
помечен `1.0.0` (19.11.2026), до него в дереве стояло `0.0.1`. Обновление с SD-карты версию не
проверяет (`FirmwareImageValidator` смотрит только структуру образа, chip ID, размер, XOR-сумму и
SHA-256), поэтому на установку с карты номер не влияет. Он влияет на:
отображение версии (экран загрузки, «О системе»), имена релизных файлов и `OtaUpdater::isUpdateNewer()`,
который сравнивает версии внутри нашего OTA-канала.

Из него `scripts/git_branch.py` собирает строку версии (`Inkwell 2.3.0`), она попадает в:

- «О системе» (`SystemInformationActivity`) и экран обновления;
- веб-интерфейс — плейсхолдер `%%CROSSPOINT%%` в `src/network/html/*.html`;
- `CROSSPOINT_VERSION` в сборке.

## Что ещё переименовано

| Место | Было | Стало |
|---|---|---|
| Точка доступа Wi-Fi (`CrossPointWebServerActivity.cpp`) | `CrossPoint-Reader` | `Inkwell-Reader` |
| mDNS-хост веб-сервера | `crosspoint` | `inkwell` |
| mDNS-хост Calibre | `crosspoint` | `inkwell` |
| Hostname в Wi-Fi-сети (`WifiSelectionActivity.cpp`) | `CrossPoint-Reader-<MAC>` | `Inkwell-Reader-<MAC>` |
| Заголовки и футеры веб-страниц | `CrossPoint Reader` | `Inkwell Reader` / `Inkwell` |
| Имя релизного файла (`scripts/package_stable_release.py`, workflows) | `firmware-Paperio-Reader-<ver>.bin` | `firmware-inkwell-<ver>.bin` |
| i18n `STR_CROSSPOINT` (24 языка) | `Paperio Reader` | `Inkwell` |
| i18n `STR_SYSTEM_UPDATE_TYPE1` | `Обновления Paperio Reader` | `Обновления Inkwell` |
| User-Agent HTTP (`HttpDownloader`, KOReader-синхронизация) | `PaperioReader-ESP32-<ver>` | `Inkwell-ESP32-<ver>` |

Строку `STR_CALIBRE_INSTRUCTION_1` («плагин CrossPoint Reader») **не** меняем: это название внешнего плагина Calibre.

## Иконка

`src/images/Logo120.h` — 120×120, 1 бит на пиксель, MSB-first, **1 = белый**. Рисуется в трёх местах:

- `BootActivity` — экран загрузки;
- `SleepActivity` — экран сна;
- `SystemInformationActivity` — «О системе».

Мотив: чёрный круг, внутри белая строчная «i», точка которой — капля чернил (Inkwell).

### Как пересобрать иконку

`scripts/convert_icon.py` требует `cairosvg` (в `.venv` не установлен). Рабочий путь без него:

```bash
# 1. SVG → PNG нужного размера (librsvg)
rsvg-convert -w 120 -h 120 icon.svg -o icon-120.png

# 2. PNG → сырой grayscale (ffmpeg)
ffmpeg -y -i icon-120.png -pix_fmt gray -f rawvideo icon.gray

# 3. упаковка в C-заголовок: 1 = белый, MSB-first, 15 байт на строку
python3 scripts/pack_logo.py icon.gray 120 120 src/images/Logo120.h
```

Проверка заголовка без устройства: распаковать 1800 байт обратно в PGM и посмотреть картинку —
полярность (`1 = белый`) и порядок битов должны совпасть с оригиналом, у которого первая строка `0xFF`.

## Открытый вопрос: имя файла конфига

Обсуждается переименование `platformio.ini` → `inkwell.ini`. Проверенные факты:

- PlatformIO 6.2.0 поддерживает `pio run -c inkwell.ini` (`-c, --project-conf FILE`);
- `platformio.ini` жёстко читают `scripts/git_branch.py` (4 места) и `scripts/build_html.py` (2),
  а также CI-воркфлоу (`pio run` без `-c`, `hashFiles('platformio.ini')`, `config.read("platformio.ini")`);
- расширение PlatformIO для VS Code ищет `platformio.ini` и при переименовании перестанет
  определять проект автоматически;
- внутри конфига есть `extra_configs = platformio.local.ini` — локальные переопределения.

Решение ещё не принято; вариант-компромисс — `inkwell.ini` + симлинк `platformio.ini`.
