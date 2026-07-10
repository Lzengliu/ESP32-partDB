# ESP32-S3 Part-DB Terminal Wiring and Bring-Up

This wiring matches `main/board_config.h`. Treat it as the current firmware
contract: if a GPIO changes, update that file and this document together.

## Safety Notes

- Use a common GND between the ESP32-S3 board and every module.
- Use 3.3 V logic. Do not feed 5 V signals into ESP32-S3 GPIOs.
- Power high-current backlights from a rail that can actually supply them. If a
  display module exposes `LED+`/`LEDA` instead of a logic `BL` pin, use the
  module's recommended resistor or driver circuit.
- GPIO 43 and GPIO 44 are used by the console UART in the current build. Do not
  wire peripherals to them.
- GPIO 0 is a boot strap pin and is intentionally not used here.
- Optional hardware is not initialized at boot. Use the web diagnostics after
  wiring each module.

## WiFi Setup

On boot the device starts a SoftAP:

| Item | Value |
| --- | --- |
| SSID | `PartDB-Terminal` |
| Password | `partdb1234` |
| Web UI | `http://192.168.4.1/` |

The serial log also prints the AP credentials after the AP starts. After WiFi
credentials are saved and STA mode connects successfully for the first time,
the firmware persists provisioning as complete and disables the AP on the next
reboot. Use the Web settings page over the STA IP address to enable the AP again
when needed.

## ILI9488 SPI Display

The firmware uses 4-wire SPI write mode. It does not use the ILI9488 parallel
interface.

| Display module pin | ESP32-S3 GPIO | Notes |
| --- | --- | --- |
| `VCC` / `3V3` | 3.3 V | Use the module's documented supply voltage. |
| `GND` | GND | Common ground. |
| `SCL` / `SCK` / `CLK` | GPIO 21 | SPI clock. |
| `SDA` / `SDI` / `MOSI` | GPIO 47 | SPI data to display. |
| `SDO` / `MISO` | Not connected | The current driver is write-only. |
| `CS` | GPIO 14 | SPI chip select. |
| `DC` / `RS` / `A0` | GPIO 2 | Command/data select. |
| `RST` / `RESET` | GPIO 1 | Display reset. |
| `BL` / `LED` | GPIO 42 | PWM backlight control if the module has a logic BL pin. |

Bring-up: connect only power, GND and the display lines first, then open the web
UI and click `显示测试`. A working display should show red, green, blue, yellow
and white horizontal bands.

## FT6336 Capacitive Touch

This is for FT6336/FT6x36-style capacitive touch panels. Resistive touch modules
using XPT2046 are not supported by the current touch driver.

| Touch pin | ESP32-S3 GPIO | Notes |
| --- | --- | --- |
| `VCC` / `3V3` | 3.3 V | 3.3 V logic. |
| `GND` | GND | Common ground. |
| `SDA` | GPIO 35 | Shared Touch/NFC I2C bus. |
| `SCL` | GPIO 36 | Shared Touch/NFC I2C bus. |
| `RST` | GPIO 37 | Touch reset. |
| `INT` | GPIO 41 | Touch interrupt input. |

The firmware expects I2C address `0x38`. Bring-up: click `读取触摸`. Expected
JSON has `ok: true`; touching the panel should set `touched: true` and update
`x`/`y`.

## PN532 NFC

Set the PN532 module to I2C mode. Many boards use DIP switches or solder pads
for this; the exact setting depends on the module.

| PN532 pin | ESP32-S3 GPIO | Notes |
| --- | --- | --- |
| `VCC` / `3V3` | 3.3 V | Prefer 3.3 V unless the module explicitly level-shifts I2C. |
| `GND` | GND | Common ground. |
| `SDA` | GPIO 35 | Shared Touch/NFC I2C bus. |
| `SCL` | GPIO 36 | Shared Touch/NFC I2C bus. |
| `IRQ` | Not connected | Not used by current firmware. |
| `RSTO` / `RSTPDN` | Not connected | Not used by current firmware. |

