#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/spi_master.h"

#define BOARD_DEVICE_NAME             "ESP32-PartDB-Terminal"
#define BOARD_DEVELOPER_NAME          "ESP32 Part-DB Terminal Project"
#define BOARD_SOURCE_REPO_URL         ""
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

/* Keep first-stage boot resilient. Probe optional peripherals from the web UI. */
#define BOARD_INIT_OPTIONAL_HARDWARE_AT_BOOT 0
#define BOARD_CAMERA_PREWARM_AT_BOOT 0
#define BOARD_HARDWARE_DIAG_AT_BOOT 1

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

/* Touch hardware I2C bus. Keep NFC physically isolated from this bus. */
#define BOARD_I2C_PORT                I2C_NUM_0
#define BOARD_I2C_SDA_GPIO            GPIO_NUM_35
#define BOARD_I2C_SCL_GPIO            GPIO_NUM_36
#define BOARD_I2C_HZ                  (100000)

#define BOARD_TOUCH_FT6336_ADDR       0x38
#define BOARD_TOUCH_RST_GPIO          GPIO_NUM_37
#define BOARD_TOUCH_INT_GPIO          GPIO_NUM_41
#define BOARD_TOUCH_RAW_WIDTH         320
#define BOARD_TOUCH_RAW_HEIGHT        480

#define BOARD_NFC_PN532_ADDR          0x24
/* PN532 shares the touch hardware I2C bus.
 * Touch uses address 0x38 and PN532 uses address 0x24, so they can coexist on
 * GPIO35/GPIO36. Avoid GPIO3/GPIO46 here; those pins are used as side-button
 * lines on related ESP32-S3 boards and GPIO46 is easily held low.
 */
#define BOARD_NFC_USE_SOFT_I2C        0
#define BOARD_NFC_SOFT_SDA_GPIO       BOARD_I2C_SDA_GPIO
#define BOARD_NFC_SOFT_SCL_GPIO       BOARD_I2C_SCL_GPIO
#define BOARD_NFC_SOFT_I2C_HZ         (50000)

/* Physical buttons are reserved for the screen UI. Set GPIOs after wiring. */
#define BOARD_BUTTON_UP_GPIO          GPIO_NUM_NC
#define BOARD_BUTTON_DOWN_GPIO        GPIO_NUM_NC
#define BOARD_BUTTON_OK_GPIO          GPIO_NUM_NC
#define BOARD_BUTTON_WAKE_GPIO        GPIO_NUM_NC
#define BOARD_BUTTON_ACTIVE_LEVEL     0
