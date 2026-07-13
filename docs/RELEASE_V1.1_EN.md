# ESP32 Part-DB Terminal V1.1 Release Notes

[Chinese Version](RELEASE_V1.1.md)

Release date: 2026-07-14

- Author: 灵异大队长
- Repository: https://github.com/Lzengliu/ESP32-partDB

## Release Scope

V1.1 is a major feature and stability update. Its main changes include the PSRAM and partition layout, a redesigned device UI, a background PN532 service, local ZXing-C++ QR decoding, TF card resource management, hardware diagnostics, and OTA rollback confirmation.

See `CHANGES_V1.0_TO_V1.1_EN.md` for the complete comparison and `KNOWN_ISSUES_EN.md` for current limitations.

## First Boot and Management AP

On a fresh device or while WiFi is not configured, connect to the default AP:

- SSID: `PartDB-Terminal`
- Password: `partdb1234`
- Web management URL: `http://192.168.4.1/`

Open the URL in a browser after connecting, then configure WiFi, Part-DB, and other device options.

## Build Environment

- ESP-IDF: v5.5.2
- Target: ESP32-S3
- Flash: 16 MB, DIO, 40 MHz
- PSRAM: 8 MB Octal
- Application version: `1.1.0`
- Partition table: `firmware/partitions.csv`

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py merge-bin
```

## V1.0 Upgrade Warning

V1.1 changes the partition table. The first upgrade from V1.0 must write the complete merged image, including the new partition table, at `0x0`:

```sh
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  0x0 esp32_partdb_terminal_v1.1_merged.bin
```

Do not upload the V1.1 OTA application image through the V1.0 web management page. Web OTA is suitable only after the V1.1 partition layout has been installed.

## Firmware Files

| File | Purpose | Offset |
| --- | --- | --- |
| `esp32_partdb_terminal_v1.1_merged.bin` | Complete first flash or partition-layout upgrade | `0x0` |
| `bootloader.bin` | Multi-file flashing | `0x0` |
| `partition-table.bin` | V1.1 partition table | `0x8000` |
| `ota_data_initial.bin` | Initial OTA state | `0xf000` |
| `esp32_partdb_terminal_v1.1_ota.bin` | Application or same-layout OTA image | `0x20000` for manual flashing |

Multi-file flashing:

```sh
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 40m \
  0x0 bootloader.bin \
  0x8000 partition-table.bin \
  0xf000 ota_data_initial.bin \
  0x20000 esp32_partdb_terminal_v1.1_ota.bin
```

## Release Assets

- `esp32_partdb_terminal_v1.1_firmware.zip`
- `esp32_partdb_terminal_v1.1_source.zip`
- `esp32_partdb_terminal_v1.1_merged.bin`
- `esp32_partdb_terminal_v1.1_ota.bin`
- `SHA256SUMS`

Run this command in the asset directory after downloading:

```sh
shasum -a 256 -c SHA256SUMS
```

## Known Limitations

- The camera module has no autofocus; rotate the lens manually before QR scanning.
- The camera wakes on demand. Before Preview or Scan has warmed it up, an abnormal Camera status in the web interface is expected.
- The web management page and APIs have no authentication and should be used only on a trusted local network.
- Uploaded fonts are not connected to runtime rasterization yet.
- GIF/WebP boot animations are not played frame by frame yet.
- Long-text scrolling and explicit multiple-lot selection still need improvement.

## Release Validation

- The complete merged image was written and verified at `0x0`; the device booted with the new V1.1 partition table.
- `/api/status` returned version `1.1.0`, the author, the repository, and normal peripheral states.
- ILI9488, FT6336, 8 MB PSRAM, TF card, PN532 firmware 1.6, WiFi, HTTP, and the device UI started successfully on hardware.
- After manual focus, OV5640 plus ZXing-C++ decoded an 800 x 600 QR frame successfully.
- Twelve concurrent status requests all returned HTTP 200 without another socket error 23.
- Web OTA wrote `ota_0` successfully and logged `OTA image confirmed valid`; another reboot still selected `ota_0` without rollback.

## License

Project-owned code uses the Apache License 2.0. The release package retains third-party software and OFL font-data notices in `NOTICE.md`, `THIRD_PARTY_CODE.md`, and `OFL-1.1.txt`.
