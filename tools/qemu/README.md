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
for p in ../../patches/*.patch; do patch -p1 < "$p"; done   # наши правки форка
export PATH=$PWD/../../../../.venv/bin:$PATH
export PKG_CONFIG_PATH=$PWD/../../slirp/pkgconfig
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

После патчей прошивка грузится дальше: I2C-контроллер существует, машина не падает на
неизвестном типе. WDT всё ещё срабатывает позже — ждут панель и SD.

## Что осталось для «полной эмуляции» (оценка по итогам разведки)

1. **GP-SPI2 + SD** — модель `esp32c3_spi` есть, но она заточена под flash (cmd/addr/dummy);
   нужен режим 8-битных потоков и инстанцирование второго контроллера. SD — готовый
   `hw/sd/ssi-sd.c` с backing-файлом образа карты.
2. **GPIO matrix/IOMUX** — сейчас в модели только strap. Прошивка (Arduino core) настраивает
   пины через регистры матрицы: нужен минимальный набор (выходная функция, входное чтение,
   прерывания) + фиксированная распиновка платы X3. Самый большой и рискованный кусок.
3. **Панель UC8253** — новый SSI-девайс: приём команд/данных, эмуляция BUSY, кадр в PNG
   через chardev.
4. **Кнопки** — chardev → линии GPIO-входа.
5. **NVS** — пустой отформатированный раздел в flash-образе, чтобы `Preferences` не ругался.

Ориентир по трудозатратам: 4–8 недель одного разработчика, причём п.2 — основной риск.
Форк Espressif официально не поддерживается, так что цена поддержки ложится на нас.
