# Optimization Roadmap

## Stable Baseline

- Display driver: ILI9488 4SPI, RGB666 pixel writes, 20 MHz SPI.
- Default display config: 320x480 portrait with 180 degree flip enabled.
- Web display settings expose driver, size, orientation, flip, and brightness.
- Boot now initializes the display early and shows a lightweight boot UI/progress
  while WiFi, HTTP, NFC, and diagnostics continue to initialize.
- Source backup: `backups/firmware_source_display-flip-font-assets-nfc-stubs_20260710_0312.tar.gz`.
- Firmware backup: `firmware/build/esp32_partdb_terminal_display-flip-font-assets-nfc-stubs_20260710_0312.bin`.

## Font Assets

Source fonts are stored outside the firmware under `字体/` during development
and should be uploaded to TF/SD card for runtime use.

Do not embed full TTF/OTF files directly in the application image. The current
font folder is about 63 MB, while raw font embedding would break OTA size
headroom and waste flash.

Current firmware embeds a full 16x16 bitmap Chinese glyph table as the fallback
font for boot, error, and no-card states. After adding the table, the firmware
image is about 1.9 MB and still has about 38% free space in each 3 MB OTA slot.
Normal UI rendering should later prefer the user-selected font stored on TF/SD
when that renderer is available.

Preferred approach:

- Keep TTF/OTF files as source assets.
- Web management can upload selectable font files to `/sdcard/fonts`; firmware
  stores only the selected `font_path`.
- Web management shows boot image and font directories as fixed paths. Users
  can delete font files only after explicitly selecting one and confirming.
- Add an offline converter that can regenerate size-specific bitmap glyph
  tables from selected source fonts.
- Generate reduced glyph ranges later if OTA headroom becomes tight.
- Store large optional font packs on TF/SD when the card is present.
- Keep the built-in 16x16 bitmap font in firmware for boot, error, and no-card
  states.

Deferred validation task:

- Verify that selecting a TF/SD font can load and render reliably across reboot,
  missing-card fallback, and memory-pressure cases.

## Cache Policy

All non-critical caches should prefer TF/SD first:

1. If TF/SD is mounted, cache Part-DB responses, downloaded images, temporary
   previews, and decoded intermediate files under `/sdcard/cache`.
2. If TF/SD is not mounted, use bounded RAM cache only.
3. RAM cache must have explicit size limits and must be safe to discard.
4. Upload-controlled assets are not general cache and remain limited to:
   `/sdcard/backgrounds`, `/sdcard/boot/animation`, and `/sdcard/lockscreen`.

Implemented baseline:

- Part-DB GET responses are cached under `/sdcard/cache/partdb`.
- If TF/SD is unavailable, the Part-DB client keeps one bounded RAM fallback
  response.
- Part-DB status reports the last response source: network, SD cache, or RAM
  cache.
- Part-DB response buffers are currently bounded at 8 KB for Web/API testing and
  cache writes.

## Part-DB And NFC

Part-DB has no native NFC object. The planned NFC object should be the Part-DB
identifier already understood by Part-DB:

- Use Part-DB internal barcode content for local objects: `P0001` for part 1,
  `L0001` for lot 1, and `S0001` for storage location 1.
- Also support Part IPN as a user-managed part-level identifier. IPN is unique
  in Part-DB and can be looked up through `/api/parts?ipn=...`.
- Also support PartLot `user_barcode` for package/lot-level labels. If a lot is
  missing `user_barcode`, the hidden write API can dry-run or PATCH it before
  writing NFC.
- Read NFC tag -> obtain NDEF text payload -> parse internal barcode first,
  then PartLot `user_barcode`, then Part IPN.
- Write NFC tag -> generate the internal barcode from `part_id`, `partdb_path`,
  or an existing barcode, or write Part IPN when `payload_kind:"ipn"` is used.
  The write only touches Part-DB or NFC when `commit:true`.
- Part lookup uses API Platform `PropertyFilter` to fetch a small summary
  instead of the full detail payload.
- Hidden `/api/nfc/partdb/read` and `/api/nfc/partdb/write` are implemented but
  intentionally not shown in the Web UI. The NFC background poller is suspended
  briefly during manual NFC read/write requests so NFC stays normally online
  without racing explicit operations.

## Physical Buttons

Reserve a small input service instead of wiring button behavior into display or
HTTP modules.

Expected actions:

- Page previous / move up
- Page next / move down
- Short press confirm
- Double press back
- Long press home
- Screen sleep / wake

Each button should be configured by GPIO and active level. Debounce, click
classification, and event dispatch should live in a dedicated button module.

Implemented baseline:

- `button_input` service is isolated from display/HTTP logic.
- GPIOs are reserved in `board_config.h` and default to `GPIO_NUM_NC`.
- Hidden `/api/buttons/status` reports configured count, pressed states, and
  last classified event.

## Device Screen UI

The first device-side UI layer is implemented as a dedicated `device_ui`
module, separate from display, HTTP, NFC, and Part-DB clients.

Implemented baseline:

- Starts after boot initialization and draws the main device screen.
- Provides HOME, PARTDB, NFC, and HARDWARE pages.
- Uses the built-in 16x16 bitmap font renderer for ASCII and Chinese text;
  selected SD font files remain reserved for later large-size or vector font
  rendering.
- Uses `display_ili9488_draw_bitmap565()` for text/bitmap writes instead of
  thousands of tiny rectangle updates.
- Polls `button_input` events and maps them to page navigation:
  up/back -> previous page, down/confirm -> next page, long OK -> home,
  wake button -> sleep/wake.
- Hidden `/api/ui/status` and `/api/ui/page` allow UI state checks and page
  switching while physical GPIOs are still unassigned.

## Web Resource Browser

The file browser is a diagnostic tool and should not be a normal user workflow.

Preferred UI direction:

- Hide the raw file browser by default.
- Keep the controlled asset preview panels visible.
- Keep delete buttons only in the three managed asset directories.
- Managed resource cards expose explicit selection for screen background, boot
  animation, and lockscreen background; path entry remains hidden from users.
- If the browser remains available, directory entries should show real names,
  not only `DIR`.
