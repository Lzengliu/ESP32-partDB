#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

#define BOARD_DEVICE_NAME             "ESP32-PartDB-Terminal"
#define BOARD_DEVELOPER_NAME          "灵异大队长"
#define BOARD_SOURCE_REPO_URL         "https://github.com/Lzengliu/ESP32-partDB"
#define BOARD_AP_DEFAULT_SSID         "PartDB-Terminal"
#define BOARD_AP_DEFAULT_PASS         "partdb1234"

#define BOARD_SD_CACHE_DIR            "/sdcard/cache"
#define BOARD_SD_PARTDB_CACHE_DIR     "/sdcard/cache/partdb"
#define BOARD_SD_IMAGE_CACHE_DIR      "/sdcard/cache/images"
#define BOARD_SD_VIDEO_CACHE_DIR      "/sdcard/cache/video"
#define BOARD_SD_BOOT_DIR             "/sdcard/boot"
#define BOARD_SD_BOOT_ANIM_DIR        "/sdcard/boot/animation"
#define BOARD_SD_SCREEN_BG_DIR        "/sdcard/backgrounds"
#define BOARD_SD_LOCK_BG_DIR          "/sdcard/lockscreen"
#define BOARD_SD_FONT_DIR             "/sdcard/fonts"

/* Keep first-stage boot resilient while still running an ordered hardware check. */
#define BOARD_INIT_OPTIONAL_HARDWARE_AT_BOOT 0
#define BOARD_CAMERA_PREWARM_AT_BOOT 0
#define BOARD_CAMERA_KEEP_ONLINE      0
#define BOARD_CAMERA_PREVIEW_SWAP_BYTES 1
#define BOARD_CAMERA_PREVIEW_SWAP_RB  0
#define BOARD_CAMERA_BRIGHTNESS       0
#define BOARD_CAMERA_CONTRAST         2
#define BOARD_CAMERA_SATURATION       2
#define BOARD_CAMERA_SHARPNESS        3
#define BOARD_CAMERA_DENOISE          0
#define BOARD_CAMERA_SCAN_BRIGHTNESS  1
#define BOARD_CAMERA_SCAN_CONTRAST    2
#define BOARD_CAMERA_SCAN_SATURATION  0
#define BOARD_CAMERA_SCAN_SHARPNESS   3
#define BOARD_CAMERA_SCAN_DENOISE     0
#define BOARD_CAMERA_SCAN_AE_LEVEL    1
#define BOARD_CAMERA_SCAN_GAINCEILING 4
#define BOARD_CAMERA_SCAN_SETTLE_MS   80
#define BOARD_CAMERA_SCAN_DROP_FRAMES 3
#define BOARD_CAMERA_NFC_QUIET_MS     160
#define BOARD_CAMERA_AF_TIMEOUT_MS    2500
#define BOARD_HARDWARE_DIAG_AT_BOOT 0
#define BOARD_BOOT_HARDWARE_CHECK_AT_BOOT 0
#define BOARD_BOOT_MIN_MS             6000
#define BOARD_NFC_SERVICE_AT_BOOT     1

/* Camera: Freenove ESP32S3_EYE pin map from bundled examples. */
#define BOARD_CAM_PWDN_GPIO           GPIO_NUM_NC
#define BOARD_CAM_RESET_GPIO          GPIO_NUM_NC
#define BOARD_CAM_XCLK_GPIO           GPIO_NUM_15
#define BOARD_CAM_SIOD_GPIO           GPIO_NUM_4
#define BOARD_CAM_SIOC_GPIO           GPIO_NUM_5
#define BOARD_CAM_D0_GPIO             GPIO_NUM_11
#define BOARD_CAM_D1_GPIO             GPIO_NUM_9
#define BOARD_CAM_D2_GPIO             GPIO_NUM_8
#define BOARD_CAM_D3_GPIO             GPIO_NUM_10
#define BOARD_CAM_D4_GPIO             GPIO_NUM_12
#define BOARD_CAM_D5_GPIO             GPIO_NUM_18
#define BOARD_CAM_D6_GPIO             GPIO_NUM_17
#define BOARD_CAM_D7_GPIO             GPIO_NUM_16
#define BOARD_CAM_VSYNC_GPIO          GPIO_NUM_6
#define BOARD_CAM_HREF_GPIO           GPIO_NUM_7
#define BOARD_CAM_PCLK_GPIO           GPIO_NUM_13

