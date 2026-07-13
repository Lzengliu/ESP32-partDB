# ESP32-partDB V1.0 Project Brief

ESP32-partDB is a hardware terminal built around the Part-DB electronic parts inventory system. It is not the Part-DB server itself. It runs on an ESP32-S3 device and is intended for on-site part lookup, stock information display, QR scanning, stock in/out operations, NFC tag workflows, and terminal resource management.

Part-DB upstream:

- GitHub: https://github.com/Part-DB/Part-DB-server
- Documentation: https://docs.part-db.de/

## Framework and Languages

| Area | Technology / Language | Notes |
| --- | --- | --- |
| Firmware | C, ESP-IDF | Hardware drivers, UI, backend APIs, and Part-DB communication are implemented in the ESP-IDF project |
| Device UI | C | Pages, buttons, soft keyboard, and dialogs are drawn directly to the ILI9488 framebuffer |
| Backend service | C, ESP-IDF HTTP Server | The ESP32 hosts its own web UI and JSON APIs |
| Backend web page | HTML/CSS/JavaScript | Embedded as strings in `http_server.c`; no external web server files are required |
| Part-DB server | PHP/Symfony | External system; this firmware talks to it through the Part-DB HTTP API |
| QR decoding | C/C++, ZXing-C++ | Camera frames are decoded locally on the ESP32 |

## Boot Flow

1. Initialize NVS and load local configuration.
2. Initialize the display and show boot progress.
3. Initialize the Part-DB HTTP client.
4. Start WiFi STA/AP.
5. Start the ESP32 local web backend.
6. Run hardware diagnostics.
7. Start the NFC polling service.
8. Start the touch-screen device UI.

Entry point: `firmware/main/app_main.c`

## Hardware Runtime Logic

### ESP32-S3 Controller

The ESP32-S3 handles all local runtime work:

- WiFi connection.
- Local web backend.
- Display and touch control.
- Part-DB API access.
- TF card resource management.
- Camera-based QR scanning.
- PN532 NFC read/write workflow.

### ILI9488 Display

The display is driven over SPI. The firmware draws directly to the screen and does not use LVGL.

Runtime logic:

- Shows a boot progress UI.
- Displays Home, Search Results, Detail, Shortcuts, Info, and Settings pages.
- The Detail page displays Part-DB part data and stock operations.

### Built-in Font

V1.0 only supports the built-in bitmap Chinese font and a small set of built-in symbol glyphs.

Runtime logic:

- UI text is rendered by `firmware/main/ui_font.c`.
- Chinese, English, digits, and common symbols are drawn from firmware-embedded glyph data.
- Additional component-parameter symbols such as `Ω`, `°C`, `℃`, `±`, `≤`, and `≥` are included.
- The web backend can upload font files and save a selected font path, but V1.0 does not parse TTF/OTF/TTC/WOFF/WOFF2 and does not use uploaded fonts for runtime rendering.

In short, V1.0 should be treated as supporting only the built-in Chinese font. External font upload is a reserved configuration feature.

### FT6336 Touch

Touch input is read through hardware I2C.

Runtime logic:

- Periodically read touch coordinates.
- Apply configured rotation, flipping, and coordinate mapping.
- Detect taps and swipes.
- Control page navigation, search input, soft keyboard, Detail page switching, and button actions.

### PN532 NFC

NFC uses a PN532 module on a dedicated hardware I2C bus: `SDA=GPIO43(TX)` and `SCL=GPIO41`. FT6336 touch uses a low-speed software I2C bus on GPIO3/GPIO44(RX), with GPIO46 as reset and INT left unconnected on the current prototype. Camera SCCB remains on GPIO4/GPIO5, so NFC and camera do not share pins.

Runtime logic:

- The NFC service periodically polls for tags.
- Read text is treated as a Part-DB object code, barcode, IPN, or link and can enter the Detail page.
- NFC writing must be confirmed through a modal dialog on the Detail page before an NDEF text payload is written.

V1.0 known issue: on the current hardware, PN532 may still report `ESP_ERR_NOT_FOUND`; PN532 I2C mode, wiring, pull-ups, and power-up timing still need verification.

### TF Card

The TF card uses 1-bit SDMMC.

Runtime logic:

- Mounted at `/sdcard`.
- Stores cache, background images, boot animations, lock-screen images, and uploaded font files.
- The web backend can upload, browse, download, and delete files.

