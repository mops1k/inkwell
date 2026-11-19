# QEMU: полная эмуляция Xteink X3 (R&D)

План: `p_3b928142cc55`. Здесь лежит то, что понадобилось, чтобы запустить **реальный**
`firmware.bin` в Espressif QEMU и начать приближать его к полной эмуляции платы.

## Почему prebuilt-бинарника недостаточно

Официальная матрица Espressif для ESP32-C3 (см. `esp-toolchain-docs/qemu`) не покрывает
то, на чём стоит прошивка: **GP SPI** (панель UC8253 и SD-карта), **I2C** (gauge/RTC),
**GPIO matrix/IOMUX** (кнопки, DC/RST/BUSY панели, CS карты), **SD/MMC**. Поэтому реальный
бинарник грузится, доходит до инициализации и падает в `Interrupt wdt timeout`, ожидая
периферию. Готовый бинарник расширить нельзя — нужна сборка из исходников.

## Сборка из исходников (проверено 18.11.2026)

```bash
cd .venv/qemu
curl -sSL -o qemu-src.tar.xz \
  https://github.com/espressif/qemu/releases/download/esp-develop-9.2.2-20261109/qemu-esp_develop_9.2.2_20261109-src.tar.xz
mkdir -p src && tar -xf qemu-src.tar.xz -C src --strip-components=1
cd src
for p in ../../../tools/qemu/patches/*.patch; do patch -p0 < "$p"; done   # наши правки форка
export PATH=$PWD/../../../.venv/bin:$PATH
export PKG_CONFIG_PATH=$PWD/../slirp/pkgconfig
./configure --target-list=riscv32-softmmu --enable-gcrypt \
  --disable-gtk --disable-sdl --disable-vnc --disable-docs --disable-tools --disable-werror
ninja -C build qemu-system-riscv32
```

Особенности WSL/Arch, которые пришлось обойти без `sudo`:

- `libslirp` без dev-пакета: распакован из пакета Arch в `.venv/qemu/slirp` + локальный
  `slirp.pc` (QEMU ищет зависимость по имени `slirp`, а `Cflags` должен указывать на
  `include/slirp`, иначе `libslirp.h` не находится).
- `libgcrypt` обязателен: без него не собираются `esp32c3_aes/rsa/ds`, и машина падает
  на старте с `unknown type 'misc.esp32c3.aes'`. Флаг — `--enable-gcrypt`.
- `-Werror` выключен: bundled `dtc/libfdt` не собирается новым GCC.

## Патчи форка

| Патч | Что делает |
|---|---|
| `0001-esp32c3-wire-i2c-controller.patch` | инстанцирует `esp32.i2c` в машине C3: MMIO на `DR_REG_I2C_EXT_BASE`, IRQ в interrupt matrix (`ETS_I2C_EXT0_INTR_SOURCE`) |
| `0002-i2c-model-for-esp32c3.patch` | собирает `hw/i2c/esp32_i2c.c` для `CONFIG_RISCV_ESP32C3` (в форке модель была только для Xtensa) |
| `0003-esp32c3-gpio-matrix-and-input-lines.patch` | GPIO-матрица C3: `FUNC0_IN/OUT_SEL_CFG`, `STATUS/W1TS/W1TC`, `PINx_CNF`, входные `qemu_irq`-линии; в машине промаплен IO_MUX (`0x60009000`) |
| `0004-xteink-buttons-chardev-device.patch` | девайс `xteink-buttons`: символы chardev → уровни GPIO (active-low), сборка `hw/gpio/xteink_buttons.c` для C3 |
| `0005-esp32c3-sar-adc.patch` | модель APB_SARADC (C3): oneshot-конверсия завершается сразу, `ADC1_DONE` в `INT_RAW`, данные в `1_DATA_STATUS` (idle raw = 4095 = «кнопка не нажата»); убирает WDT в `analogRead` |
| `0006-esp32c3-gpio-interrupts.patch` | GPIO-прерывания C3: `INT_TYPE`/`INT_ENA` из `PINx_CNF`, `STATUS` на фронт/уровень, level-IRQ в intmatrix (`ETS_GPIO_INTR_SOURCE`) |
| `0007-xteink-adc-buttons.patch` | АЦП-лестница X3: per-channel raw в SARADC (ADC1 ch N = GPIO N) + свойства `<key>-adc`/`<key>-adc-raw` у `xteink-buttons` |
| `0008-esp32c3-gpspi2-controller.patch` | модель GP-SPI2 (`ssi.esp32c3.gpspi`): CMD/USR, USER/USER1/USER2, `MS_DLEN`, FIFO `W0..W15` @0x98; собирается для `CONFIG_RISCV_ESP32C3` |
| `0009-esp32-gpio-output-lines.patch` | выходные линии GPIO (`qdev_init_gpio_out`, 40 пинов): уровень `gpio_out & gpio_enable`, чтобы CS/DC/RST панели и CS карты шли из GPIO-матрицы, как на железе |
| `0010-xteink-uc8253-panel.patch` | панель UC8253 как SSI-периферия (`xteink.uc8253`): DC/RST на вход, BUSY на выход, таймер возврата BUSY в idle |
| `0011-esp32c3-i2c-restart-and-x3-chips.patch` | RSTART-опкод C3 (6) в модели I2C + чипы X3 на шине (`xteink.x3-i2c`: BQ27220 0x55, DS3231 0x68, QMI8658 0x6B) |
| `0012-esp32c3-machine-xteink-board.patch` | проводка платы в машине C3: SPI2 по `0x60024000`, панель и `ssi-sd` на его SSI-шине, GPIO21/4/5/6/12, три I2C-чипа |
| `0013-esp32c3-i2c-v2-command-encoding.patch` | **Q3:** кодировка команд i2c v2 у C3: `READ=3`, `STOP=2` (у Xtensa `READ=2`, `STOP=3`). Свойство `riscv-cmd-encoding` (`DEFINE_PROP_BOOL`) выбирает карту, машина C3 его включает. Без этого READ декодировался как STOP: транзакция обрывалась после адреса, RX FIFO пустой, `requestFrom()` возвращал 0 |
| `0014-esp32c3-gpspi-single-data-length.patch` | **Q3:** `SPI_MS_DLEN` у C3 — одно поле `SPI_MS_DATA_BITLEN [17:0]` на обе линии (а не `MOSI [9:0]`/`MISO [19:10]`, как у ESP32). Без этого MISO читался 1 битом, R1-ответ SD приходил как 0x00 |
| `0015-esp32c3-xteink-sd-card-spi-mode.patch` | **Q3:** карта на `ssi-sd` создаётся как `sd-card-spi`, а не `sd-card`: только SPI-таблица команд отвечает на CMD0 настоящим R1 = idle, нативная возвращает «нет ответа» → ssi-sd отдаёт 0x04, SdFat вечно повторяет CMD0 |
| `0016-qemu-i2c-gpspi-debug-traces.patch` | **Q3:** отладочные трассы, по умолчанию выключены: `ESP32_I2C_TRACE=1` (регистры I2C), `ESP32_GPSPI_TRACE=1` (одна строка на SPI-транзакцию, до 4000) и включённый в машине `[X3I2C]`-лог трёх чипов |
| `0017-esp32c3-gpio-pcpu-int-register.patch` | **Q4, главный фикс:** регистр `GPIO_PCPU_INT` (`0x5c`) у C3 — pending-набор «status & INT_ENA». `gpio_intr_service()` из ESP-IDF читает именно его, и без него ISR видел пустой набор, выходил, **не сбрасывая** `STATUS`, level-IRQ оставался поднятым → CPU навсегда в обработчике (ровно то, что Q3 видел как «idle после DRF»). Теперь завершение BUSY доходит до `waitRefreshComplete()` |
| `0018-xteink-uc8253-frame-ram-and-busy-timing.patch` | **Q4:** панель хранит оба RAM-плоскости (DTM1/DTM2, 99×528 байт) и на каждый `0x12` пишет запись кадра в chardev (заголовок `X3PN` + обе плоскости); реалистичные длительности BUSY (PON 50 / DRF 300 / POF 30 / DSL 5 мс, переопределяются `XTEINK_PANEL_BUSY_*_MS`), трассы `XTEINK_PANEL_TRACE_BUSY=1` и `XTEINK_PANEL_TRACE_BYTES=1`, счётчик принятых байт данных в строке команды |
| `0019-esp32c3-machine-panel-frame-chardev.patch` | **Q4:** машина C3 получает свойство `xteink-panel=<chardev-id>` и привязывает chardev к создаваемой ею панели (`-global` не может адресовать тип с точкой в имени) |
| `0020-xteink-x3-i2c-wide-branch-trace.patch` | **Q4:** `[X3I2C]` пишется и для `wide`-ветки, т.е. для газ-гейджа BQ27220 `0x55` (раньше трасса стояла только в индексированной ветке: в логе были только DS3231 и QMI8658) |
| `0021-qemu-ssi-sd-r1-and-trace.patch` | **Q4:** `ssi-sd` собирает настоящий R1 для CMD8/CMD58 из состояния карты (было жёстко `1` = idle), из-за чего SdFat после ACMD41 считал карту не вышедшей из idle и не монтировал том; плюс трасса `SSISD_TRACE=1` (команда/аргумент/R1/блок, с виртуальным временем) |
| `0022-xteink-adc-conversion-trace.patch` | **Q4:** трасса завершённых АЦП-конверсий `XTEINK_ADC_TRACE=1` — печатает только значения, отличные от idle, т.е. ровно те, что видит `analogRead()` при нажатой кнопке (доказательство, что инъекция `xteink-buttons` доходит до прошивки) |
| `0023-qemu-ssi-sd-spi-write.patch` | **Q6:** `ssi-sd` умеет писать в SPI-режиме: `CMD24`/`CMD25` с приёмом DATA-токена `0xFE`/`0xFC` и 512 байт, `STOP_TRANSMISSION` (`CMD12`/токен `0xFD`), data response token `0x05`, BUSY-фаза после блока, трасса `data token`/`data=512 write done`. Корень проблемы — переход из состояния ответа происходил на байт позже, поэтому токен съедался |
| `0024-qemu-sd-spi-r1-and-status.patch` | **Q6:** `sd.c` в SPI-режиме отвечает настоящим R1 даже на illegal-команду (раньше отдавал 0 байт, и `ssi-sd` подставлял `0x04`), а `CMD13` в SPI отдаёт 32-битный статус (из него `ssi-sd` собирает R2 из 2 байт), а не 16 байт CSD |
| `0025-xteink-uc8253-screen-state-and-4level-frame.patch` | **Q7:** панель хранит не только обе RAM-плоскости, но и **скомпонованный экран** (2 бита на пиксель: 0 paper / 1 light gray / 2 dark gray / 3 black) и отдаёт его в записи кадра как плоскости 3–4 (`planes=4`, `version=2`, флаг `0x2` = «refresh был 4-градационным»). Плюс распознавание 4-градационной пары по хвосту транзакции: `DTM1` (LSB) и сразу за ним `DTM2` (MSB), причём первый payload — не константная заливка `0xFF`. Раньше читалка выглядела чёрной, потому что инструменты рендерили код AA-плоскостей как BW-картинку |
| `0026-esp32c3-deep-sleep-crc-and-wake.patch` | **Q8, фикс холодной паники:** SYSTEM-блок C3 получает `SYSTEM_RTC_FASTMEM_CONFIG`/`RTC_FASTMEM_CRC` (запрос CRC по RTC fast memory завершается сразу — выставляется `RTC_MEM_CRC_FINISH`, который опрашивает `rtc_deep_sleep_start()`), а запись `RTC_CNTL_STATE0.SLEEP_EN` моделируется как реальное пробуждение: сброс машины с причиной `ESP32C3_DEEPSLEEP_RESET` (`rst:0x5 (DSLEEP)`). Без этого глубокий сон (а прошивка уходит в него на холодной загрузке, когда wake-gate отклоняет незажатый Power) навсегда зависал в `rtc_sleep.c:302`, ловил interrupt watchdog (`Guru Meditation … Interrupt wdt timeout on CPU0`, `rst:0xc`) и перезагружал чип |

Патчи 0005–0007 — этап Q1 (АЦП-кнопки и GPIO-прерывания), их детали в разделах ниже.
Патчи 0008–0012 — этап Q2 (NVS, GP-SPI2, панель, SD, I2C-фингерпринт).
Патчи 0013–0016 — этап Q3 (I2C-фингерпринт доведён до X3, SD-инициализация, первый кадр);
патчи 0017–0022 — этап Q4 (BUSY/IRQ, кадр в PNG, SD с FAT, трасса газ-гейджа, кнопки);
патчи 0023–0024 — этап Q6 (запись на microSD, R1/CMD13 в SPI);
патч 0025 — этап Q7 (скомпонованный экран панели и 4-градационный кадр читалки);
патч 0026 — этап Q8 (CRC RTC fast memory и пробуждение из deep sleep);
подробности — в разделах «Q3», «Q4», «Q6», «Q7» и «Q8».

Набор 0001..0026 применяется по порядку с нуля (`patch -p0` из корня дерева). Проверено
19.11.2026 на чистой распаковке тарбола: все 26 патчей — `patch` без ошибок, затронутые файлы
после применения совпадают md5 с рабочим деревом, а обратный откат 0025..0017 возвращает
файлы Q4/Q5/Q6/Q7 к pre-Q4 состоянию (см. «Q7», блок проверки round-trip).


