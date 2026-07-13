# Feature Details

[Chinese Version](FEATURES.md)

Version: V1.1

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
- Show the author, Lingyi Daduizhang (灵异大队长), and the source repository.

## Camera and QR Scanning

- PSRAM frame buffers and SVGA grayscale scan frames.
- Exposure warm-up can finish early based on measured image brightness.
- Fast ZXing-C++ decoding, quirc fallback, and a conditional second-frame retry.
- `/api/camera.jpg` provides preview images; `/api/camera/scan` runs local decoding.

The installed camera module has no autofocus. Rotate the lens manually until the QR edges are sharp. See [Known Issues](KNOWN_ISSUES_EN.md).

The camera is enabled on demand and normally sleeps and releases its driver after an operation. Before a preview or scan has woken and warmed up the sensor, Camera may appear abnormal in the web Overview and Hardware Diagnostics pages; this is expected.

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
