# V1.11 Known Issues

[Chinese Version](KNOWN_ISSUES.md)

Version: V1.11

## AF Depends on the Complete Lens Assembly

The firmware provides a separate AF button and `/api/camera/af`. AF runs only after an explicit user action and never during startup, preview, or scanning. The driver can identify an OV5640 sensor, but the sensor PID alone cannot prove that the lens assembly contains a voice-coil actuator. A fixed-focus or actuator-free module may report an AF status without physically moving the lens. In that case, rotate the lens manually and keep the QR code large enough, well lit, and stationary.

The camera normally sleeps and releases its driver after a short idle interval. If Preview or Scan has not yet woken and warmed up the sensor, the web Overview or Hardware Diagnostics page may show Camera as abnormal. This is expected and does not indicate a damaged module; starting a preview or scan initializes the camera on demand.

## No Authentication on the Web Management Interface

The web page and `/api/*` management endpoints do not currently enforce authentication. They include configuration changes, TF card formatting, resource deletion, and OTA. Deploy V1.11 only on a trusted local network and do not expose the device directly to the public internet. A later version should add login or device-key verification, CSRF protection, and permission levels for destructive actions.

## External Fonts Are Not Used for Runtime Rendering

The web page can upload, select, and delete TTF/OTF/TTC/WOFF/WOFF2 files, but `ui_font_set_active_path()` currently only stores the selected path. Runtime text uses the built-in 8/12/16 px bitmap fonts and does not parse uploaded fonts.

## Animated Boot Images Are Not Played Frame by Frame

Static JPEG boot images can be displayed. GIF and WebP boot animations can be uploaded and selected, but no frame decoder is integrated into the startup path yet.

## Long Text and Multiple-Lot Workflows

The 320 x 480 display can truncate very long descriptions, notes, or parameter values. When a stock operation cannot determine a specific lot, it is rejected instead of guessing; there is no dedicated lot-selection page yet.

## NFC Depends on Prototype Wiring Quality

The PN532 reports its firmware version and starts successfully on the current prototype. GPIO0 is still used temporarily as a recovery reset pin, and I2C mode switches, pull-ups, power quality, and cable length can affect stability. A production PCB should use a dedicated reset pin that is not a boot strapping pin and should follow `firmware/docs/wiring-and-bringup.md`.

## Limited Automated Test Coverage

Current validation relies mainly on strict compiler warnings, hardware flashing, boot logs, HTTP status and OTA regression checks, and manual UI testing. Part-DB mocks, UI state-machine unit tests, a QR image regression set, and NFC hardware simulation are not yet available.
