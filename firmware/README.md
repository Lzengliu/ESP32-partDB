# ESP32 Part-DB Terminal Firmware

Current stable release: **V1.1**

- Author: 灵异大队长
- Repository: https://github.com/Lzengliu/ESP32-partDB

This directory is the ESP-IDF project for the ESP32-S3 terminal. Board GPIOs
are centralized in `main/board_config.h`; do not move touch signals back to
GPIO35/36/37 because those pins are used by Octal PSRAM/MSPI on this board.

Current prototype wiring:

- FT6336 software I2C: `SDA=GPIO3`, `SCL=GPIO44`, `RST=GPIO46`
- PN532 hardware I2C: `SDA=GPIO43`, `SCL=GPIO41`, `IRQ=GPIO48`, temporary
  recovery reset on `GPIO0`
- Camera SCCB: `GPIO4/GPIO5`
- Logs: USB Serial/JTAG

See `docs/wiring-and-bringup.md` for the complete wiring table.

## Build

ESP-IDF v5.5.2 is the verified toolchain.

```sh
idf.py set-target esp32s3
idf.py build
idf.py merge-bin
```

The first build may download the locked managed components. ZXing-C++ is
vendored at `../third_party/zxing-cpp` and linked by
`components/zxing_qr/CMakeLists.txt`.

## Flash

Use `idf.py flash` when possible:

```sh
idf.py -p PORT flash monitor
```

The V1.1 application image belongs at `0x20000`; it is not a complete image.
Manual multi-file flashing uses 40 MHz flash frequency:

```sh
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 40m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xf000 build/ota_data_initial.bin \
  0x20000 build/esp32_partdb_terminal.bin
```

For one-file flashing, generate a merged image and write it at `0x0`:

```sh
idf.py merge-bin
python -m esptool --chip esp32s3 -b 460800 write_flash \
  0x0 build/merged-binary.bin
```

V1.0 users must install the V1.1 merged image once because the partition table
changed. Web OTA is suitable only after the V1.1 partition layout is present.

## Operational notes

- The management page and APIs are intended for a trusted local network and do
  not currently enforce authentication.
- TF cards must use FAT/FAT32. The format action erases the entire card.
- Uploaded TTF/OTF files are stored and selected, but the runtime renderer still
  uses built-in bitmap fonts.
- The installed camera module has manual optical focus only. Adjust the lens
  physically before QR scanning.