The firmware expects PN532 I2C address `0x24`. Bring-up: click `读取 NFC` while
holding a tag near the antenna. Expected JSON has `ok: true`, `present: true`
and a non-empty `uid`.
If the module initializes but no tag is detected, the endpoint returns
`ok: true`, `present: false`.

## TF Card, SDMMC 1-Bit

Use a 3.3 V TF/SD breakout. The current firmware uses SDMMC 1-bit mode, not SPI.

| TF module pin | ESP32-S3 GPIO | Notes |
| --- | --- | --- |
| `VCC` / `3V3` | 3.3 V | 3.3 V module. |
| `GND` | GND | Common ground. |
| `CLK` | GPIO 39 | SDMMC clock. |
| `CMD` | GPIO 38 | SDMMC command. |
| `D0` / `DAT0` | GPIO 40 | SDMMC data 0. |
| `D1` / `DAT1` | Not connected | Not used in 1-bit mode. |
| `D2` / `DAT2` | Not connected | Not used in 1-bit mode. |
| `D3` / `DAT3` / `CS` | 10 kΩ pull-up to 3.3 V | Required by many SPI-labeled TF breakouts even in SDMMC 1-bit mode. |

Bring-up: insert a FAT-formatted card and click `挂载 TF`. Expected JSON has
`ok: true`, `mounted: true`, a non-zero `card_size_mb` and `free_mb`.

The web UI also has `格式化 TF` and `初始化目录` actions on the resource page.
Formatting is destructive and should only be used for cache/media cards. After
format or initialization, the firmware creates:

| Path | Purpose |
| --- | --- |
| `/sdcard/cache` | General cache root. |
| `/sdcard/cache/images` | Browser-resized image resources. |
| `/sdcard/cache/video` | Reserved MP4/video cache path. |
| `/sdcard/boot` | Boot image path, default `/sdcard/boot/boot.jpg`. |
| `/sdcard/fonts` | External font files. |

The current build targets FAT/FAT32. SDHC/SDXC cards can be used if formatted
for this filesystem; exFAT cards should be formatted from the web UI before use.

## Camera

If you use the Freenove ESP32-S3 EYE camera connector from the bundled examples,
no hand wiring should be needed. For an external OV-series DVP camera, the
firmware pin map is:

| Camera signal | ESP32-S3 GPIO |
| --- | --- |
| `XCLK` | GPIO 15 |
| `SDA` / `SIOD` | GPIO 4 |
| `SCL` / `SIOC` | GPIO 5 |
| `D0` | GPIO 11 |
| `D1` | GPIO 9 |
| `D2` | GPIO 8 |
| `D3` | GPIO 10 |
| `D4` | GPIO 12 |
| `D5` | GPIO 18 |
| `D6` | GPIO 17 |
| `D7` | GPIO 16 |
| `VSYNC` | GPIO 6 |
| `HREF` / `HSYNC` | GPIO 7 |
| `PCLK` | GPIO 13 |
| `PWDN` | Not connected |
| `RESET` | Not connected |

Bring-up: click `抓拍`. The browser should open a JPEG from `/api/camera.jpg`.
Without PSRAM the firmware captures to internal RAM, so very high resolutions
are intentionally not used.

## Recommended Bring-Up Order

1. Boot with no external modules attached. Confirm the AP and web UI work.
2. Wire and test the display with `显示测试`.
3. Wire and test touch with `读取触摸`.
4. Wire and test PN532 with `读取 NFC`.
5. Wire and test TF card with `挂载 TF`.
6. Test the camera last with `抓拍`.

If a diagnostic returns `ESP_ERR_TIMEOUT`, check power, GND, signal wiring and
module mode first. If a shared I2C device fails, temporarily disconnect the
other I2C device so only one module is on GPIO 35/36.
