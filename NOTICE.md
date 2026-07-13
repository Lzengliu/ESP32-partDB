# Notices

ESP32-partDB / ESP32 Part-DB Terminal

Copyright 2026 灵异大队长.

Project repository: https://github.com/Lzengliu/ESP32-partDB

The original project source code in this repository is licensed under the
Apache License, Version 2.0, except where a file or this notice states
otherwise.

## Third-party software

- ESP-IDF and its components: Espressif Systems and contributors; individual
  component licenses apply, with the framework primarily under Apache-2.0.
- `espressif/esp32-camera` 2.1.7: Espressif Systems; Apache-2.0.
- `espressif/esp_jpeg` 1.3.1: Espressif Systems; Apache-2.0.
- `espressif/quirc` 1.2.0 and upstream quirc: Daniel Beer; ISC license.
- `third_party/zxing-cpp`: ZXing-C++ contributors; Apache-2.0. The V1.1
  firmware links a reader-only QR Code subset from upstream commit
  `6c2961d2a9ea4bc4e4ae8f37b1497299f04dd861`. Its license is retained in
  `third_party/zxing-cpp/LICENSE`.

## Font data

Generated bitmap data in `firmware/main/calendar_font_data.h` is derived from
GNU Unifont 16.0.02. Generated bitmap data in
`firmware/main/ui_font_sizes_data.h` is derived from JiangChengXieHei 200W
(江城斜黑体 200W, Version 2.0, author Liu Peng / 刘鹏) with GNU Unifont fallback
glyphs. These font data are distributed under the SIL Open Font License 1.1,
not the project's Apache-2.0 license. See `docs/licenses/OFL-1.1.txt`.

The original TTF and GNU Unifont source files used by the local generator are
not included in the public source package. The generated headers retain their
source attribution.

## External projects and local material

Part-DB is an external project. This firmware communicates with it through its
HTTP API and does not include the Part-DB server source. Local hardware
archives, vendor examples, historical builds and private development material
are not part of the public source package.

## Release binaries

V1.1 binaries are build artifacts generated with ESP-IDF v5.5.2. They include
linked third-party software and generated font data. Distribute this NOTICE,
the root LICENSE, the OFL text and the V1.1 release notes with binary packages.
