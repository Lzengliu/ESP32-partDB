# ESP32-S3 Part-DB Terminal Wiring and Bring-Up

This wiring matches `main/board_config.h`. Treat it as the current firmware
contract: if a GPIO changes, update that file and this document together.

## Safety Notes

- Use a common GND between the ESP32-S3 board and every module.
- Use 3.3 V logic. Do not feed 5 V signals into ESP32-S3 GPIOs.
- Power high-current backlights from a rail that can actually supply them. If a
  display module exposes `LED+`/`LEDA` instead of a logic `BL` pin, use the
  module's recommended resistor or driver circuit.
- GPIO 43 is used as PN532 I2C `SDA` in the current build. Firmware logs use
  USB Serial/JTAG instead of the board UART pins.
- GPIO 0 is a boot strap pin. The current prototype temporarily uses it only
  for PN532 reset; do not add buttons or other circuits that can hold it low
  while ESP32-S3 resets.
- GPIO 35, GPIO 36 and GPIO 37 are Octal PSRAM/MSPI lines on the current
  ESP32-S3R8 board. Do not connect touch, NFC or other external modules to
  these pins.
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
| `SDA` | GPIO 3 | Touch I2C data. |
| `SCL` | GPIO 44 / RX | Touch I2C clock; uses the board RX/U0RXD pin. |
| `RST` | GPIO 46 | Touch reset; do not add a pull-down. |
| `INT` | Not connected | The firmware polls FT6336 over I2C. |

The first prototype used GPIO 35/36/37 for `SDA`/`SCL`/`RST`. That wiring
conflicts with Octal PSRAM and must stay disconnected. GPIO 44 is available as
the board `RX`/`U0RXD` pin on the current prototype and is reserved for touch
SCL. GPIO 19/20 are left free for native USB flashing and serial logs, and
GPIO 45 is avoided because it is a flash-voltage
strapping pin.

Do not fly-wire GPIO 26 through GPIO 37 for touch. Those pins are used by the
module flash/Octal PSRAM path on the current ESP32-S3R8 configuration. GPIO 44
is available through the board `RX`/`U0RXD` pad on this prototype and is reserved
for touch SCL.

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
| `SDA` | Board `TX` / GPIO 43 | PN532 I2C data. Logs use USB Serial/JTAG, so UART TX is free. |
| `SCL` | GPIO 41 | PN532 I2C clock. |
| `IRQ` | GPIO 48 | PN532 ready signal. Firmware waits for IRQ low before reading command results. |
| `RSTO` / `RSTPDN` | GPIO 0 / BOOT | Temporary hard reset line for this prototype. Do not hold it low during ESP32 boot; move it to a non-strapping GPIO on the next PCB. |

The firmware expects PN532 I2C address `0x24` at 50 kHz. NFC is intended to stay
online after boot. Touch uses GPIO 3/44(RX) for its own software I2C on the
current prototype. Camera SCCB remains on GPIO 4/5 and must not share the PN532
wiring.

`/api/status.hardware.i2c.nfc_shares_camera_sccb` should be `false`,
`/api/status.hardware.i2c.nfc_shares_touch_i2c` should be `false`,
`nfc_irq_gpio` should be `48`, and `nfc_rst_gpio` should be `0`.
Bring-up: click `读取 NFC` while holding a tag near the antenna. Expected JSON
has `ok: true`, `present: true` and a non-empty `uid`.
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

## Physical Buttons

The firmware already has logical button events for `UP`, `DOWN`, `OK/RETURN`
and `SLEEP/WAKE`, but all four GPIOs remain disabled on the current Freenove
prototype.

There are not four spare, conflict-free external GPIOs left after the current
camera, ILI9488, FT6336 touch, PN532 NFC, TF card and native USB wiring:

| GPIO | Decision | Reason |
| --- | --- | --- |
| GPIO19 / GPIO20 | Avoid | Native USB D+ / D-, needed for flashing and USB serial logs. |
| GPIO35 / GPIO36 / GPIO37 | Do not use | Octal PSRAM/MSPI lines on the current ESP32-S3R8 board. |
| GPIO45 | Avoid | Flash-voltage strapping pin. |
| GPIO0 | Avoid | Boot strapping pin; already a temporary PN532 reset line. |
| GPIO46 | In use | FT6336 touch reset. |
| GPIO48 | In use | PN532 IRQ. |

For the next PCB revision, reserve four non-strapping GPIOs that are not
MSPI/PSRAM, USB, camera DVP, display SPI, touch I2C, PN532 I2C/IRQ/RST or TF
SDMMC. Until then, keep `BOARD_BUTTON_*_GPIO` as `GPIO_NUM_NC`.

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

Bring-up: click `扫码` for the real workflow, or `预览抓图` to fetch a diagnostic
JPEG from `/api/camera.jpg`. Without PSRAM the firmware captures to internal
RAM and can fall back to a smaller preview buffer, so very high resolutions are
intentionally not used.

## Recommended Bring-Up Order

1. Boot with no external modules attached. Confirm the AP and web UI work.
2. Wire and test the display with `显示测试`.
3. Wire and test the camera QR workflow with `扫码`; this verifies camera init
   before NFC is kept online.
4. Wire and test PN532 with `读取 NFC`.
5. Wire and test touch with `读取触摸`.
6. Wire and test TF card with `挂载 TF`.

If a diagnostic returns `ESP_ERR_TIMEOUT`, check power, GND, signal wiring and
module mode first. If touch fails, keep GPIO 35/36/37 disconnected and verify
the panel is wired to GPIO 3/44(RX)/46.