/* TF card: one-bit SDMMC from bundled examples. */
#define BOARD_SD_CMD_GPIO             GPIO_NUM_38
#define BOARD_SD_CLK_GPIO             GPIO_NUM_39
#define BOARD_SD_D0_GPIO              GPIO_NUM_40
#define BOARD_SD_D3_GPIO              GPIO_NUM_NC
#define BOARD_SD_MOUNT_POINT          "/sdcard"

/* ILI9488 SPI display. Change only here after final wiring. */
#define BOARD_LCD_SPI_HOST            SPI2_HOST
#define BOARD_LCD_SCLK_GPIO           GPIO_NUM_21
#define BOARD_LCD_MOSI_GPIO           GPIO_NUM_47
#define BOARD_LCD_MISO_GPIO           GPIO_NUM_NC
#define BOARD_LCD_CS_GPIO             GPIO_NUM_14
#define BOARD_LCD_DC_GPIO             GPIO_NUM_2
#define BOARD_LCD_RST_GPIO            GPIO_NUM_1
#define BOARD_LCD_BL_GPIO             GPIO_NUM_42
#define BOARD_LCD_SPI_HZ              (40000000)
#define BOARD_LCD_WIDTH               480
#define BOARD_LCD_HEIGHT              320

/*
 * Hardware I2C is reserved for PN532 so it stays off the LCD SPI bus and does
 * not share the slow software touch bus.
 */
#define BOARD_I2C_PORT                I2C_NUM_0
#define BOARD_I2C_SDA_GPIO            GPIO_NUM_43
#define BOARD_I2C_SCL_GPIO            GPIO_NUM_41
#define BOARD_I2C_HZ                  (50000)

/*
 * FT6336 touch panel on the current hand-wired prototype. GPIO35/GPIO36/GPIO37
 * are Octal PSRAM/MSPI pins, so touch uses GPIO3 with GPIO44/RX for I2C and
 * GPIO46 for reset.
 */
#define BOARD_TOUCH_FT6336_ADDR       0x38
#define BOARD_TOUCH_SDA_GPIO          GPIO_NUM_3
#define BOARD_TOUCH_SCL_GPIO          GPIO_NUM_44
#define BOARD_TOUCH_USE_SOFT_I2C      1
#define BOARD_TOUCH_SOFT_I2C_HZ       (20000)
#define BOARD_TOUCH_RST_GPIO          GPIO_NUM_46
#define BOARD_TOUCH_INT_GPIO          GPIO_NUM_NC
#define BOARD_TOUCH_RAW_WIDTH         320
#define BOARD_TOUCH_RAW_HEIGHT        480

#define BOARD_NFC_PN532_ADDR          0x24
#define BOARD_NFC_BACKEND_HARD_I2C    0
#define BOARD_NFC_BACKEND_SOFT_I2C    1
#define BOARD_NFC_BACKEND_UART_HSU    2
#define BOARD_NFC_BACKEND_SPI         3
/*
 * PN532 uses a dedicated hardware I2C pair. The earlier SPI bring-up shared
 * the LCD SPI bus and stayed offline on this prototype, so keep NFC isolated
 * from both display SPI and touch software I2C.
 */
#define BOARD_NFC_ENABLED             1
#define BOARD_NFC_BACKEND             BOARD_NFC_BACKEND_HARD_I2C
/*
 * PN532 uses IRQ-driven ready waits. ESP32 still initiates PN532 commands over
 * I2C, but it does not poll I2C status bytes while PN532 is busy; PN532 pulls
 * IRQ low when a response is ready.
 */
