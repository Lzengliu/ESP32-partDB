# Notices

ESP32 Part-DB Terminal

Copyright 2026 ESP32 Part-DB Terminal Project contributors.

This repository's original project source code is licensed under the Apache
License, Version 2.0, except where a file or third-party notice states
otherwise.

## Third-party and external materials

The following materials are used by or referenced from the project and remain
under their own licenses:

- ESP-IDF and bundled ESP-IDF components: Espressif Systems, mostly Apache-2.0.
- `espressif/esp32-camera`: Espressif Systems, Apache-2.0.
- `espressif/esp_jpeg`: Espressif Systems, Apache-2.0.
- `espressif/quirc` and upstream quirc: Daniel Beer, ISC license.
- Generated glyph data in `firmware/main/calendar_font_data.h`: generated from
  GNU Unifont 16.0.02. Treat this font data as separate from the Apache-2.0
  project code; see `docs/THIRD_PARTY_CODE.md` before publishing.
- Part-DB is an external upstream project and service target. This firmware only
  integrates with Part-DB through its HTTP API. Part-DB server source is not part
  of the V1.0 open-source package.
- Hardware datasheets, vendor examples and local reference archives are not part
  of the public source package unless explicitly copied into a release.

## Release binaries

V1.0 release binaries are build artifacts generated from this source tree and
the ESP-IDF toolchain. They include linked third-party components and generated
font data. Keep this NOTICE file and the release notes together with binary
downloads.