Про пару `0006`/`0009` (оба правят `hw/gpio/esp32_gpio.c`): исходные версии были
**взаимозависимы** — `0006` вставлял код после строки `esp32_gpio_sync_outputs(s);`,
которую добавляет `0009`, а `0009` — после `esp32_gpio_refresh_levels(s, 0);` из `0006`,
поэтому ни один порядок не применялся (по одному проваленному ханку в каждом). Патчи
пересобраны 18.11.2026 из фактического дерева: `0006` теперь содержит только прерывания
(`s0 → s1`), `0009` — только выходные линии (`s1 → final`), где `s0` = чистое дерево +
`0003`. Никакого «пустого 0006» не нужно.

После патчей прошивка проходит ROM → bootloader → `entry 0x403cbf10` → NVS и **не падает
в Interrupt WDT**: `analogRead()` (АЦП-лестница кнопок) получает валидный ответ. Дальше прошивка
уходит в обмен по GP-SPI2 (SPI2, `0x60024000`) — панель/SD, см. «Q2» и «Q3».

## GPIO-матрица и кнопки (Q1, проверено 18.11.2026)

`hw/gpio/esp32_gpio.c` (субтип C3 — `hw/gpio/esp32c3_gpio.c`) теперь ведёт себя как
GPIO-периферия C3 на `0x60004000`:

- `PINx_CNF` `0x74+4·pin`, `FUNC0_IN_SEL_CFG` `0x154+4·pin`, `FUNC0_OUT_SEL_CFG`
  `0x554+4·pin`, `GPIO_STATUS`/`W1TS`/`W1TC` `0x44/0x48/0x4c` — читаются и пишутся;
- `GPIO_IN` (`0x3c`) = `(OUT & ENABLE) | (инжектированные уровни) | (pull-up на пинах без
  внешнего драйвера)`;
- `qdev_init_gpio_in` даёт по одной входной линии на пин (у C3 22 пина) — внешний девайс
  дёргает их через `qdev_get_gpio_in()`; уровни хранятся в модели вместе с маской
  «пин кем-то ведётся»;
- IO_MUX `0x60009000` (pin×4, бит 8 — pull-up) примаплен в машине как регион 1 контроллера.

Дебаг-девайс кнопок (`hw/gpio/xteink_buttons.c`, `-device xteink-buttons`):

```bash
# терминал 1: QEMU + сокет для кнопок
export LD_LIBRARY_PATH=$PWD/.venv/qemu/lib
timeout 30 .venv/qemu/src/build/qemu-system-riscv32 -nographic -machine esp32c3 \
  -drive file=$PWD/build/qemu/flash.bin,if=mtd,format=raw \
  -chardev socket,id=keys,host=127.0.0.1,port=4545,server=on,wait=off \
  -device xteink-buttons,chardev=keys,power-gpio=3

# терминал 2: нажать/отпустить (символы можно писать пачкой)
exec 3<>/dev/tcp/127.0.0.1/4545
printf 'p' >&3    # power нажата  → линия GPIO3 в 0
printf 'P' >&3    # power отпущена → линия GPIO3 в 1
printf '-' >&3    # отпустить всё
```

Символы: `b c l r u d p` — back/confirm/left/right/up/down/power (нажатие, active-low);
заглавные `B C L R U D P` — отпускание; `-` — отпустить всё. Пины задаются свойствами
`back-gpio,confirm-gpio,left-gpio,right-gpio,up-gpio,down-gpio,power-gpio`
(по умолчанию `0xffffffff` = пин не задан, линия не трогается). Без `-device
xteink-buttons` поведение прежнее — прошивка грузится ровно как до Q1.

Проверка модели без гостя (QMP, физические адреса):

```bash
# при запуске добавить -qmp unix:/tmp/q1.sock,server=on,wait=off
# затем в QMP: human-monitor-command command-line="xp /1wx 0x6000403c"
```

Наблюдаемые значения: `GPIO_IN=0x0027e018` (GPIO3=HIGH) → после `p` `0x0027e010`
(GPIO3=LOW) → после `P` снова `0x0027e018`. `FUNC_OUT_SEL[0]`, `FUNC_IN_SEL[0]` и
IO_MUX GPIO3 читаются как записанные гостём (`0x00000200`, `0x00000030`, `0x00001202`).

## SAR ADC: почему прошивка падала в WDT (проверено 18.11.2026)

После Q1 загрузка вставала в `Interrupt wdt timeout` — не из-за панели/SD, а из-за АЦП.
Факты (по `framework-arduinoespressif32-libs/esp32c3`, `APB_SARADC`):

- прошивка на boot читает `analogRead(GPIO1)`/`analogRead(GPIO2)` (АЦП-лестница кнопок,
  `InputManager`, `ADC_NO_BUTTON = 3900`); `adc_oneshot` делает так: очистка `INT_CLR`
  (`0x4C`, бит 31), `ONETIME_SAMPLE` (`0x20`) с `ONETIME_START` (бит 29), затем spin на
  `INT_RAW` (`0x44`) в ожидании `ADC1_DONE` (бит 31) и чтение `1_DATA_STATUS` (`0x2C`, [12:0]).
  В QEMU регистра не было — spin навсегда, interrupt watchdog.
- Модель `hw/misc/esp32c3_saradc.c` (тип `misc.esp32c3.saradc`, регион `0x60040000`):
  хранит регистровый файл, по записи `ONETIME_SAMPLE` с `ONETIME_START` (и `CTRL.START`)
  сразу завершает конверсию, ставит `ADC1_DONE|ADC2_DONE`, кладёт значение `idle-raw`
  (по умолчанию 4095 — «ничего не нажато») в `1_DATA_STATUS`/`2_DATA_STATUS`, `INT_CLR`
  сбрасывает флаги, IRQ заведён в intmatrix (`ETS_APB_ADC_INTR_SOURCE`).

Проверка: после патча лог за 15 с — `entry 0x403cbf10`, один `nvs_open failed` и **ни одного
Guru Meditation/WDT/reboot**; прошивка продолжает работать и уходит в SPI2-обмен.
Внутри цикла обмена (по замеру QMP): `SPI2_CMD` `0x60024000`, `SPI2_MOSI_DLEN` `0x6002401c`,
`SPI2_W0` `0x60024098`; CS панели (GPIO21) переключается ~20 раз/с, CS SD (GPIO12) — только
на старте инициализации SD.

Инъекция кнопок через АЦП (патч 0007): у `xteink-buttons` есть `<key>-adc` (канал ADC1, канал
равен номеру GPIO) и `<key>-adc-raw` (значение при нажатии; дефолты — усреднённые значения
X3: back 3512, confirm 2694, left 1493, right 5, up 2242, down 5). Например:

```bash
-device xteink-buttons,chardev=keys,power-gpio=3,\
back-adc=1,confirm-adc=1,left-adc=1,right-adc=1,up-adc=2,down-adc=2
```

При отпускании канал возвращается к `idle-raw` (4095); одновременно задавать `-gpio` и `-adc`
для одной кнопки нельзя (ошибка конфигурации).

## Q2: NVS, GP-SPI2, панель UC8253, SD (проверено 18.11.2026)

### Как запускать

```bash
# 1. сборка QEMU из исходников (пути окружения — как в начале этого файла)
cd .venv/qemu/src && export PATH=$PWD/../../../.venv/bin:$PATH \
  && export PKG_CONFIG_PATH=$PWD/../slirp/pkgconfig \
  && ninja -C build qemu-system-riscv32

# 2. прошивка (отдельный build-каталог: .pio/build общий с другими агентами)
PLATFORMIO_BUILD_DIR=build/pio-q2 .venv/bin/pio run -e default
mkdir -p build/qemu/fw && cp build/pio-q2/default/{firmware.bin,firmware.elf,bootloader.bin,partitions.bin} build/qemu/fw/

# 3. образ + прогон (20 с, лог build/qemu/boot.log)
tools/qemu/boot.sh 20
```

`tools/qemu/boot.sh` собирает 16 МБ merged-образ `build/qemu/flash.bin` из
`bootloader.bin` + `partitions.bin` + `boot_app0.bin` + `firmware.bin`, собирает FAT-образ
`build/qemu/sd.img` (см. «Q4», `make-sd-image.py`) и запускает QEMU с
`LD_LIBRARY_PATH=.venv/qemu/lib` и `timeout`. Артефакты прошивки берутся из
`build/qemu/fw/`, если они там есть (`.pio/build` в общем дереве регулярно перезаписывается),
иначе из `.pio/build/default`.

С Q4 скрипт выводит три файла: `build/qemu/boot.log` (трассы моделей),
`build/qemu/uart.log` (UART гостя: ROM-баннер, логи IDF/Arduino) и `build/qemu/frame.bin`
(дамп кадров панели). UART выведен в отдельный chardev намеренно: `-nographic` мультиплексирует
его на stdin/stdout, и в скриптовом прогоне без tty stdin закрывается сразу, а вместе с ним
теряется весь лог гостя (так было в Q1–Q3).


### Блок 1 — NVS: раздел в порядке, `NOT_FOUND` — это отсутствующий namespace

Что проверено:

- таблица разделов в образе (`0x8000`): `nvs` @ `0x9000`, размер `0x5000`; дальше `otadata`,
  `app0`/`app1`, `spiffs`, `coredump`. Адрес и размер совпадают с `partitions.csv`;
- NVS-регион в образе изначально весь `0xFF` (blank), и **прошивка форматирует его сама**:
  после прогона по смещению `0x9000` лежит валидная страница
  `fe ff ff ff | 00 00 00 00 | fe ff ff ff ... 84 2d ba b9`
  (state `0xFFFFFFFE` = ACTIVE, seq 0, version `0xFE`, CRC). Значит таблица разделов
  читается, а erase/program флеша через модель SPI-NOR работает;
- оставшиеся `[E][Preferences.cpp:47] begin(): nvs_open failed: NOT_FOUND` (**2 шт. за
  прогон**, было 4) приходят из `src/main.cpp:260` — `readQuickWakeMarker()` открывает
  namespace `cpwake` **read-only** (`begin(name, true)`), а он на свежем NVS ещё не создан.
  `nvs_open(..., NVS_READONLY)` для несуществующего namespace возвращает ровно
  `ESP_ERR_NVS_NOT_FOUND`, и прошивка это штатно обрабатывает (`return MarkerMode::None`).
  Это не «нет раздела» и не ошибка эмуляции.

Предзаполнять NVS вручную не нужно и не следует: страницу создаёт сама IDF. Если нужен
совсем чистый лог, namespace должен создать тот, кто в него пишет (первый `put*` при
включённом quick-resume).

### Блок 2 — панель UC8253 на GP-SPI2

Что появилось:

- `hw/ssi/esp32c3_gpspi.c` (`ssi.esp32c3.gpspi`, регион `0x60024000`). Регистровая карта
  взята из `soc/esp32c3/register/soc/spi_reg.h` и **отличается** от SPI0/1 (`SPI_MEM_*`):
  `CMD` 0x00 (`USR` бит 24, `UPDATE` бит 23), `ADDR` 0x04, `CTRL` 0x08, `CLOCK` 0x0c,
  `USER` 0x10, `USER1` 0x14, `USER2` 0x18, `MS_DLEN` 0x1c (MOSI dbitlen [9:0],
  MISO [19:10]), `W0..W15` **0x98..0xd4**. По записи `CMD.USR` модель сразу выполняет
  транзакцию (command → address → dummy → data) на SSI-шине и снимает `USR`, поэтому
  `while (spi->dev->cmd.usr);` в `esp32-hal-spi.c` завершается; `TRANS_DONE` выставляется
  в `DMA_INT_RAW`.
- `hw/display/xteink_uc8253.c` (`xteink.uc8253`) — SSI-периферия с `cs_polarity =
  SSI_CS_LOW`, входами `dc`/`rst` и выходом `busy`. Команда (DC=0) защёлкивается, данные
  считаются; на `0x04` (PON), `0x12` (DRF), `0x02` (POF), `0x07` (DSL) BUSY уходит в LOW и
  таймером (5/20/3/1 мс) возвращается в HIGH. Это принципиально: `BusyPolarity::X3TwoPhase`
  сначала ждёт LOW, потом HIGH, и на «плавающей» линии висит до 30 с — раньше это и давало
  WDT. Лог команд включается свойством `trace-commands` (по умолчанию on).
- `hw/gpio/esp32_gpio.c` — выходные линии: `qdev_init_gpio_out` на 40 пинов, уровень
  `(gpio_out & gpio_enable)`; так CS/DC/RST приходят в панель из GPIO-матрицы, а не из
  аппаратных CS контроллера (прошивка использует software CS).
- `hw/riscv/esp32c3.c` — `spi2` в состоянии машины, `object_initialize_child("spi2")`,
  функция `esp32c3_init_xteink_board()`: маппинг SPI2, панель и карта на SSI-шине,
  `GPIO21→panel CS`, `GPIO4→DC`, `GPIO5→RST`, `panel BUSY→GPIO6`, `GPIO12→SD CS`.

Лог: панель принимает кадрированный поток — 24 команды и 66 байт данных за 20 с, DC
переключается корректно (`byte 0x12 dc=0`, `byte 0x80 dc=1`), **ни одного** `Guru
Meditation`/WDT/reboot.

### Блок 3 — SD по SPI

`hw/sd/ssi-sd.c` подключён второй периферией на SSI-шине SPI2 (адрес шины `cs=1`, чтобы не
конфликтовать с панелью; реальный CS — GPIO12), карта создаётся как `TYPE_SD_CARD` на его
`sd-bus`, образ — `-drive file=build/qemu/sd.img,if=sd`. Без образа карта тоже создаётся
(blank), этого достаточно для идентификации.

