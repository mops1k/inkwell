# Эмуляция прошивки: `ui_sim` и `qemu_boot`

Два инструмента, которые позволяют видеть и проверять прошивку **без загрузки на X3**.
Ни один из них не заменяет приёмку на устройстве (план `p_e2f3b165ca94`, фаза Ф7).

- **`tools/ui_sim`** — рисует экран настоящим `GfxRenderer` и настоящими шрифтами на хосте и сохраняет кадры PNG. Быстро, детерминированно, входит в `ctest`.
- **`tools/qemu_boot.sh`** — грузит собранный `firmware.bin` в Espressif QEMU (ESP32-C3) и снимает UART-лог. Показывает только ранний boot.

## ui_sim: визуальная проверка экрана

```bash
cmake -S tools/ui_sim -B build/ui_sim -DCMAKE_BUILD_TYPE=Release
cmake --build build/ui_sim -j
./build/ui_sim/ui_sim --out /tmp/frames --scenario tools/ui_sim/scenarios/demo.json
```

Результат — `frame_000.png`, `frame_001.png`, … в указанном каталоге, уже в читательской
ориентации (панельный кадр 792×528 поворачивается в 528×792).

Сценарий — обычный JSON, сейчас поддерживаются только нажатия:

```json
{ "keys": ["Right", "Right", "Confirm", "Left"] }
```

`Left`/`Right` двигают выбор в демо-сцене, каждый следующий кадр сохраняется отдельным файлом.

### Что внутри

- Компилируются **реальные** `lib/GfxRenderer/GfxRenderer.cpp` (+`Bitmap`, `BitmapHelpers`), `lib/EpdFont`, `lib/Utf8`, `src/UiFonts.cpp` — тот же пиксельный путь, что на устройстве.
- Подменены только границы железа: `tools/ui_sim/host/HalDisplay.h` (кадр в память, 52 272 Б), `host/HalGPIO.h` (клавиши из сценария), `host/HostEsp.cpp` (heap-хелперы), `host/Stubs.cpp` (SD-шрифты и декомпрессор — заглушки).
- Рисует **встроенными** шрифтами; SD-шрифты в симуляторе не работают.

### Что покрывает тест

`ui_sim_demo` в главном `ctest` рендерит 5 кадров и падает, если кадр пустой (нет ни одного
закрашенного пикселя) или PNG не записался. Golden-сравнение кадров появится, когда экран
полки стабилизируется в Ф3.

### Как добавить настоящий экран

Экран полки (Ф3) встраивается вместо `ShelfDemoScene`: сцена получает `GfxRenderer&`,
рисует в его фреймбуфер и вызывает `displayBuffer()`; harness и PNG-вывод менять не нужно.

## qemu_boot.sh: проверка загрузки

```bash
.venv/bin/pio run -e default        # собрать прошивку
./tools/qemu_boot.sh 20             # 20 секунд прогона, лог в build/qemu/boot.log
```

Скрипт собирает полный flash-образ 16 МБ (`bootloader` 0x0, `partitions` 0x8000, `boot_app0`
0xe000, `firmware` 0x10000) через `esptool merge-bin` и запускает:

```bash
qemu-system-riscv32 -nographic -machine esp32c3 -drive file=flash.bin,if=mtd,format=raw
```

### Что нужно установить один раз (без изменения системы)

- Espressif QEMU 9.2.2, релиз `esp-develop-9.2.2-20261109`, ассет
  `qemu-riscv32-softmmu-esp_develop_9.2.2_20261109-x86_64-linux-gnu.tar.xz` →
  распаковать в `.venv/qemu/`; SHA-256 сверяется по `qemu-esp_develop_9.2.2_20261109-checksum.sha256`.
- `libslirp.so.0` — единственная библиотека, которой нет в системе. Кладётся локально в
  `.venv/qemu/lib` (распаковка пакета дистрибутива), подключается через `LD_LIBRARY_PATH`.
- `esptool` в `.venv` (`/.venv/bin/esptool`) для `merge-bin`.

`sudo` не требуется: система не меняется, всё лежит в `.venv/` (каталог в `.gitignore`).

### Что показывает прогон (проверено 18.11.2026)

1. `ESP-ROM:esp32c3-api1-20210207` → bootloader → `entry 0x403cbf10` — прошивка **грузится**.
2. `[E][Preferences.cpp:47] begin(): nvs_open failed: NOT_FOUND` — NVS в образе не отформатирован (ожидаемо для чистой эмуляции).
3. `i2c_master_transmit_receive failed: [259] ESP_ERR_INVALID_STATE` — I2C-устройства (gauge/датчики) не эмулируются.
4. `Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)` → перезагрузочный цикл: прошивка ждёт железо (панель UC8253 `BUSY`, SD, I2C), которого в QEMU нет.

### Чего QEMU не умеет и почему это ожидаемо

- Панель **UC8253** и SD-карта (SPI) не эмулируются — UI и работу с библиотекой проверить нельзя.
- Виртуальный framebuffer QEMU (`-graphics`) предназначен для `esp_lcd_qemu_rgb`, а не для e-ink контроллера.
- Поэтому «загрузка прошла» здесь означает только: bootloader + ранняя инициализация без паники в коде, не трогающем железо.

## Правила использования

- Визуальную вёрстку проверяем `ui_sim` до прошивки; на X3 — только приёмка Ф7.
- Если после изменений в коде раннего boot QEMU начал падать раньше прежнего места — это сигнал, но не приговор: сравнение с предыдущим `boot.log` обязательно.
- `build/qemu/` — артефакты прогона, в репозиторий не попадают.