### Camera

The camera uses the ESP32-S3 EYE / Freenove example pin map.

Runtime logic:

- The web backend can capture JPEG frames.
- The device UI or backend can trigger QR scanning.
- QR results can enter Part-DB lookup and the Detail page.

### Physical Buttons

Physical buttons are reserved in V1.0 but no GPIOs are configured. The primary interaction method is the touch screen.

## IO Pin Map

### ILI9488 SPI Display

| Signal | GPIO |
| --- | --- |
| SCLK | GPIO21 |
| MOSI | GPIO47 |
| MISO | NC |
| CS | GPIO14 |
| DC | GPIO2 |
| RST | GPIO1 |
| BL | GPIO42 |

### FT6336 Touch

| Signal | GPIO / Address |
| --- | --- |
| I2C SDA | GPIO3 |
| I2C SCL | GPIO44/RX |
| I2C address | `0x38` |
| RST | GPIO46 |
| INT | NC |

### PN532 NFC

| Signal | GPIO / Address |
| --- | --- |
| SPI SCLK | GPIO21 |
| SPI MOSI | GPIO47 |
| SPI MISO | GPIO48 |
| SPI CS | GPIO43 |
| SPI clock | 100 kHz during current bring-up |
| SPI bus | Shared with the ILI9488 SPI bus, with dedicated CS; not shared with camera SCCB |

### TF Card SDMMC 1-bit

| Signal | GPIO |
| --- | --- |
| CMD | GPIO38 |
| CLK | GPIO39 |
| D0 | GPIO40 |
| D3 | NC |

### Camera

| Signal | GPIO |
| --- | --- |
| XCLK | GPIO15 |
| SIOD | GPIO4 |
| SIOC | GPIO5 |
| D0 | GPIO11 |
| D1 | GPIO9 |
| D2 | GPIO8 |
| D3 | GPIO10 |
| D4 | GPIO12 |
| D5 | GPIO18 |
| D6 | GPIO17 |
| D7 | GPIO16 |
| VSYNC | GPIO6 |
| HREF | GPIO7 |
| PCLK | GPIO13 |
| PWDN | NC |
| RESET | NC |

### Physical Buttons

| Function | GPIO |
| --- | --- |
| UP | NC |
| DOWN | NC |
| OK | NC |
| WAKE | NC |

## Web Backend Capabilities

The ESP32 hosts its own backend at the device IP address, for example:

```text
http://device-ip/
```

Backend features:

- View WiFi, Part-DB, TF card, display, touch, NFC, camera, and UI status.
- Configure device name.
- Configure WiFi/AP.
- Configure Part-DB URL and API token.
- Test Part-DB API access.
- Configure display size, orientation, flip, and brightness.
- Configure touch rotation, flipping, and coordinate range.
- Run hardware diagnostics.
- View touch and button status.
- Switch the device UI page.
- Read current NFC status.
- Run NFC read/write requests for Part-DB objects.
- View TF card capacity and mount status.
- Prepare TF card directories.
- Format the TF card.
- Upload, browse, download, and delete TF card files.
- Upload and select font files.
- Upload, select, and delete screen background images.
- Upload, select, and delete boot animations.
- Upload, select, and delete lock-screen images.
- Capture camera JPEG.
- Trigger camera QR scanning.
- Upload OTA firmware and reboot.

Main backend APIs are registered in `firmware/main/http_server.c`.

## Part-DB Features

Device side:

- Fuzzy part search.
- Search result list.
- Direct Detail page entry from search results.
- Display name, IPN, category, manufacturer, MPN, footprint, location, barcode, stock amount, lots, description, comment, and parameters.
- Quantity input.
- Stock in/out write-back to Part-DB.
- NFC write confirmation dialog.

Backend side:

- Save Part-DB base URL.
- Save API token.
- Send Part-DB test requests.
- Proxy Part-DB API queries through `/api/partdb/get`.

## V1.0 Limitations

- NFC hardware communication is still unstable and may remain offline.
- V1.0 only supports the built-in Chinese font; uploaded TTF/OTF/TTC/WOFF/WOFF2 files are saved but not parsed for runtime rendering.
- Long Detail page text may still be truncated by the small screen.
- Multi-lot part stock operations do not yet have a full lot selection page.