**Ограничение:** до SD прошивка в Q2 не доходит — см. блокер ниже; в логе нет ни одного
обращения по CS карты (GPIO12).

### Блокер Q2 → Q3: прошивка определяет плату как X4 и гоняет чужой драйвер панели

**Решено в Q3** — см. раздел «Q3» ниже. Причин было две: кэш `cphw/dev_det` в NVS
(прошивка вообще не трогала I2C) и неверная кодировка команд `READ`/`STOP` в модели
i2c v2. Ниже — исходная формулировка Q2, она сохраняется как история.

1. `HalGPIO::begin()` выбирает профиль платы через `freeink::detectXteinkIsX3()`
   (`freeink-sdk/libs/hardware/XteinkDetect/src/XteinkDetect.cpp`). Это **I2C-фингерпринт**:
   два прохода по шине, в каждом опрос трёх чипов — BQ27220 `0x55` (SoC ≤ 100 и
   2500…5000 мВ), DS3231 `0x68` (валидные BCD-секунды), QMI8658 `0x6B/0x6A` (WHO_AM_I =
   `0x05`). X3 подтверждается только если **оба** прохода дали ≥ 2 из 3.
2. Модель I2C C3 (`hw/i2c/esp32_i2c.c`) сначала не поддерживала RSTART-опкод C3 (`6`), из-за
   чего каждая связка «запись-регистра + повторный старт + чтение» падала с
   `ESP_ERR_INVALID_STATE`. Это исправлено (патч 0011), ошибки I2C из лога ушли.
3. Но **`esp32_i2c_do_transaction()` при этом ни разу не вызывается**: инструментирование
   функции в прогоне дало 0 вызовов (при том, что раньше там были видны опкоды). То есть
   драйвер `i2c-ng` не доходит до запуска команды — трансфер не стартует, фингерпринт
   видит пустую шину, `detectXteinkIsX3()` = false.
4. Следствие: `BoardConfig::selectDevice(XteinkX4)`, `einkDisplay.setDisplayX3()` не
   вызывается, и в нашу UC8253-модель прилетает инициализация SSD1677/UC8179:
   `0x12 → 0x18 → 0x0c → 0x01 → 0x3c → 0x11 → 0x44 …` (у UC8253 первая команда —
   `0x00 PANEL_SETTING`, `Uc8253X3Driver::initController`). Панель эти байты принимает, но
   это не её протокол, поэтому полноценной инициализации UC8253 в Q2 нет.

Следующий шаг (Q3, узкий и проверяемый): добить модель I2C C3 до реального запуска
транзакции — сверить с `i2c_ll.h` для C3 путь `CTR.TRANS_START` / `FIFO_CONF` /
`COMD*` (модель писалась по карте Xtensa ESP32), затем убедиться по логу `[X3I2C]`, что
чипы читаются, `deviceIsX3()` = true, и в панель пошёл `0x00 0x3F 0x0A 0x61 …`. После этого
оживают и SD-инициализация, и отрисовка кадров.

## Q3: I2C-фингерпринт доведён до X3, SD-инициализация и первый кадр (18.11.2026)

### Как запускать

```bash
# 1. QEMU (как в разделе «Сборка из исходников»), затем прошивка/образ — как в Q2.
# 2. Чистый прогон: NVS должен быть пустым, иначе прошивка не станет пробовать шину
#    (см. «Причина 1»). QEMU_FRESH_FLASH=1 выбрасывает build/qemu/flash.bin.
QEMU_FRESH_FLASH=1 tools/qemu/boot.sh 20

# 3. С трассами (обе выключены по умолчанию):
ESP32_I2C_TRACE=1 ESP32_GPSPI_TRACE=1 QEMU_FRESH_FLASH=1 tools/qemu/boot.sh 20
grep -E "esp32_i2c:|GPSPI2\] txn|X3I2C|UC8253" build/qemu/boot.log
```

### Причина 1 (почему в Q2 «`esp32_i2c_do_transaction()` не вызывается»): кэш X4 в NVS

`lib/hal/HalGPIO.cpp:79-113` (`detectDeviceTypeWithFingerprint()`) идёт по цепочке
«NVS-override → **NVS-кэш** → I2C-фингерпринт». Кэш — namespace `cphw`, ключ `dev_det`
(`1` = X4, `2` = X3); значение пишется только для подтверждённого результата.

В образе, оставшемся от Q2, NVS уже содержал вердикт X4 (первый прогон до фикса 0011
получил пустую шину и закэшировал X4), поэтому каждая следующая загрузка выходила из
`detectDeviceTypeWithFingerprint()` **до** `Wire.begin()`: модель I2C не получала ни одного
обращения. Проверено двумя способами:

- декод NVS-страницы `0x9000` из `build/qemu/flash.bin`: заголовок
  `feffffff00000000feffffffffff…842dbab9`, далее записи `ns=0 type=0x01 key='cphw' data=02…`
  (namespace id 2) и `ns=2 type=0x01 key='dev_det' data=01…` → **X4**;
- прогон с `ESP32_I2C_TRACE=1`: `grep -c "esp32_i2c:" build/qemu/boot.log` = **0** за 10 с.

Лечение — не код, а чистый NVS: `QEMU_FRESH_FLASH=1` (добавлено в `tools/qemu/boot.sh`).
После него прогон сразу даёт сотни строк трассы I2C (836 в замере до фикса кодировки команд),
то есть транзакции запускаются.
Успешный прогон сам себе пишет `dev_det = 2`: после финального запуска декод NVS даёт
`ns=2 type=0x01 key='dev_det' data=02…`, и следующий прогон без `QEMU_FRESH_FLASH` идёт по
кэшу, но уже правильному — X3 (в нём 462 строки I2C-трассы: прошивка в профиле X3 сама
опрашивает газ-гейдж; в кэше X4 обращений было ровно 0, потому что проба пропускалась,
а профиль X4 эти чипы не читает).

### Причина 2 (настоящий баг модели): у C3 другая кодировка команд READ/STOP

`hal/esp32c3/include/hal/i2c_ll.h` (строки 45-49) для C3:

```c
#define I2C_LL_CMD_RESTART 6
#define I2C_LL_CMD_WRITE   1
#define I2C_LL_CMD_READ    3
#define I2C_LL_CMD_STOP    2
```

У Xtensa ESP32 (`i2c v1`) `READ=2`, `STOP=3`, то есть **READ и STOP поменяны местами**;
патч 0011 поправил только RSTART. Фактическая команда из трассы (пустой NVS, до фикса):

```
esp32_i2c: write +0x58 = 0x00003000   COMD0 RESTART(6)
esp32_i2c: write +0x5c = 0x00000902   COMD1 WRITE 2 байта (адрес+регистр)
esp32_i2c: write +0x60 = 0x00003000   COMD2 RESTART(6)
esp32_i2c: write +0x64 = 0x00000901   COMD3 WRITE 1 байт (адрес|R)
esp32_i2c: write +0x68 = 0x00001801   COMD4 READ 1  ← модель читала как STOP
esp32_i2c: write +0x6c = 0x00001c01   COMD5 READ 1 (ack_val=1)
esp32_i2c: write +0x70 = 0x00001000   COMD6 STOP(2) ← модель читала как READ 0
```

Модель обрывала транзакцию на COMD4: `TRANS_COMPLETE` выставлен, RX FIFO пуст,
`qemu-system-riscv32: esp32_i2c: read I2C FIFO while it is empty`, `Wire.requestFrom()`
возвращал 0 → все три пробы дали 0 → X4.

Фикс — патч 0013: свойство `riscv-cmd-encoding` (`DEFINE_PROP_BOOL`) переключает карту
команд, машина C3 его включает (`qdev_prop_set_bit(... "riscv-cmd-encoding", true)`).
Кодировка выбрана свойством, а не `#ifdef TARGET_RISCV32`, потому что в форке одна и та же
модель собирается и для Xtensa-таргетов.

### Что теперь в логе (цитаты, 20 с, `QEMU_FRESH_FLASH=1`)

```
qemu-system-riscv32: info: [X3I2C] kind=1 reg=0x00 -> 0x00     # DS3231 0x68: BCD-секунды
qemu-system-riscv32: info: [X3I2C] kind=2 reg=0x00 -> 0x05     # QMI8658 0x6B: WHO_AM_I
qemu-system-riscv32: info: [X3I2C] kind=1 reg=0x00 -> 0x00     # второй проход
qemu-system-riscv32: info: [X3I2C] kind=2 reg=0x00 -> 0x05
qemu-system-riscv32: info: [UC8253] byte 0x00 dc=0 cs=0
qemu-system-riscv32: info: [UC8253] cmd 0x00 (#1)              # PANEL_SETTING — X3-профиль
qemu-system-riscv32: info: [UC8253] cmd 0x61 (#2)              # RESOLUTION (0x03 0x18 0x02 0x58)
qemu-system-riscv32: info: [UC8253] cmd 0x65 (#3)              # GATE_SOURCE_START
...
qemu-system-riscv32: info: [UC8253] cmd 0x04 (#23)             # PON
qemu-system-riscv32: info: [UC8253] cmd 0x12 (#24)             # DRF: первый кадр
```

Было в Q2 (X4-профиль): первой командой шло `cmd 0x12` (SSD1677 SOFT_RESET). Два из трёх
чипов ответили в каждом проходе → `detectXteinkVerdict()` = X3Confirmed → `deviceIsX3()` =
true, `setDisplayX3()` вызван, панель получила протокол UC8253 (`0x00 → 0x61 → 0x65 …`).

### Два бага GP-SPI2/SD, найденных по ходу (0014, 0015)

- **`SPI_MS_DLEN` у C3 — одно поле** `SPI_MS_DATA_BITLEN [17:0]` на обе линии
  (`soc/esp32c3/register/soc/spi_reg.h`), а не `MOSI [9:0]` / `MISO [19:10]` как у ESP32.
  С картой Xtensa модель на каждый байт читала MISO **1 битом**: R1-ответ SD приходил как
  0x00, SdFat бесконечно повторял CMD0 (`head=40 00 00 00` … `95` в трассе GP-SPI2).
- **Карта создавалась как `sd-card` (нативный протокол)**, а `ssi-sd` говорит по SPI: только
  SPI-таблица команд (`sd_proto_spi`) отвечает на CMD0 настоящим `R1 = idle`, нативная
  возвращает «нет ответа», `ssi-sd` превращает это в `R1 = 0x04` (illegal command).
  Фикс: `qdev_new(TYPE_SD_CARD_SPI)`.

### До чего дошла прошивка (трасса `ssi-sd` включалась временно, в патчи не вошла)

- **SD-инициализация наступает**: `SSISD cmd=0 arg=0x00000000 ret=4`, затем
  `cmd=8 arg=0x000001aa`, `cmd=55`, `cmd=41 arg=0x40000000` (дважды), `cmd=58` — все с
  полноценным R1 (`ret=4`), то есть карта отвечает. Дальше чтения сектора 0 нет: образ
  `build/qemu/sd.img` — пустой (64 МиБ нулей), файловой системы на нём нет.
- **Первый кадр залит в панель**: plane-команды `0x10`/`0x13` с данными, затем `0x04` (PON)
  и `0x12` (DRF). После `0x12` новых SPI-транзакций и команд панели нет — гость уходит в
  idle: снятый через QMP `info registers` даёт `pc=0x4038a048 restore_stack_pointer`,
  `mepc=0x403857ac esp_cpu_wait_for_intr`, `mstatus.MIE=0`; паники и WDT в чистых прогонах
  нет (единственный `Interrupt wdt timeout` в заметках получен при двух одновременных
  QEMU-прогонах на одном `boot.log` — артефакт эксперимента, не прошивки).

### Что осталось после Q3

1. **BUSY/готовность обновления панели.** После DRF гость стоит в ожидании; нужно понять,
   доходит ли `waitRefreshComplete()` до конца (ISR-путь на `attachInterrupt` GPIO6 или
   30-секундный таймаут `waitBusy`) и обновляется ли кадр. Полезно снять PC несколько раз
   через QMP и/или временно отключить ISR-путь (`_busyWaitSliceHook`).
2. **Кадр в PNG.** Модель панели держит RAM-плоскости, но конвертера в PNG/chardev нет;
   после п.1 это следующий очевидный шаг (52 272 байта на плоскость, 1 бит/пиксель).
3. **`[X3I2C]` для BQ27220 (0x55).** Трассировка стоит только в индексированной ветке
   `x3_i2c_recv()`; `wide`-ветка (газ-гейдж) логируется молча, поэтому в логе видны только
   DS3231 и QMI8658. Чип отвечает (SoC 87 %, 4020 мВ) — нужно перенести лог выше `if (wide)`.
4. **Кнопки** после загрузки не проверены: прогон с `-device xteink-buttons` и инъекцией
   `l`/`L` шёл одновременно с другим QEMU и в `boot.log` попали оба, тест недействителен.
5. **SD с файловой системой.** Нужен образ с FAT (например, `mkfs.vfat` + каталог), чтобы
   проверить `Storage.begin()`/индекс книг, а не только идентификацию карты.

Все пять пунктов закрыты в Q4 (см. следующий раздел) — кроме «индекс книг»: каталог
читается, но до построения индекса прошивка в прогоне не доходит.

