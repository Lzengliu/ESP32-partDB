# V1.0 to V1.1 Changes

[Chinese Version](CHANGES_V1.0_TO_V1.1.md)

Comparison baseline: the public V1.0 source package stored under `dist/v1.0/source/`.

## Incompatible Changes

| Area | V1.0 | V1.1 |
| --- | --- | --- |
| Application partitions | `factory`, `ota_0`, and `ota_1` are each `0x300000` | Each application partition is `0x480000` |
| Coredump start | `0x920000` | `0xDA0000` |
| `fontfs` | `0x2E0000` | `0x100000` |
| `spiffs` | `0x3E0000` | `0x140000` |
| First upgrade | Existing layout | Must flash the V1.1 merged image at `0x0` |

V1.0 web OTA replaces only the application image and cannot install the new partition table, so it must not be used for the first upgrade to V1.1.

## Hardware and Reliability

- Enabled 8 MB Octal PSRAM with allocation and capacity diagnostics.
- Moved the touch bus to `GPIO3/GPIO44` to avoid PSRAM/MSPI pins.
- Moved PN532 to dedicated hardware I2C on `GPIO43/GPIO41` and added IRQ, recovery, and background service support.
- Changed flash frequency to 40 MHz and moved logs to USB Serial/JTAG.
- Added peripheral arbitration to reduce conflicts between camera, NFC, and other long-running operations.

## UI and Resources

- Redesigned Home, Search Results, Detail, Shortcuts, Info, and Settings pages.
- Added automatic screen sleep, boot image, background/lock resource management, and additional diagnostic states.
- Added 8 px and 12 px bitmap fonts while retaining the 16 px font for denser Chinese layouts.
- Search results can open details directly by Part ID and avoid a duplicate exact query.

## Part-DB and Stock

- Added search/detail caching and cache-source status.
- Extended routing for Part, Lot, IPN, barcode, and NFC content.
- Improved detail, stock-lot, and binary diagnostic upload paths.

## Camera and QR Scanning

- Camera frames use PSRAM; scanning uses SVGA grayscale frames.
- Added fast local ZXing-C++ decoding with quirc fallback.
- Added adaptive exposure warm-up and a conditional second scan.
- Removed nonfunctional autofocus controls; the installed module uses manual focus only.
- The camera now initializes on demand and normally sleeps after use; an abnormal web status before preview/scan warm-up is expected.

## NFC

- Added a dedicated PN532 I2C bus, bus recovery, firmware detection, and cached status.
- Added background polling, NDEF read/write/clear, and Part-DB routing.
- Documented GPIO0 as a temporary recovery reset pin for the prototype only.

## Web, OTA, and Maintenance

- Web status now includes PSRAM, NFC, camera, scanner, UI, author, and repository data.
- Added TF file/resource management, camera diagnostics, and more complete configuration controls.
- OTA now validates empty and oversized images and confirms a healthy image after boot, fixing rollback of an unconfirmed update.
- Failed allocation, receive, or write operations remove incomplete upload files.
- A missing default device secret is generated and persisted; AP logs no longer print the password.
- Increased HTTP socket capacity, enabled idle-connection LRU cleanup, and serialized Part-DB requests to prevent long-running web sessions from rejecting new connections.

## Release Engineering

- Updated the application version to `1.1.0`.
- Added the author, 灵异大队长, and the public repository URL throughout the project.
- Completed notices for ZXing-C++, quirc, GNU Unifont, JiangCheng XieHei 200W, and the OFL.
- Added a repeatable packaging script, complete firmware and source archives, and SHA-256 verification.
- Added Chinese and English README, feature, known-issue, release, and version-comparison documentation.
