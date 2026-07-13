# Feature Details

[Chinese Version](FEATURES.md)

Version: V1.11

## Part-DB

- Save the Part-DB base URL and API token through the web management page.
- Perform fuzzy search and read cached results, details, parameters, stock, and lots.
- Update Part-DB stock from the on-device Detail page.
- Route barcode, QR, and NFC content to Part, Lot, IPN, or barcode lookups.

## On-Device UI

- Home, Search Results, Detail, Shortcuts, Info, and Settings pages.
- Structured detail sections with quantity input, stock-in, stock-out, and NFC writing.
- Touch keyboard, brightness setting, automatic screen sleep, and wake handling.
- Built-in 8/12/16 px Chinese, Latin, and common symbol bitmap fonts.

## Web Management

- Inspect WiFi, PSRAM, Part-DB, TF card, display, touch, NFC, camera, scanner, and UI status.
- Save network, Part-DB, display, touch, and resource settings.
- Manage TF card files, backgrounds, boot images, lock images, and font files.
- Run hardware diagnostics, camera preview, local scanning, and OTA updates.
- The Overview and Hardware Diagnostics pages provide a separate AF button; AF runs only after an explicit click.
- Show the author, Lingyi Daduizhang (灵异大队长), and the source repository.

## Camera and QR Scanning

- PSRAM frame buffers and a VGA grayscale fast path; the scanner conditionally upgrades to SVGA only after QR geometry is detected but decoding fails.
- Exposure warm-up can finish early based on measured image brightness.
- Fast ZXing-C++ decoding with quirc fallback; luma buffers, the quirc context, and matching camera configurations are reused so consecutive scans skip redundant warm-up.
- `/api/camera.jpg` provides preview images; `/api/camera/scan` runs local decoding.
- `/api/camera/af` triggers OV5640 AF in manual mode. Startup, preview, and scan paths never run AF automatically.

Fixed-focus modules and modules without a lens actuator still require physical lens adjustment. See [Known Issues](KNOWN_ISSUES_EN.md).

The camera is enabled on demand and sleeps and releases its driver after a short idle interval. Before a preview or scan has woken and warmed up the sensor, Camera may appear abnormal in the web Overview and Hardware Diagnostics pages; this is expected.

## NFC

- PN532 uses a dedicated hardware I2C bus and does not share the touch or camera SCCB bus.
- Background polling, bus recovery, and NDEF text read, write, and clear operations.
- Peripheral arbitration prevents NFC and camera operations from running concurrently.

## TF Card and Resources

- FAT/FAT32 mounting, space status, directory initialization, upload, preview, and deletion.
- Static boot JPEG images can be decoded and displayed during startup.
- External font files can be stored and selected; runtime rendering still uses built-in bitmap fonts.

## OTA and Diagnostics

- 4.5 MiB `factory`, `ota_0`, and `ota_1` application partitions.
- Empty-image and partition-capacity validation before OTA writes.
- Bootloader rollback is enabled; the new image is confirmed only after core services start successfully.
- Status APIs expose firmware version, author, repository, and hardware diagnostics.
- A low-priority runtime guard samples internal RAM and PSRAM, releases scanner caches and idle camera resources under memory pressure, and checks heap integrity every five minutes. It does not hide faults behind periodic reboots.