## Q4: BUSY/IRQ, кадр панели в PNG, SD с FAT, кнопки (проверено 18.11.2026)

Итог этапа одной строкой: **прошивка больше не виснет после `0x12`.** Причина была не в
панели и не в прошивке, а в GPIO-модели: у неё не было регистра, который читает ISR из
ESP-IDF. После фикса прошивка проходит полный boot-цикл панели (несколько refresh'ей,
`POF`, уход в сон), кадры панели снимаются в файл и конвертируются в PNG, карта SD
монтируется как FAT16, значение кнопки из инъекции видно в `analogRead()`.

### Как запускать

```bash
# сборка QEMU (как выше) + один прогон, UART гостя и кадры — рядом
cd .venv/qemu/src && export PATH=$PWD/../../../.venv/bin:$PATH \
  && export PKG_CONFIG_PATH=$PWD/../slirp/pkgconfig && ninja -C build qemu-system-riscv32
cd -

QEMU_FRESH_FLASH=1 tools/qemu/boot.sh 20      # чистый NVS -> проба I2C -> X3
tools/qemu/panel-frame.py --input build/qemu/frame.bin --ascii   # PNG + PGM + статистика
```

Вспомогательные инструменты (все в `tools/qemu/`):

| Файл | Что делает |
|---|---|
| `boot.sh` | merged-образ, FAT-карта, прогон QEMU; пишет `build/qemu/boot.log` (трассы моделей), `build/qemu/uart.log` (UART гостя) и `build/qemu/frame.bin`. Env: `QEMU_FRESH_FLASH`, `QEMU_FRESH_SD`, `QEMU_QMP` |
| `make-sd-image.py` | reproducible 64 МиБ FAT16-образ (MBR + 2 копии FAT + LFN): `/books/test.txt`, `/.crosspoint/`, `--add host:/books/name` |
| `sd-files.py` | чтение готового образа с хоста (Q6): разбор MBR → BPB → FAT → каталогов, дерево/`--json`/`--cat`/`--extract`, `--deleted` для удалённых записей. Независимая проверка, что запись гостя легла в файл |
| `panel-frame.py` | записи `frame.bin` → PNG + PGM + статистика + ASCII-превью; по умолчанию (`--plane screen`) — 4 оттенка, 8 бит/пиксель (Q7), `--plane dtm1\|dtm2` — сырая RAM-плоскость, 1 бит/пиксель; `--all`, `--frame N` |
| `qmp-probe.py` | снять `info registers` через QMP, разобрать стек и раскодировать адреса через `riscv32-esp-elf-addr2line` (кто в этот момент на CPU) |
| `verify-patches.sh` | круговой прогон набора патчей: чистая распаковка → `patch -p0` 0001..0022 → md5 с рабочим деревом → `patch -R` 0022..0017 → md5 с pre-Q4 деревом |

### Блок 1 — почему после DRF ничего не происходило: `GPIO_PCPU_INT` (патч 0017)

Диагностика шла через QMP (`-qmp unix:…` + `qmp-probe.py`). В состоянии «после DRF
ничего не происходит» регистры гостя выглядели так:

```
[sample 0] pc=0x4038a048 mstatus=0x00001880 mip=0x00000800 mepc=0x403857ac mcause=0x80000009
    0x4038a048 restore_stack_pointer (portasm.S:704) <- pc
    0x403857ac esp_cpu_wait_for_intr (cpu.c:64) <- mepc
    0x40380290 _interrupt_handler (vectors.S:507)
    0x40380ed2 gpio_intr_service (gpio.c:505)
```

`mstatus.MIE=0` + `mcause=0x80000009` (machine external) + `mip.MEIP=1` — CPU не в idle,
а **внутри обработчика внешнего прерывания, которое не снимается**. Дальше физические
регистры: `GPIO_STATUS 0x60004044 = 0x40` (бит 6 = BUSY панели), `GPIO_IN 0x6000403c =
0x0037b063` (линия BUSY = 1, т.е. панель уже не занята), `PIN6_CNF 0x6000408c = 0x2080`
(`INT_ENA=1`, `INT_TYPE=1` = POSEDGE). В матрице прерываний висящим был ровно один источник:
`INTMTX IO_STATUS 0x600C20F8 = 0x00010000` = бит 16 = `ETS_GPIO_INTR_SOURCE`.

Причина — в дизассемблере прошивки (`gpio_intr_service`, `0x40380ed2`):

```
lw a5,0(a5)      # a5 = GPIO base 0x60004000
lw s1,92(a5)     # 0x5c = GPIO_PCPU_INT
and s1,s1,s3     # маска пинов
beqz s1, +0xa0   # пусто -> выход БЕЗ записи в STATUS_W1TC
...
lw a5,76(a4)     # 0x4c = GPIO_STATUS_W1TC, сброс бита
```

ISR берёт pending-набор из `GPIO_PCPU_INT` (`0x5c`), а не из `STATUS`. В модели этого
регистра не было — чтение возвращало 0, ISR выходил, `STATUS` оставался взведённым,
level-IRQ в intmatrix оставался высоким, и CPU бесконечно перезаходил в обработчик.
Первое такое прерывание — фронт BUSY после `0x12` (в `displayFinish()`), поэтому Q3 и
видел «idle после DRF»: гость был не в idle, а в шторме прерываний.

Патч 0017 добавляет `pcpu_int_off` и читает `0x5c` как `status & INT_ENA`. После него
`mip` в выборках равен 0, PC — `esp_cpu_wait_for_intr` с `MIE=1` (обычный idle FreeRTOS),
и лог панели идёт дальше DRF:

```
[UC8253] cmd 0x12 (#24, 0 data bytes since last cmd)
[UC8253] BUSY -> 0 (DRF, 300 ms) at 2164 ms
[UC8253] BUSY -> 1 (idle) at 2464 ms
[UC8253] cmd 0x10 (#25, 0 data bytes since last cmd)      # пост-синк DTM1, дальше settle-refresh
...
[UC8253] cmd 0x04 (#59, 0 data bytes since last cmd)      # PON перед settle
[UC8253] cmd 0x12 (#60, 0 data bytes since last cmd)
[UC8253] cmd 0x07 (#74, 0 data bytes since last cmd)      # DEEP_SLEEP: sleep-screen отрисован
```

Побочно выяснилось, что `waitRefreshComplete()` в этой прошивке идёт ISR-путём
(`setBusyWaitSliceHook` не установлен), т.е. без 0017 он не мог завершиться вообще.

Тайминги BUSY в модели (патч 0018) подняты до правдоподобных (PON 50, DRF 300, POF 30,
DSL 5 мс, переопределяются `XTEINK_PANEL_BUSY_*_MS`): на 5/20 мс фронт легко проскакивал
между опросами `delay(1)` и wait завершался по таймауту, а не по прерыванию.

### Блок 2 — кадр панели в PNG (патч 0018 + 0019)

Модель панели теперь хранит оба RAM-плоскости (`DTM1 0x10`, `DTM2 0x13`, по 99×528 =
52 272 байта) и на каждую `0x12` пишет запись в chardev:

```
u32 magic 'X3PN' | version | width=792 | height=528 | stride=99 | planes=2
u32 flags (бит0: строки в порядке гейтов, снизу вверх) | u32 индекс кадра
далее plane DTM1, plane DTM2 — как их приняла панель (драйвер шлёт кадр снизу вверх,
см. EpdBus::sendPlaneFlipped), 1 бит/пиксель, MSB first, 1 = белый
```

Chardev подключается свойством машины (патч 0019): `-machine esp32c3,xteink-panel=<id>`;
`-global` тут не работает — QEMU режет `-global` по первой точке, а тип называется
`xteink.uc8253`. `boot.sh` делает это сам и кладёт поток в `build/qemu/frame.bin`.

Проверено на прогоне с `QEMU_FRESH_FLASH=1`: 5 записей, статистика последней (`panel-frame.py`):

```
build/qemu/frame.bin: 5 frame record(s), last index 4, 792x528 stride 99, planes 2, rows bottom-up
[record 4] index=4 plane=DTM2
  DTM2: 52272 bytes for 792x528 = 418176 pixels
    black 408403 (97.663%), white 9773
    distinct byte values: 46, rows with non-uniform content: 112
```

Разные кадры boot-последовательности (одинаковые записи = повторный refresh того же
содержимого):

```
frame #0: md5=b12c8c5b            # splash (логотип Inkwell + CROSSPOINT_VERSION)
frame #1: md5=b12c8c5b diff_vs_prev=0
frame #2: md5=b12c8c5b diff_vs_prev=0
frame #3: md5=50c10500 diff_vs_prev=32756 px
frame #4: md5=50c10500 diff_vs_prev=0
```

Кадр не пустой по определению статистики: 878 чёрных пикселей у splash (0.210 % — логотип
и три строки текста), 7 277 (1.740 %) у следующего экрана, 408 403 (97.663 %) у sleep-screen.
Артефакты: `build/qemu/frame.png` (PNG 1 бит), `build/qemu/frame.pgm`, `build/qemu/frame-ascii.txt`
(ASCII-превью для проверки без просмотрщика).

Ограничение: запись плоскости идёт строго последовательно, поэтому partial-window
(`0x91`/`0x90`) как окно не учитывается — данные DTM ложатся в начало плоскости. Для
полнокадровых записей (все пути этой прошивки) это верно; для точного воспроизведения
частичных обновлений понадобится учитывать окно.

С Q7 запись кадра несёт ещё и скомпонованный экран (`planes=4`), а 1-битные плоскости
DTM1/DTM2 остаются как были — см. «Q7».

### Блок 3 — SD с FAT16 (патч 0021)

Образ собирает `tools/qemu/make-sd-image.py` (без `mkfs.vfat`/`mtools`, reproducible):
MBR с одной активной партицией @2048, FAT16 с кластерами 2 КиБ (32 184 кластера, 126
секторов на копию FAT, две копии), `/books/test.txt` и `/.crosspoint/` (с LFN-записями,
чтобы имена были именно в нижнем регистре). `boot.sh` пересобирает образ, если его нет
(`QEMU_FRESH_SD=1` — принудительно).

Одного образа мало: SdFat останавливался сразу после CMD58. Причина — `ssi-sd` собирал
R1 для CMD8/CMD58 жёстко как `1` («карта в idle»), хотя после ACMD41 карта уже вышла из
idle. Лог до фикса (R1 после ACMD41 = 0x00, а CMD58 всё равно 0x01 → обрыв инициализации):

```
ssi_sd: [831 ms] cmd=41 arg=0x40000000 rlen=4
ssi_sd: [831 ms] r1 cmd=41 = 0x00
ssi_sd: [831 ms] cmd=58 arg=0x00000000 rlen=4
ssi_sd: [831 ms] r1 cmd=58 = 0x01          # ← SdFat: карта всё ещё idle -> отказ
```

После фикса (`card_idle` отслеживается по CMD0/CMD1/ACMD41) инициализация идёт дальше в
файловую систему:

```
ssi_sd: [830 ms] cmd=58 arg=0x00000000 rlen=4
ssi_sd: [830 ms] r1 cmd=58 = 0x00
ssi_sd: [831 ms] cmd=18 arg=0x00000000 rlen=4     # MBR
ssi_sd: [833 ms] read done cmd=18 bytes=512
ssi_sd: [834 ms] cmd=18 arg=0x00100000 rlen=4     # boot sector партиции (LBA 2048)
ssi_sd: [835 ms] read done cmd=18 bytes=512
ssi_sd: [836 ms] cmd=18 arg=0x0011fa00 rlen=4     # корневой каталог (LBA 2301)
ssi_sd: [838 ms] cmd=18 arg=0x00124a00 rlen=4     # кластер /.crosspoint
...
```

Всего 114 строк трассы за прогон: чтение MBR, BPB, корневого каталога и каталога
`/.crosspoint` — это и есть доказательство, что `Storage.begin()` смонтировал том. Записей
(CMD24/25) нет: прошивка в прогоне каталог только читает. До построения индекса библиотеки
дело не доходит — после монтирования прошивка уходит в сон (см. «Что осталось»).

### Блок 4 — `[X3I2C]` для газ-гейджа BQ27220 (патч 0020)

Трасса перенесена за обе ветки `x3_i2c_recv()`, поэтому `wide`-чип `0x55` тоже логируется:

```
[X3I2C] kind=0 wide=1 reg=0x2c -> 0x57      # StateOfCharge = 87 %
[X3I2C] kind=0 wide=1 reg=0x2c -> 0x00
[X3I2C] kind=0 wide=1 reg=0x08 -> 0xb4      # Voltage = 0x0FB4 = 4020 мВ
[X3I2C] kind=0 wide=1 reg=0x08 -> 0x0f
[X3I2C] kind=1 wide=0 reg=0x01 -> 0x00      # DS3231
[X3I2C] kind=2 wide=0 reg=0x01 -> 0x05      # QMI8658 WHO_AM_I
```

### Блок 5 — кнопки после загрузки (патч 0022 + `xteink-buttons`)

Инъекция идёт через `-device xteink-buttons` (АЦП-лестница: back/confirm/left/right — канал
ADC1 = GPIO1, up/down — GPIO2, power — линия GPIO3). Патч 0022 добавляет трассу конверсий
АЦП, которая печатает только «не-idle» значения — это прямое доказательство, что нажатие
доходит до прошивки, а не только до chardev:

```
[X3I2C] kind=0 wide=1 reg=0x2c -> 0x57
[SARADC] ch=1 raw=1493 (1130 mV approx) — ladder not idle
[SARADC] ch=1 raw=1493 (1130 mV approx) — ladder not idle
...                                                       # держим 'l' (left), 400 строк
```