#define BOARD_NFC_BACKGROUND_POLL     1
#define BOARD_NFC_USE_SOFT_I2C        (BOARD_NFC_BACKEND == BOARD_NFC_BACKEND_SOFT_I2C)
#define BOARD_NFC_USE_UART_HSU        (BOARD_NFC_BACKEND == BOARD_NFC_BACKEND_UART_HSU)
#define BOARD_NFC_USE_SPI             (BOARD_NFC_BACKEND == BOARD_NFC_BACKEND_SPI)
#define BOARD_NFC_SDA_GPIO            BOARD_I2C_SDA_GPIO
#define BOARD_NFC_SCL_GPIO            BOARD_I2C_SCL_GPIO
#define BOARD_NFC_SOFT_SDA_GPIO       BOARD_NFC_SDA_GPIO
#define BOARD_NFC_SOFT_SCL_GPIO       BOARD_NFC_SCL_GPIO
#define BOARD_NFC_SOFT_I2C_HZ         (50000)
#define BOARD_NFC_UART_PORT           UART_NUM_1
#define BOARD_NFC_UART_TX_GPIO        GPIO_NUM_NC
#define BOARD_NFC_UART_RX_GPIO        GPIO_NUM_NC
#define BOARD_NFC_UART_BAUD           (115200)
#define BOARD_NFC_SPI_HOST            BOARD_LCD_SPI_HOST
#define BOARD_NFC_SPI_SCLK_GPIO       BOARD_LCD_SCLK_GPIO
#define BOARD_NFC_SPI_MOSI_GPIO       BOARD_LCD_MOSI_GPIO
#define BOARD_NFC_SPI_MISO_GPIO       BOARD_LCD_MISO_GPIO
#define BOARD_NFC_SPI_CS_GPIO         GPIO_NUM_43
#define BOARD_NFC_SPI_HZ              (100000)
#define BOARD_NFC_IRQ_GPIO            GPIO_NUM_48
/*
 * Prototype PN532 hard reset. GPIO0 is an ESP32-S3 BOOT strapping pin, so the
 * module must not pull it low during ESP32 reset. Firmware uses it only after
 * explicit/manual NFC I/O and fault recovery to release a PN532 module that is
 * holding SCL low. Move this to a non-strapping GPIO on the next PCB revision.
 */
#define BOARD_NFC_RST_GPIO            GPIO_NUM_0
#define BOARD_NFC_SHARES_CAMERA_SCCB  (BOARD_NFC_SDA_GPIO != GPIO_NUM_NC && \
                                        BOARD_NFC_SCL_GPIO != GPIO_NUM_NC && \
                                        BOARD_NFC_SDA_GPIO == BOARD_CAM_SIOD_GPIO && \
                                        BOARD_NFC_SCL_GPIO == BOARD_CAM_SIOC_GPIO)
#define BOARD_NFC_SHARES_TOUCH_I2C    (BOARD_NFC_SDA_GPIO != GPIO_NUM_NC && \
                                        BOARD_NFC_SCL_GPIO != GPIO_NUM_NC && \
                                        BOARD_NFC_SDA_GPIO == BOARD_TOUCH_SDA_GPIO && \
                                        BOARD_NFC_SCL_GPIO == BOARD_TOUCH_SCL_GPIO)

/*
 * Physical buttons are reserved for the screen UI. Keep them disabled until
 * the final wiring is chosen; do not reuse NFC, touch, LCD, camera or SD pins.
 * Logical mapping: UP, DOWN, OK/RETURN, SLEEP/WAKE.
 *
 * Current Freenove ESP32-S3 board has no four spare conflict-free external IOs
 * after camera/display/touch/NFC/TF wiring:
 * - GPIO19/GPIO20 are native USB D+/D- and kept free for flashing/logging.
 * - GPIO35/GPIO36/GPIO37 are Octal PSRAM/MSPI and must stay unused.
 * - GPIO45 and GPIO0 are strapping pins and should not be used for buttons.
 * Enable these only on the next PCB, or after accepting a specific tradeoff.
 */
#define BOARD_BUTTON_UP_GPIO          GPIO_NUM_NC
#define BOARD_BUTTON_DOWN_GPIO        GPIO_NUM_NC
#define BOARD_BUTTON_OK_GPIO          GPIO_NUM_NC
#define BOARD_BUTTON_WAKE_GPIO        GPIO_NUM_NC
#define BOARD_BUTTON_ACTIVE_LEVEL     0
