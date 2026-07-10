# ESP32 Part-DB Terminal Firmware

Current public release target: **V1.0**.

This is the ESP-IDF firmware workspace for the ESP32-S3 camera terminal.

Current scope:

- WiFi AP/STA bootstrap and web configuration portal.
- Part-DB API endpoint/token storage.
- OTA upload endpoint.
- Device name, display brightness, boot image path and font directory storage.
- ILI9488 SPI display driver bring-up.
- FT6336 touch controller bring-up.
- PN532 NFC bring-up on a dedicated software I2C bus, physically isolated from
  the FT6336 touch controller bus.
- ESP32 camera manager using the Freenove ESP32-S3 EYE pin map.
- SDMMC one-bit TF card mount, browser file preview, explicit format, cache
  directory initialization and resource upload for fonts, boot images,
  screensavers and cache data.

The first hardware pass intentionally keeps all board pins centralized in
`main/board_config.h`. Treat that file as the only place to change GPIOs while
the actual board wiring is being verified.

Current NFC wiring shares hardware I2C0 with touch: `GPIO35=SDA`,
`GPIO36=SCL`, PN532 7-bit address `0x24`. Touch uses the same bus with FT6336
address `0x38`. Avoid `GPIO3/GPIO46` for NFC on this board family because those
pins are commonly wired to side buttons, and `GPIO46` can be held low.

Hardware wiring and bring-up steps are documented in
[`docs/wiring-and-bringup.md`](docs/wiring-and-bringup.md). GitHub release and
open-source packaging notes are documented in the repository-level `docs/`
directory.

## Build

ESP-IDF v5.5.x is the target toolchain.

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
```

The camera driver is declared as a managed component dependency in
`main/idf_component.yml`, so the first build requires network access unless the
component is already cached locally.

## Web Management Notes

- The resource page shows TF card total/free space and uses `/sdcard/cache`,
  `/sdcard/cache/images`, `/sdcard/cache/video`, `/sdcard/boot` and
  `/sdcard/fonts` as the unified storage layout.
- The TF format button intentionally requires a manual click and formats the
  whole card. The card is treated as replaceable cache/media storage, not as the
  only copy of important data.
- Current firmware is built for FAT/FAT32. Cards sold as 64 GB or larger often
  ship as exFAT; use the format button to convert them for this firmware.
- Image uploads are resized in the browser to the 480 x 320 screen target before
  upload. MP4 upload is reserved and currently stores the original file.
- The Part-DB page expects the saved base URL to point at the Part-DB API, for
  example `http://partdb.local/api`, and uses the saved API token as a Bearer
  token.

## Flash

Use `idf.py flash` when possible. It writes the bootloader, partition table,
OTA metadata and application to the correct offsets:

```sh
cd firmware
idf.py -p PORT flash monitor
```

If you flash manually with esptool, do not write
`build/esp32_partdb_terminal.bin` to `0x0`. That file is the application image
and must be written to `0x20000` with the matching bootloader and partition
table:

```sh
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xf000 build/ota_data_initial.bin \
  0x20000 build/esp32_partdb_terminal.bin
```

For flashing tools that accept only one file, generate and flash the merged
image at offset `0x0`:

```sh
idf.py merge-bin
python -m esptool --chip esp32s3 -b 460800 write_flash 0x0 build/merged-binary.bin
```