1493 — это ровно «left» из таблицы X3 (back 3512, confirm 2694, left 1493, right 5,
up 2242, down 5), т.е. `analogRead(BUTTON_ADC_PIN_1)` в прошивке видит нажатие.

Реакция UI видна по кадрам: в прогоне с инъекцией `r`/`d`/`c`/`b` панель получила
дополнительные refresh'и (`frame #4..#7`) с реально другим содержимым, а экран
переключается между двумя состояниями и возвращается назад — то есть события кнопок
обрабатываются активностью:

```
frame #3: md5=7a9b578a diff_vs_prev=16945 px
frame #4: md5=aef0b49e diff_vs_prev=12129 px
frame #5: md5=7a9b578a diff_vs_prev=12129 px      # back вернул предыдущий экран
```

Оговорка (честно): задержка между отправкой символа и refresh'ем в замерах 1–5 с, а
причина не локализована — вероятнее всего, это шаг опроса/рендера в простое (light sleep),
а не потеря события. Детерминированную задержку < 1 с пока подтвердить не удалось;
для проверки UI-логики этого достаточно, для «кнопка → отклик за N мс» — нет.

### Проверки Q4 (команды и результаты)

- `ninja -C build qemu-system-riscv32` — exit 0 (пересобрано 8 раз по ходу этапа).
- `QEMU_FRESH_FLASH=1 tools/qemu/boot.sh 20` — проба I2C, X3-профиль, полный boot панели,
  4–5 кадров, `uart.log` с ROM-баннером; паники в чистом прогоне нет.
- Круговая проверка патчей `tools/qemu/verify-patches.sh`: чистая распаковка тарбола →
  `patch -p0 --forward` для 0001..0022 → **0 провалов**; md5 22 затронутых файлов совпал с
  рабочим деревом; `patch -R` для 0022..0017 → **0 провалов**, 10 файлов Q4 вернулись к
  pre-Q4 md5. Вывод последнего прогона: `RESULT forward=0 md5_mismatch=0 reverse=0
  pre_q4_mismatch=0`.

## Q5: кнопки end-to-end, интерактивное окно и smoke-гейт (проверено 18.11.2026)

План: `p_7a4c50cfdf7b`. Итог: нажатие «физической» кнопки доходит до прошивки и меняет
экран, отклик измерен и объяснён, прошивкой можно управлять из окна, и появился обязательный
смоук-гейт перед выкладкой прошивки.

### Как запускать

```bash
tools/qemu/run-ui.sh                              # build/qemu/fw/firmware.bin
tools/qemu/run-ui.sh build/pio-q2/default/firmware.bin
tools/qemu/run-ui.sh --fresh-flash                # остальные аргументы уходят в x3-gui.py
tools/qemu/x3-gui.py --choose                     # открыть диалог выбора прошивки
tools/qemu/x3-gui.py --firmware p/firmware.bin --backend terminal   # без X11
tools/qemu/q5-scenarios.py                        # headless-сценарии + PNG + отчёт
tools/qemu/smoke.sh                               # смоук-гейт (обязателен перед прошивкой)
```

`run-ui.sh` копирует выбранный образ в `build/qemu/fw/firmware.bin` (вместе с `.elf`), а
`bootloader.bin`/`partitions.bin` берёт из `.pio/build/default`, если их нет; `.pio` только
читается. Копирование живёт в одном месте — `x3emu.install_firmware()`, её же зовут GUI,
сценарии и смоук.

Порядок выбора прошивки в `x3-gui.py` без аргументов: `build/qemu/fw/firmware.bin` →
`.pio/build/default/firmware.bin` → и только если нет ни того, ни другого — диалог выбора.
То есть после `pio run` окно сразу показывает прошивку: диалог — это `--choose` или клавиша `O`,
а не первый экран.

### Карта клавиш (окно = устройство)

| клавиша | кнопка X3 |
|---|---|
| ↑ / ↓ | Up / Down (боковые) |
| ← / → | Left / Right (передняя полоса) |
| Enter | Confirm |
| Esc, Backspace | Back |
| P | Power |
| O | «Выбрать прошивку…» — файловый диалог поверх экрана |
| R | перезапустить эмулятор с той же прошивкой |
| F12 | сохранить скриншот окна (`build/qemu/q5/gui-window.png`) |
| Q | выход |

Удержание клавиши = удержание кнопки: модель держит АЦП-значение (back/confirm/left/right —
канал ADC1 = GPIO1, up/down — GPIO2) или линию GPIO3 (power), пока клавиша нажата, поэтому
долгое нажатие прошивки (порог 1000 мс, `src/ButtonEventManager.h`) получается само собой.
Строка состояния показывает нажатую кнопку и время удержания (`LEFT 1.2s LONG`).

### Окно

- 528×792 (кадр 1:1) + две строки состояния сверху и строка подсказки снизу; поворот на 90°,
  потому что в RAM панели экран лежит «боком» (792×528, текст вдоль левой кромки).
- Рисуется через **X11 + ctypes** (`libX11.so.6`, `XCreateImage` глубиной 1 в формате
  XYBitmap поверх 24-битного окна). Без pip-пакетов: `tkinter` в этом окружении нет
  (`libtk8.6.so` отсутствует), `PIL` нет, `gi` есть, но это заглушка без `require_version`.
- Строка состояния: имя прошивки, кнопка и удержание, число кадров, возраст последнего кадра,
  задержка «клавиша → кадр», время с запуска, состояние QEMU.
- Диалог выбора прошивки: каталоги и `*.bin`, Enter — войти/загрузить, Esc/Backspace — наверх.
  Текст рисуется системным X-шрифтом `fixed` (ISO8859-1), поэтому не-латинские имена
  показываются как `?`.
- `--backend terminal` — ANSI-рендер кадра в терминале (полублоки, 88×44), стрелки/Enter/
  Backspace/p; запасной путь для машины без X.
- `--self-test N` — прогнать окно без клавиатуры: загрузить, нажать Back, снять скриншот, выйти.

### Откуда в Q4 взялись «1–5 с» отклика: это окно загрузки, а не задержка ввода

Все замеры — wall-clock, `tools/qemu/q5-scenarios.py`; сырые данные в
`build/qemu/q5/report.json` и `report.md`.

| что | результат |
|---|---|
| до устойчивого экрана после старта QEMU | **4.3–4.5 с** (первый кадр панели ~2.2 с) |
| короткий Back → кадр поп-ап меню | **127–133 мс** |
| короткий Back (закрыть) → кадр | **117–133 мс** |
| Down/Up в поп-ап меню → кадр | **117–138 мс** |
| короткий Confirm → кадр карточки | **132–148 мс** (два refresh'а) |
| короткий Back с карточки | **127–138 мс** |
| долгий Back (удержание 1200 мс) → фильтры | кадр на **1201 мс**, т.е. на отпускании |
| долгий Back (удержание 2500 мс) → фильтры | кадр на **2500 мс** |
| простой 25 с без нажатий | ни новых кадров, ни новых паник |

`tap` в сценариях держит кнопку 90 мс, поэтому 117–148 мс = «нажали, отпустили, кадр» —
рендер стартует через ~30 мс после отпускания. Долгое нажатие показывает результат на
отпускании: `applyTimeout()` в `ButtonEventManager` умеет выдать Long по порогу, но в этом
прогоне кадр появляется ровно на отпускании (удержание 2.5 с → кадр на 2.5 с), так что
«долгое нажатие» в GUI = подержать и отпустить.

Клавиша, нажатая **до** появления UI, теряется, а не «доходит с задержкой»: в boot-probe
нажатие Back на 1.0 с после старта не открыло меню, экран стал устойчивым на 3.4–3.8 с.
Именно это и наблюдалось в Q4 как «1–5 с»: нажатия попадали в окно, где первый boot падал в
`Interrupt wdt timeout` и чип перезагружался.

Задержки нет ни в прошивке, ни в модели: `lib/hal/HalGPIO.cpp` опрашивает АЦП отдельной
задачей каждые 10 мс и складывает фронты в очередь на 32 события (нажатие не теряется, даже
если loop-таск занят), а `xteink-buttons` меняет значение АДЦ синхронно с приходом символа.

### Ограничения модели совпадают с X3: 400 КБ SRAM без PSRAM, 16 МБ flash

Проверено на текущей модели (вывод команд — в разделе «Проверки Q5»):

- SRAM: `mc->default_ram_size = 400 * 1024`; карта памяти — DRAM `0x3fc80000` 0x60000
  (384 КБ) + SRAM0 16 КБ (`ESP32C3_INTERNAL_SRAM0_SIZE`), IRAM `0x4037c000` 0x70000, т.е.
  ровно 400 КБ, как у ESP32-C3. `info mtree` подтверждает регионы, PSRAM в машине нет
  (у C3 её и не бывает).
- Flash: образ 16 МиБ собирается `esptool merge-bin --flash_size 16MB --fill-flash-size 16MB`
  из того же `partitions.bin` (app0/app1 по 6.25 МБ, spiffs 3.375 МБ, coredump). Машина
  выбирает модель чипа по размеру образа: 16 МиБ → `is25lp128` (ISSI 16 МБ), `info qtree`
  показывает `dev: is25lp128`; JEDEC этого чипа отдаёт ёмкость 16 МБ, т.е. прошивка видит
  ровно тот чип, что на X3. Меньший образ дал бы w25x16/gd25q32/gd25q64 — тогда часть
  разделов (`spiffs`, `coredump`) не влезла бы.
- Поведение при нехватке heap настоящее: прошивка получает 400 КБ SRAM и падает/логирует
  штатными средствами IDF, модель ничего не «добавляет» (PSRAM нет, `HAL` не подменяется).

### Smoke-гейт перед прошивкой

```bash
tools/qemu/smoke.sh                      # build/qemu/fw/firmware.bin
tools/qemu/smoke.sh path/to/firmware.bin
tools/qemu/smoke.sh --strict-panic       # любая паника = FAIL
```

Один прогон с **чистыми** flash (пересобранный merged-образ, пустой NVS) и SD (заново
собранный FAT16): загрузка до отрисованной библиотеки, затем короткий Back → кадр поп-ап меню,
Back (закрыть) → Confirm → кадр карточки. Артефакты в `build/qemu/smoke/`:
`frame-library.png`, `frame-menu.png`, `frame-card.png`, `boot.log`, `uart.log`, `smoke.txt`
(итог, шаги, задержки, паники). Последняя строка — одна из:

```
SMOKE OK: frames=8 panic=0
SMOKE OK: frames=8 panic=1 (known cold-boot panic, rebooted=1; strict mode would fail)
SMOKE FAIL: <причина>
```

Гейт падает (exit 1), если библиотека не отрисовалась (меньше 500 тёмных пикселей), если
Back/Confirm не дали нового кадра или если в `uart.log` есть паника, а `--strict-panic`
включён. **Известная холодная паника** (IDWDT на первом boot: `Interrupt wdt timeout`,
`rst:0xc RTC_SW_CPU_RST`) по умолчанию считается WARN: она недетерминирована, зависит от
скорости хоста (эмуляция медленнее реального чипа, а прошивка проходит участок с замаскированными
прерываниями), после неё чип перезагружается и доходит до UI — то есть на картинку и на
кнопки не влияет. Если нужен строгий «panic=0», запускайте с `--strict-panic`; сейчас на
текущем образе он даёт FAIL именно из-за этой паники.

### Новое ограничение: запись на SD-карту (CMD24 → R1=0x04)

С `SSISD_TRACE=1` видно, что чтение работает (CMD18 читает MBR, BPB, каталоги), а запись —
нет: на `CMD24` карта отвечает `R1 = 0x04` (illegal command), SdFat повторяет попытку
десятки раз.

```
ssi_sd: [1983 ms] cmd=24 arg=0x0011fa00 rlen=4
ssi_sd: [1983 ms] r1 cmd=24 = 0x04
ssi_sd: [1983 ms] cmd=24 arg=0x0011fa00 rlen=0
ssi_sd: [1983 ms] r1 cmd=24 = 0x04          # 29 попыток за прогон
```

Следствия: прошивка не может сохранить `/crash_report.txt` в корне карты и `books.json`
индекса полки, поэтому полка остаётся с одной записью (`crash_report`), а книги из
`/books` (`test.txt`, добавленные сценарием `alpha/beta/gamma.txt`) в сетке не появляются —
перемещение по сетке нечего перерисовывать. Поэтому навигация стрелками проверена в поп-ап
меню (7 пунктов). Разбираться с SPI-записью (протокол в `hw/sd/sd.c`, токен данных и
CRC в `ssi-sd`) — отдельная задача.

**Решено в Q6** (патчи 0023 и 0024, раздел «Q6» ниже): причиной был не `sd.c`, а модель
`ssi-sd`, которая теряла DATA-токен `0xFE` на байте после R1.

### Модель не менялась: новых патчей нет

Набор остался `0001..0022`, рабочее дерево `.venv/qemu/src` в части патчей не тронуто.
Причина: задержка отклика оказалась не в модели и не в скриптах, а в окне загрузки (выше),
а единственная найденная модельная проблема — паника на первом boot — это известное
ограничение Q4 (не реализованы RTC/часы и deep sleep) и к тому же недетерминированное.
Чинить его «правильно» в этой задаче было бы хуже: если смоделировать deep sleep как reset
с причиной deep-sleep, прошивка после пробуждения снова уйдёт в сон и получится цикл
boot → sleep вместо рабочего UI; сейчас паника перезагружает чип с `RTC_SW_CPU_RST`, и
прошивка по признаку краха остаётся в UI. `src/` не тронут.

### Известные ограничения (Q5)

- первая загрузка: одна паника IDWDT и reboot, 4.3–4.5 с до UI (см. «Smoke-гейт»);
- запись на SD не работает (CMD24/25), поэтому полка не индексирует книги с карты —
  **закрыто в Q6** (патчи 0023/0024, см. раздел «Q6»);
- deep/light sleep не моделируются: если прошивка уйдёт в сон, будет та же паника;
- окно требует X11 (`libX11.so.6`) и шрифт `fixed`; иначе `--backend terminal`;
- не-латинские имена файлов в окне показываются как `?`;
- навигация по сетке не проверена из-за одноэлементной полки (см. запись на SD).

### Проверки Q5 (команды и результаты)

- `tools/qemu/q5-scenarios.py` → exit 0: `report.json`/`report.md`, PNG `home.png`,
  `menu.png`, `menu-selection.png`, `book-card.png`, `filters.png`, `filters-hold.png` в
  `build/qemu/q5/`, отклик 117–143 мс, boot-probe «клавиша на 1.0 с не открыла меню,
  экран устойчив на 3.4 с».
- `tools/qemu/smoke.sh` → exit 0: `SMOKE OK: frames=8 panic=1 (known cold-boot panic,
  rebooted=1; strict mode would fail)`, шаги 127/122/143 мс.
- `tools/qemu/smoke.sh --strict-panic` → exit 1 (`SMOKE FAIL: panic in uart.log`) — ожидаемо
  на текущем образе.
- Окно: `tools/qemu/run-ui.sh` без аргументов подняло окно `X3 QEMU — firmware`
  (528×850) и сразу показало полку с книгой — диалога на старте нет
  (`build/qemu/q5/gui-default.png`); клавиша `O` открывает диалог выбора прошивки
  (`build/qemu/q5/gui-dialog.png`); `F12` сохраняет скриншот окна
  (`build/qemu/q5/gui-window.png`).
- `x3-gui.py --self-test` → `SELFTEST: boot_to_first_frame=2.2s back_to_frame=0.121s
  frames=6`, скриншот сохранён.
- `tools/qemu/verify-patches.sh --make-pre` → `RESULT forward=0 md5_mismatch=0 reverse=0
  pre_q4_mismatch=0 pre_q4_skipped=0` (набор 0001..0022 применяется и откатывается
  чисто; скрипт сам собирает pre-Q4-эталон и убирает деревья, `--keep` их оставляет).
- Сверка ограничений: `info qtree` → `dev: is25lp128` (16 МБ flash), `info mtree` → SRAM
  `0x3fc80000-0x3fcdffff` + IRAM `0x4037c000-0x403dffff` = 400 КБ, `default_ram_size = 400*1024`.

## Q6: запись на microSD и настоящий индекс библиотеки (проверено 18.11.2026)

Итог этапа одной строкой: **карта пишется.** `CMD24`/`CMD25` принимают DATA-токен и 512 байт,
блок реально ложится в `sd.img`, прошивка сохраняет `/.crosspoint/shelf/books.json`, а метаданные
`.fb2.zip` доходят до названий — в кадре библиотеки вместо имён файлов стоят «Личный враг Бога» и
«Личный друг Бога».

### Как запускать

```bash
# карта с настоящими книгами: всё из build/qemu/sd-books/ попадает в /books
ls build/qemu/sd-books/          # lichnyy_vrag_boga.fb2.zip, lichnyy_drug_boga.fb2.zip
QEMU_FRESH_FLASH=1 QEMU_FRESH_SD=1 tools/qemu/boot.sh 40

# что реально появилось на карте (парсер FAT16, без монтирования и mtools)
tools/qemu/sd-files.py --image build/qemu/sd.img
tools/qemu/sd-files.py --cat /.crosspoint/shelf/books.json

# кадр библиотеки: build/qemu/frame.bin -> PNG
tools/qemu/panel-frame.py --input build/qemu/frame.bin
```

`build/qemu/sd-books/` в git не лежит (`build/` игнорируется): тестовые книги — пользовательский
контент, а не фикстуры. Если каталога нет, карта собирается как раньше — с `/books/test.txt`.

### Почему запись не работала: R1 приходил, а DATA-токен съедался

В Q5 было видно только следствие — `CMD24` и `R1 = 0x04`, 29 повторов. Пошаговая трасса байтов
(`SSISD_TRACE_XFER`, временная, в патчи не вошла) показала другое:

```
SSISDX mode=1 val=0x87 cmd=24   # CRC команды: sd.c принял CMD24, state = receiving-data
SSISDX mode=2 val=0xff cmd=24   # Ncr
SSISDX mode=3 val=0xff cmd=24   # ← R1 = 0x00: команда УСПЕШНА
SSISDX mode=3 val=0xfe cmd=24   # ← сюда пришёл DATA-токен 0xFE и был съеден
SSISDX mode=0 val=0x2e cmd=24   # payload разбирается как команды: CMD46 arg=0x20202020
SSISDX mode=1 val=0x20 cmd=46   #   (это пробелы из записи каталога FAT)
```

Причина в модели `ssi-sd`: после последнего байта ответа она оставалась в состоянии
`SSI_SD_RESPONSE` ещё на один байт и только следующим вызовом переходила в `CMD`. В SPI хост
начинает следующую фазу сразу после R1 — для записи этим байтом и является токен `0xFE`.
Токен уходил в никуда, дальше 512 байт данных декодировались как команды (`0x00` в потоке — это
`CMD0`, который переводит карту в idle), а каждый следующий `CMD24` в состоянии receiving-data
получал `R1 = 0x04` (illegal command) — 5712 повторов за прогон.

То есть `R1 = 0x04` из Q5 был не «прошивка не умеет писать», а «модель потеряла токен, а потом
сама себя загнала в неверное состояние».

### Что изменено в модели

**Патч 0023 (`hw/sd/ssi-sd.c`) — движок записи:**

- переход из состояния ответа выполняется на том же байте, который несёт последний байт ответа
  (`ssi_sd_response_done()`): для чтения — `PREP_DATA` (байт-в-байт как раньше, хост опрашивает
  токен), для записи — сразу `CMD`, где и ждётся токен;
- `SSI_SD_CMD` принимает `0xFE` (single) и `0xFC` (multi) **только если карта действительно в
  receiving-data** (`sdbus_receive_ready()`), иначе токен не съедает 512 чужих байт;
- 512 байт пишутся в `sdbus_write_byte()` (это `sd_write_byte()` из `sd.c` →
  `sd_blk_write()` → `blk_pwrite` в `sd.img`), байты CRC16 принимаются и пропускаются (CRC не
  проверяется — как и в upstream-модели);
- после блока отдаётся data response token `0x05`, затем **BUSY-фаза**: `SSI_SD_WRITE_BUSY`
  держит MISO в нуле 8 опрошенных байт (реалистичная «запись во флеш»); если хост не опрашивает
  занятость, а сразу шлёт команду, байт не теряется;
- `0xFD` (`STOP_TRANSMISSION`) для multi-block по-прежнему уходит в `sd.c` как `CMD12`;
- адресация абсолютная, та же, что у чтения: `arg` команды — номер сектора в образе (MBR и
  партиция не сдвигаются, `sd.c` сам считает `sd_bootpart_offset`);
- трасса `SSISD_TRACE=1`: `data token 0xfe cmd=24 arg=0x...`, `cmd=24 arg=0x... data=512
  write done`, `stop transmission (0xfd)`, плюс прежние `cmd=`/`r1 cmd=`/`read done`.

**Патч 0024 (`hw/sd/sd.c`) — настоящий R1 в SPI:**

- на illegal-команду в SPI-режиме `sd_do_command()` больше не возвращает 0 байт, а отдаёт R1 из
  `card_status` (бит `ILLEGAL_COMMAND` и остальные); «нет ответа» в SPI невозможно, и подстановка
  `0x04` в `ssi-sd` осталась только как fallback;
- `CMD13` (`SEND_STATUS`) в SPI отдаёт 32-битный статус вместо 16 байт CSD: `ssi-sd` из него
  собирает ровно 2 байта R2 (`s->arglen = (cmd == 13) ? 2 : 1`), как и было задумано в его коде.

### Что появилось на карте (чистые flash + SD, 40 с прогона)

```
$ QEMU_FRESH_FLASH=1 QEMU_FRESH_SD=1 tools/qemu/boot.sh 40
build/qemu/sd.img: 67108864 bytes, FAT16, 2048 B/cluster, 32184 clusters, ...
  /books/test.txt
  /.crosspoint/
  /books/lichnyy_drug_boga.fb2.zip (765 bytes)
  /books/lichnyy_vrag_boga.fb2.zip (816 bytes)

$ tools/qemu/sd-files.py --image build/qemu/sd.img
build/qemu/sd.img: FAT16 'XTEINKX3', partition @2048, 2048 B/cluster, 32184 clusters
  /.crosspoint/
  /.crosspoint/shelf/
  /.crosspoint/shelf/books.json  693 B        ← файла не было: запись не работала
  /.crosspoint/txt_421684520/
  /.crosspoint/txt_421684520/.cover-render-v2  0 B
  /books/
  /books/test.txt  121 B
  /books/lichnyy_drug_boga.fb2.zip  765 B
  /books/lichnyy_vrag_boga.fb2.zip  816 B
  5 file(s), 2395 bytes
```

Индекс содержит настоящие метаданные (это и есть доказательство, что метаданные `.fb2.zip`
собрались: временный `.shelf_fb2_<hash>.zip.fb2` пишется рядом с книгой на карте):

```json
{"version":3,"books":[
 {"path":"/books/test.txt","format":"txt","title":"test","metaOk":true},
 {"path":"/books/lichnyy_drug_boga.fb2.zip","format":"fb2zip","title":"Личный друг Бога",
  "author":"Ольга Громыко","series":"urchim","seriesIndex":"2","genre":"sf_fantasy","metaOk":true},
 {"path":"/books/lichnyy_vrag_boga.fb2.zip","format":"fb2zip","title":"Личный враг Бога",
  "author":"Ольга Громыко","series":"urchim","seriesIndex":"1","genre":"sf_fantasy","metaOk":true}]}
```

Трасса записи за тот же прогон (24 блока, `SSISD_TRACE=1`; строки `r1` идут после `write done`
потому, что R1 относится к следующей команде — токен идёт раньше):

```
ssi_sd: [2477 ms] cmd=24 arg=0x00125200 rlen=4
ssi_sd: [2477 ms] r1 cmd=24 = 0x00
ssi_sd: [2477 ms] data token 0xfe cmd=24 arg=0x00125200
ssi_sd: [2479 ms] cmd=24 arg=0x00125200 data=512 write done
ssi_sd: [2480 ms] r1 cmd=24 = 0x05
ssi_sd: [2492 ms] cmd=25 arg=0x00126200 rlen=4          # multi-block: два блока подряд
ssi_sd: [2492 ms] r1 cmd=25 = 0x00
ssi_sd: [2492 ms] data token 0xfc cmd=25 arg=0x00126200
ssi_sd: [2493 ms] cmd=25 arg=0x00126200 data=512 write done
ssi_sd: [2494 ms] data token 0xfc cmd=25 arg=0x00126200
ssi_sd: [2495 ms] cmd=25 arg=0x00126200 data=512 write done
ssi_sd: [2495 ms] stop transmission (0xfd) cmd=25
ssi_sd: [2495 ms] cmd=24 arg=0x00100200 rlen=4          # FAT-копия (LBA 2049)
ssi_sd: [2496 ms] cmd=24 arg=0x00100200 data=512 write done
```

Все адреса — абсолютные номера секторов образа, как и у чтения: `0x00125200` = сектор 2345 (данные
партиции, `@2048`), `0x00100200` = сектор 2049 (первая копия FAT), `0x00126200` = 2353. Ни одна
запись не уходит в MBR или в таблицу разделов.

Кадр библиотеки (шестой refresh прогона, upright 528×792): `build/qemu/q6/frame-5.png` —
три плитки: `test` (TXT-иконка), «Личный враг Бога / Ольга Громыко», «Личный друг Бога /
Ольга Громыко», у двух последних бейдж `FB2`. Именно то, чего не было в Q5: там на плитках
стояли имена файлов, потому что без записи нельзя было распаковать `.fb2.zip` в скретч-файл.

### Smoke-гейт теперь проверяет книги, а не только кадры

`tools/qemu/smoke.sh` после отрисовки библиотеки читает **образ карты** (через `sd-files.py`) и
требует:

1. `/.crosspoint/shelf/books.json` существует (иначе `SMOKE FAIL: library index ... was not
   written to the card`);
2. в индексе есть хотя бы одна книга с непустым `title`;
3. если `build/qemu/sd-books/` не пуст — среди них есть FB2/FB2.ZIP с настоящим названием (то
   есть скретч-файл метаданных тоже записался) и каждая тестовая книга лежит в `/books`.

Каждый шаг с кнопкой теперь ждёт кадр, содержимое которого **отличается** от предыдущего: с
работающей записью гость стал занятее, и первый refresh после нажатия иногда ещё показывает
прежний экран (перерисовка «того же» реакцией не считается — иначе гейт падал на ровном месте,
наблюдалось один раз из четырёх прогонов).

Итог в `smoke.txt` и в последней строке:

```
  card_files: 8
  card_index_books: 4
  card_titles: test; Личный друг Бога; Личный враг Бога; crash_report
  card_fb2_titles: 2
SMOKE OK: frames=8 panic=1 books=4 (known cold-boot panic, rebooted=1; strict mode would fail)
```

`build/qemu/sd-books/` попадает и в карту смоука: `x3emu.ensure_sd()` копирует каждый файл из
него в `/books` — теперь это единственное место с этой логикой, `boot.sh` и смоук используют её
через один и тот же `make-sd-image.py --add`.

### Ограничения (Q6)

- CRC16 данных не проверяется (upstream-модель тоже его только пропускает); команды CRC7 тоже
  (`sd_req_crc_validate()` в upstream отключён) — для SdFat это неважно, для проверки целостности
  записи нужен отдельный шаг;
- BUSY-фаза — бюджет в 8 опрошенных байт, а не время: тайминги программирования флеша не
  моделируются (для «карта занята N мс» нужен таймер);
- стирание (`CMD32/33/38`) не реализовано: запись идёт поверх, как в upstream;
- `state.json` в этих прогонах не появляется, и это ожидаемо: `ShelfActivity::saveState()`
  вызывается только когда применяется фильтр/сортировка/скрытие книги
  (`src/activities/shelf/ShelfActivity.cpp`: ветки `BookAction::Hide`, `Overlay::HiddenBooks` и
  общий `applyQuery(); saveState()`), а сценарии диалог фильтров только открывают и закрывают
  кнопкой Back. Запись доказана другими файлами: `books.json`, маркер
  `txt_*/.cover-render-v2` и (в прогонах q5) обложки `thumb_180x240.bmp` / `thumb_264x352.bmp`;
- `/crash_report.txt` тоже пишется (в смоуке он попадает в индекс как книга `crash_report`) —
  то есть файл, из-за которого Q5 падал на первой же записи, теперь создаётся;
- в Q5 отклик кнопок мерился как 117–148 мс; теперь `q5-scenarios.py` даёт **174–2751 мс**, и это
  не регрессия ввода: с работающей записью каждое действие, меняющее экран, дописывает на карту
  кэш обложек (`/.crosspoint/txt_*/thumb_180x240.bmp`, `thumb_264x352.bmp`), а запись в модели
  идёт побайтно через SSI — 36 блоков по 512 байт на обложку стоят сотни миллисекунд. Быстрые
  шаги (перемещение по меню) остаются 174–210 мс;
- `q5-scenarios.py` теперь **всегда** стартует с чистым flash и заново собранной картой: с
  работающей записью повторный запуск на «грязном» flash накапливает счётчик неудачных boot'ов
  (`BootHealth`), и после нескольких убитых прогонов прошивка совершенно правильно уходит в
  «recovery firmware mode» — экран выбора `*.bin` для обновления с SD вместо полки. Старое
  поведение доступно флагом `--reuse-state`;

### Проверки Q6 (команды и результаты)

- `ninja -C .venv/qemu/src/build qemu-system-riscv32` — exit 0 (пересобрано 3 раза по ходу).
- `SSISD_TRACE_MAX=600 QEMU_FRESH_FLASH=1 QEMU_FRESH_SD=1 tools/qemu/boot.sh 40` — exit 0,
  **24** строки `write done`, `books.json` на карте, паники в окне нет
  (`build/qemu/q6/{boot.log,uart.log,frame.bin}`).
- `tools/qemu/sd-files.py --image build/qemu/sd.img` — exit 0, дерево выше (5 файлов,
  2395 байт, снимок — `build/qemu/q6/card.txt`); `--cat /.crosspoint/shelf/books.json` — exit 0,
  693 байта JSON с названиями (`build/qemu/q6/books.json`).
- `tools/qemu/panel-frame.py ... build/qemu/q6/frame.bin` — 6 записей, последняя 26 195 чёрных
  пикселей (6,26 %, 107 различных значений байта); PNG кадра — `build/qemu/q6/frame-0..5.png`,
  библиотека — `build/qemu/q6/frame-5.png`.
- `tools/qemu/smoke.sh` — **два прогона подряд, exit 0**: `SMOKE OK: frames=8 panic=1 books=4`,
  шаги 91 / 1310 / 688 мс, `card_titles: test; Личный друг Бога; Личный враг Бога; crash_report`.
- `tools/qemu/q5-scenarios.py` — exit 0, `failures: 0`, `rotation check: OK`, отклик
  174–2751 мс (см. «Ограничения (Q6)»), boot probe «menu opened: False».
- `tools/qemu/verify-patches.sh --make-pre` — `RESULT forward=0 md5_mismatch=0 reverse=0
  pre_q4_mismatch=0 pre_q4_skipped=0` (набор 0001..0024 применяется и откатывается чисто;
  `PRE_Q4_FILES=11`).

## Q7: экран чтения книги — 4 градации, а не чёрный кадр (проверено 19.11.2026)

Итог этапа одной строкой: **кадр читалки рисуется как страница.** Причина чёрного экрана
была не в отсутствии 4-градаций в модели (обе RAM-плоскости она хранила с Q4), а в том, что
запись кадра отдавала **код** 4-градационного прохода, а инструменты рендерили её как
1-битную BW-картинку (`1` = белый). У AA-плоскостей полярность обратная (`1` = чернила), а
сама страница в них не лежит — их проход только «подталкивает» уже показанную BW-страницу.

### Как воспроизвести

```bash
# 1. кадр читалки в эмуляторе: Enter на книге открывает карточку, второй Enter — читалку
tools/qemu/reader-screen.py                  # проверка + PNG в build/qemu/q7/
DISPLAY=:0 tools/qemu/run-ui.sh              # то же руками в окне X11

# 2. разбор дампа кадров (после Q7: --plane screen = 4 оттенка, старый вид — --plane dtm2)
tools/qemu/panel-frame.py --input build/qemu/q7/frame.bin                 # 4 оттенка
tools/qemu/panel-frame.py --input build/qemu/q7/frame.bin --plane dtm2    # сырая RAM, 1 бит
tools/qemu/panel-frame.py --input build/qemu/q7/frame.bin --all --stats-only
```

### Причина: у читалки два прохода, и второй — не картинка

`GfxRenderer::renderGrayscalePlanesSequential()` (его зовёт `EpubReaderActivity::render()`,
`EpubReaderActivity.cpp:4497`) делает так:

1. **BW-проход** — обычная 1-битная страница в DTM2 (`1` = белый), затем `0x12`: кадр
   правильный, ровно как у библиотеки;
2. **4-градационный (AA) проход** — `copyGrayscaleLsb()` пишет плоскость LSB в **DTM1**,
   `copyGrayscaleMsb()` — плоскость MSB в **DTM2**, затем `displayGray()` грузит банк `_gc`
   (`0x50` + LUT `0x20..0x24`) и снова `0x12`.

Второй проход заполняет только пиксели сглаживания: фон и «тело» глифов в этих плоскостях
нулевые. Трасса панели (до фикса, `build/qemu/q7/run2/boot.log`, строки 178–191) — что реально
пришло в панель перед вторым кадром:

```
[UC8253] cmd 0x10 (#121, 0 data bytes since last cmd)
[UC8253] cmd 0x11 (#122, 52272 data bytes since last cmd)   # DTM1 = плоскость LSB
[UC8253] cmd 0x10 (#123, 0 data bytes since last cmd)
[UC8253] cmd 0x11 (#124, 52272 data bytes since last cmd)   # DTM1 ещё раз (baseline-синк)
[UC8253] cmd 0x13 (#125, 0 data bytes since last cmd)
[UC8253] cmd 0x11 (#126, 52272 data bytes since last cmd)   # DTM2 = плоскость MSB
[UC8253] cmd 0x50 (#127, 0 data bytes since last cmd)       # CDI 0x29 0x07
[UC8253] cmd 0x21..0x24 (#129..#132, 42 bytes each)         # банк _gc
[UC8253] cmd 0x12 (#133, 42 data bytes since last cmd)      # DRF -> запись кадра
```

Что из этого видели инструменты (`panel-frame.py --plane dtm2`, то есть `1` = белый):

```
library frame:  dtm2 black  2.4%     # полка: белый фон, чёрный текст
reader  #7:     dtm2 black  7.3%     # BW-проход страницы (правильно)
reader  #8:     dtm1 black 99.0%, dtm2 black 98.4%   # AA-проход: «полностью чёрный экран»
```

Отсюда и картинка «чёрный экран»: 98 % нулей — это бумага в инверсной кодировке, а 2 %
единиц — контуры глифов (`build/qemu/q7/legacy/ui12-dtm2.png` — тот самый чёрный кадр).

### Что изменено в модели (патч 0025, `hw/display/xteink_uc8253.c/.h`)

Панель теперь ведёт **состояние экрана**, а не только RAM: две плоскости `screen MSB/LSB`,
`level = 2*MSB + LSB` (0 = бумага, 1 = светло-серый, 2 = тёмно-серый, 3 = чёрный).

- **BW-обновление**: экран = инверсия DTM2 (белый → 0, чёрный → 3), как и было видно раньше;
- **4-градационное обновление**: код в RAM-плоскостях применяется как «подталкивание» уже
  показанной страницы — ровно то, на что рассчитывает драйвер
  (`Uc8253X3Driver::displayGray` «nudge»):

  | (MSB,LSB) в DTM2/DTM1 | что делает модель |
  |---|---|
  | (0,0) | пиксель не трогается: остаётся то, что нарисовал BW-проход |
  | (1,0) | светло-серый (level 1) |
  | (0,1) | тёмно-серый (level 2) |
  | (1,1) | чёрный (level 3) |

- **Как модель отличает 4-градационный refresh от BW** (панель не получает явного «режима»,
  его задаёт только LUT): 4-градационная пара — это `DTM1` (LSB), сразу за ним `DTM2` (MSB),
  между ними только `0x11 DATA_STOP`, и первый payload — не константная заливка `0xFF`
  (полный BW-синк начинается именно с неё). BW-путь такого хвоста не даёт: fast/half пишут
  только DTM2, а baseline-синки после refresh отделены от следующего DTM2 загрузкой банка.
  В `boot.log` каждый кадр теперь помечен: `... 1-bit BW` или `... 4-level gray`.
- **Запись кадра расширена** (`version=2`, `planes=4`): плоскость 0 — DTM1, 1 — DTM2,
  2 — screen MSB, 3 — screen LSB; `flags` бит 0 — порядок строк (снизу вверх), бит 1 — refresh
  был 4-градационным. Старые парсеры не ломаются: число плоскостей берётся из заголовка.

### Что изменено в инструментах

| файл | что |
|---|---|
| `panel-frame.py` | `--plane screen` (по умолчанию) рендерит 4 оттенка в 8-битный PNG/PGM и печатает доли оттенков; `--plane dtm1|dtm2` — прежний 1-битный путь по сырой RAM; в шапке вывода — `v2`, `planes 4`, тип последнего refresh |
| `x3emu.py` | `rows_upright(frame, plane="screen")` — композит для 1-битных фронтендов (любые чернила = чёрный); `levels_upright()`, `frame_png(plane="screen")` — 4 оттенка; `frame_screen_stats()`; `frame_stats()` остался 1-битным по сырой RAM (его цифры не поменялись) |
| `x3-gui.py` | `--plane` по умолчанию `screen`, поэтому окно показывает страницу, а не инверсный код (4 оттенка сведены в 1 бит: AA-края чёрные) |
| `reader-screen.py` | новый скрипт-проверка «в читалке есть текст» (см. ниже) |

### Проверка: `tools/qemu/reader-screen.py`

```bash
tools/qemu/reader-screen.py                     # прошивка build/qemu/fw, карта build/qemu/ui/sd.img
tools/qemu/reader-screen.py --firmware p/fw.bin --book "Личный враг" --timeout 300
tools/qemu/reader-screen.py --check-dump build/qemu/ui/frame.bin --frame 36   # офлайн, старый дамп
```

Скрипт эмулирует пользователя (полка → карточка книги → читалка), ждёт кадр читалки и требует,
чтобы это была **страница текста**: доля чернил в разумном диапазоне, достаточно строк с
чернилами и ≥ 7 отдельных штрихов в строке (у текста ~11,5 на строку — по одному на глиф; у
карточки книги и экрана ошибки ~4). Провал печатает причину и возвращает 1; артефакты —
`build/qemu/q7/reader-page.png` (4 оттенка), `reader-raw-dtm2.png` (сырая RAM, «как было»),
`reader-library.png`, `boot.log`, `uart.log`, `reader-screen.txt`.

### Проверки Q7 (команды и результаты)

- `ninja -C .venv/qemu/src/build qemu-system-riscv32` — exit 0 (пересобрано 4 раза по ходу).
- `tools/qemu/reader-screen.py` — **exit 0**, одна попытка:
  `ink_pct 7.598 / paper_pct 92.402 / gray_pct 0.61 / text_rows 410 / ink_runs_per_row 11.53`,
  `reader_refresh: 4-level gray`, `reader_ms 1989`, `READER SCREEN OK`
  (`build/qemu/q7/reader-page.png` — страница «Михаил Кликин, ЛИЧНЫЙ ВРАГ БОГА», 6/6).
- `tools/qemu/reader-screen.py --check-dump build/qemu/ui/frame.bin --frame 36` (**старый дамп,
  модель до патча**) — **exit 1**: `raw DTM2 ink 98.4%`,
  `READER SCREEN FAIL: raw RAM plane has no readable page (the Q7 black frame)` — то есть
  проверка действительно ловит прежний чёрный кадр.
- `tools/qemu/reader-screen.py --check-dump build/qemu/q7/run2/frame.bin` (дамп после патча) —
  **exit 0**: `ink_pct 7.598`, `text_rows 410`, `ink_runs_per_row 11.53`.
- `tools/qemu/panel-frame.py --input build/qemu/q7/run2/frame.bin` — exit 0:
  `11 frame record(s), v2 ... planes 4, last refresh 4-level gray`;
  `SCREEN: ink 7.598% (gray 0.610%)`, `paper 92.402% | light 0.610% | dark 0.000% | black 6.988%`;
  тот же кадр через `--plane dtm2` даёт `black 98.113%` — исходный «чёрный экран».
- `tools/qemu/verify-patches.sh --make-pre` — `RESULT forward=0 md5_mismatch=0 reverse=0
  pre_q4_mismatch=0 pre_q4_skipped=0`, `FORWARD_FAILS=0`, `TOUCHED_FILES=23 MISMATCHES=0`,
  `REVERSE_FAILS=0` (0025 применяется и откатывается с чистого тарбола; деревья удалены).
- `tools/qemu/smoke.sh` — exit 0, `SMOKE OK: frames=8 panic=1` (прежняя холодная паника).
- `tools/qemu/q5-scenarios.py` — exit 0, 0 провалов (включая `rows_upright()` vs
  `rows_upright_naive()` — оба теперь на композите).
- `tools/qemu/nav-measure.py` — скрипт работает (7 нажатий, кадры приходят), но вердикт
  **SLOW (exit 1)**: шаги 0.3–3.8 с при бюджете 400 мс. Это не регрессия Q7 — правки не
  касаются ввода и таймингов, а на хосте в этот момент работали два чужих прогона QEMU
  (~20 % CPU каждый). Отдельно: на общей карте 3 книги в одном ряду, поэтому `Down`
  упирается в `std::clamp` и не перерисовывает экран (`down -> None` в первом прогоне,
  `ShelfActivity::moveSelection` возвращается раньше, чем `requestUpdate()`).

### Ограничения (Q7)

- Композит опирается на то, что `_gc`-банк **только подталкивает** уже показанную BW-страницу
  (так устроен `Uc8253X3Driver::displayGray`). Если прошивка когда-нибудь начнёт рисовать
  4 градации отдельным полным банком (factory-режим `_full`), композит покажет страницу
  BW-прохода — это надо будет перепроверить тем же `reader-screen.py`;
- окно/PTL (`0x90`/`0x91`/`0x92`) по-прежнему не учитывается: частичная запись ложится в начало
  плоскости (ограничение Q4). Для полностраничных записей читалки это верно;
- запись кадра выросла до 4 плоскостей — 209 120 байт на кадр вместо 104 576; парсеры берут
  число плоскостей из заголовка, но внешние потребители старого формата увидят `planes=4`;
- окно X11 и терминальный бэкенд 1-битные: 4 оттенка там сводятся к «любые чернила = чёрный»,
  поэтому AA-края в окне чернее, чем на панели. Полные 4 оттенка — в PNG от `panel-frame.py` и
  `reader-screen.py`;
- навигация по полке недетерминирована: нажатие Right иногда складывается с перерисовкой
  обложек и не двигает выделение (наблюдалось 3 раза из 5). `reader-screen.py` поэтому делает
  до 3 попыток, сдвигая выделение, и требует страницу текста на каждой;

## Q8: холодные паники при холодной загрузке — CRC RTC fast memory и deep sleep (19.11.2026)

Итог этапа одной строкой: **холодной паники больше нет.** Причина была в модели, а не в
прошивке: на холодной загрузке QEMU прошивка совершенно правильно уходит в `deep sleep`, а
модель этот уход не завершала — CPU навсегда застревал в ожидании аппаратного CRC по RTC
fast memory, и interrupt watchdog печатал `Guru Meditation … Interrupt wdt timeout on CPU0`.

### Как воспроизвести

```bash
tools/qemu/smoke.sh                                   # был panic=1..3, стало panic=0
build/qemu/panic-0/panic-hold.sh build/qemu/q8 60 100 0.1   # то же без смоука, 3 прогона
```

Паника воспроизводится только когда в машине есть `-device xteink-buttons` (как в `x3emu.py`
и в смоуке); без устройства её нет — то есть дело не в кнопке, а в профиле прогона.

### Причина: wake-gate отклоняет «нажатие», которого не было, а модель не умеет просыпаться

1. Холодный старт эмулятора — это `rst:0x1 (POWERON)`, но линия Power в этот момент
   отпущена (никто не держит кнопку) и USB-питания нет. `HalGPIO::getWakeupReason()`
   (`lib/hal/HalGPIO.cpp:498-503`) для X3 в этом случае возвращает `PowerButton`, а
   `verifyPowerButtonWakeup()` (`lib/hal/HalGPIO.cpp:394-399`) видит не нажатый пин и
   `trustedQuickWake=false` (маркера `cpwake` на свежем NVS нет) — вердикт `NotPressed`.
   `setup()` по этому вердикту уходит обратно в сон (`src/main.cpp:897-901`,
   `powerManager.startDeepSleep()`), и на реальном железе это ровно то, что нужно.
2. `rtc_deep_sleep_start()` (`esp_hw_support/port/esp32c3/rtc_sleep.c:302`) перед сном считает
   CRC по RTC fast memory: пишет `SYSTEM_RTC_FASTMEM_CONFIG` (`0x600C0048`) с
   `RTC_MEM_CRC_START` (бит 8) и **ждёт в цикле** бит `RTC_MEM_CRC_FINISH` (бит 31). В
   QEMU-модели SYSTEM-блока (`hw/riscv/esp32c3_clk.c`) этого регистра не было, чтение
   возвращало 0 → цикл не завершался. Прерывания на этом участке выключены, поэтому через
   300 мс срабатывает interrupt watchdog: `mcause=0x80000018` (внешнее прерывание IDWDT),
   `MEPC` внутри `rtc_sleep_low_init`/`rtc_deep_sleep_start`, паника `rst:0xc`
   (`RTC_SW_CPU_RST`). Второй boot идёт уже с `reset_reason != POWERON`, wake-gate
   пропускается и прошивка доходит до UI — отсюда и «паника есть, но всё работает».
3. Ровно поэтому же `/crash_report.txt` на карте показывал `Reset reason (esp_reset_reason): 5`
   (это `ESP_RST_INT_WDT`), `Panic reason: not captured` и регистры из кадра паники.

### Что изменено в модели (патч 0026)

- `hw/riscv/esp32c3_clk.c` + `include/hw/riscv/esp32c3_clk.h`: реализованы
  `SYSTEM_RTC_FASTMEM_CONFIG` и `SYSTEM_RTC_FASTMEM_CRC`. Запись с `RTC_MEM_CRC_START`
  сразу выставляет `RTC_MEM_CRC_FINISH` (CRC-значение пишется нулём: прошивка кладёт его в
  `RTC_CNTL` и не сверяет — wake stub не регистрируется);
- `hw/misc/esp32c3_rtc_cntl.c` + `include/hw/misc/esp32c3_rtc_cntl.h`: запись
  `RTC_CNTL_STATE0.SLEEP_EN` (последнее, что делает любой sleep) моделируется как
  **пробуждение**: сброс машины, в `RTC_CNTL_RESET_STATE` остаётся причина
  `ESP32C3_DEEPSLEEP_RESET` → ROM печатает `rst:0x5 (DSLEEP)`, `esp_reset_reason()` даёт
  `ESP_RST_DEEPSLEEP`, `HalGPIO::getWakeupReason()` возвращает `Other` и прошивка спокойно
  поднимается в UI (и `BootHealth` такой ресет крахом не считает).

Проверено: `tools/qemu/panic-hold.sh` (3 прогона по 60 с) — `panic=0`, кадры библиотеки
рисуются, в `boot.log` ровно одна строка
`[RTCCNTL] sleep requested (STATE0.SLEEP_EN) -> RTC reset, deep-sleep cause`; `smoke.sh` —
`SMOKE OK: … panic=0`; `verify-patches.sh --make-pre` — `RESULT forward=0 md5_mismatch=0
reverse=0 pre_q4_mismatch=0 pre_q4_skipped=0` (26 патчей, 27 затронутых файлов).

### Ограничения (Q8)

- **deep sleep моделируется как «мгновенное пробуждение»**: «спящего» состояния нет, чип
  сразу перезагружается с причиной `DSLEEP`. Для тестов это удобно (UI доступен), но реальную
  длительность сна и пробуждение по RTC/GPIO это не проверяет;
- light sleep отличать нечем: он ставит тот же `SLEEP_EN` и тоже даст сброс. Прошивка light
  sleep не использует (`esp_light_sleep_start` в `src/`/`lib/` нет), но при появлении его в
  коде модели понадобится отдельная ветка;
- CRC по RTC fast memory не считается: значение всегда 0. Это безопасно только до тех пор,
  пока прошивка не регистрирует wake stub (`esp_deep_sleep_register_wake_stub`), иначе ROM
  на пробуждении сверит CRC;
- «лишний» boot при холодной загрузке остаётся: сначала POWERON → отказ wake-gate →
  DSLEEP-сброс → UI. Это поведение самой прошивки (так же ведёт себя железо при отпущенной
  кнопке), а не эмулятора: чтобы UI появился на первом boot, кнопку Power надо держать при
  старте.

## Что осталось для «полной эмуляции» (оценка по итогам разведки)

1. **GP-SPI2 (SPI2) + SD** — сделано в Q2/Q3/Q4 (патчи 0008, 0012, 0014, 0015, 0021):
   модель `ssi.esp32c3.gpspi` на `0x60024000`, панель и `ssi-sd` на её SSI-шине, CS — из
   выходных линий GPIO (0009), `MS_DLEN` по карте C3, карта в SPI-режиме, настоящий R1 для
   CMD58, FAT16-образ и смонтированный том. **Запись закрыта в Q6** (патчи 0023, 0024):
   `CMD24`/`CMD25` с DATA-токеном и 512 байтами, BUSY-фаза, настоящий R1 в SPI, поэтому
   прошивка сохраняет `/.crosspoint/shelf/books.json`, а метаданные `.fb2.zip` доходят до
   названий (см. «Q6»). Осталось: стирание (`CMD32/33/38`), проверка CRC и тайминги
   программирования.
2. **GPIO-прерывания** — сделано в 0006/0017: `INT_TYPE`/`INT_ENA` → `STATUS` → level-IRQ в
   intmatrix + `GPIO_PCPU_INT` (`0x5c`), который читает ISR. Проверено на живой прошивке:
   ISR завершения BUSY (GPIO6, `attachInterrupt`, RISING) срабатывает, `waitRefreshComplete()`
   возвращается, шторма прерываний нет.
3. **Панель UC8253** — сделано в 0010/0018/0019/0025: команды/данные, DC/RST, BUSY-семантика
   (`X3TwoPhase`) с правдоподобными длительностями, RAM-плоскости и дамп кадров в chardev,
   конвертер в PNG, 4-градационный (LSB/MSB) композит экрана — читалка рисуется как страница
   (см. «Q7»). Осталось: учитывать partial-window (`0x90`/`0x91`) при записи плоскостей.
4. **NVS** — закрыто в Q2: раздел `0x9000`/`0x5000` прошивка форматирует сама, `Preferences`
   работает; оставшиеся два `NOT_FOUND` — read-only открытие ещё не созданного namespace
   `cpwake` (см. «Блок 1»). В Q3 выяснилось, что NVS ещё и хранит кэш платы `cphw/dev_det`,
   который отключает I2C-фингерпринт (см. «Причина 1» в разделе Q3).
5. **Кнопки** — цифровые линии (0004), АЦП-лестница (0007) и трасса конверсий (0022);
   инъекция `xteink-buttons` доведена до реальной реакции UI и **закрыта в Q5**: отклик
   117–148 мс, долгое нажатие видно на отпускании, «1–5 с» из Q4 оказались окном загрузки
   (паника IDWDT + reboot), см. «Q5». Патчей в Q5 не добавлялось.
6. **I2C-фингерпринт платы** — **закрыто в Q3** (патчи 0013, 0016): кодировка команд i2c v2
   (`READ=3`, `STOP=2`), `detectXteinkVerdict()` = X3Confirmed, плата поднимается как X3.
   Напоминание: без `QEMU_FRESH_FLASH=1` (или чистого NVS) проба не запускается вообще —
   прошивка верит кэшу `cphw/dev_det`.
7. **Deep sleep / light sleep — снято в Q8 (патч 0026).** `esp_deep_sleep_start()` больше не
   зависает: CRC по RTC fast memory в модели завершается, а `SLEEP_EN` моделируется как
   пробуждение — сброс с причиной `DSLEEP` (`rst:0x5`), после которого прошивка поднимается в
   UI. Что по-прежнему не проверяемо: длительность сна и пробуждение по кнопке/RTC (чип
   «просыпается» мгновенно), light sleep (тот же `SLEEP_EN` даст сброс вместо возврата) и
   wake stub с проверкой CRC. Подробности и ограничения — в разделе «Q8».

Ориентир по трудозатратам: 4–8 недель одного разработчика, причём п.3 (панель) — основной
риск. Форк Espressif официально не поддерживается, так что цена поддержки ложится на нас.
