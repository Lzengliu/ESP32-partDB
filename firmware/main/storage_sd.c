#include "storage_sd.h"

#include <errno.h>
#include <sys/stat.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage_sd";
static sdmmc_card_t *s_card;
static bool s_mounted;

static const esp_vfs_fat_mount_config_t MOUNT_CONFIG = {
    .format_if_mount_failed = false,
    .max_files = 6,
    .allocation_unit_size = 16 * 1024,
};

static void configure_card_pullups(void)
{
    gpio_set_pull_mode(BOARD_SD_CMD_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(BOARD_SD_D0_GPIO, GPIO_PULLUP_ONLY);
    if (BOARD_SD_D3_GPIO != GPIO_NUM_NC) {
        gpio_set_pull_mode(BOARD_SD_D3_GPIO, GPIO_PULLUP_ONLY);
    }
}

static esp_err_t storage_sd_mount(bool format_if_mount_failed)
{
    if (s_mounted) {
        return ESP_OK;
    }

    configure_card_pullups();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = BOARD_SD_CLK_GPIO;
    slot_config.cmd = BOARD_SD_CMD_GPIO;
    slot_config.d0 = BOARD_SD_D0_GPIO;
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = BOARD_SD_D3_GPIO;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_mount_config_t mount_config = MOUNT_CONFIG;
    mount_config.format_if_mount_failed = format_if_mount_failed;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(BOARD_SD_MOUNT_POINT, &host, &slot_config,
                                            &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "Check 3.3 V power, FAT card format, CMD/D0 pull-ups, and DAT3/CS pull-up");
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD mounted at %s", BOARD_SD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t storage_sd_init(void)
{
    return storage_sd_mount(false);
}

static esp_err_t mkdir_if_missing(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "mkdir %s failed: errno=%d", path, errno);
    return ESP_FAIL;
}

esp_err_t storage_sd_prepare_paths(void)
{
    esp_err_t err = storage_sd_init();
    if (err != ESP_OK) {
        return err;
    }
    if (err == ESP_OK) err = mkdir_if_missing(BOARD_SD_CACHE_DIR);
    if (err == ESP_OK) err = mkdir_if_missing(BOARD_SD_PARTDB_CACHE_DIR);
    if (err == ESP_OK) err = mkdir_if_missing(BOARD_SD_IMAGE_CACHE_DIR);
    if (err == ESP_OK) err = mkdir_if_missing(BOARD_SD_VIDEO_CACHE_DIR);
    if (err == ESP_OK) err = mkdir_if_missing(BOARD_SD_BOOT_DIR);
    if (err == ESP_OK) err = mkdir_if_missing(BOARD_SD_BOOT_ANIM_DIR);
    if (err == ESP_OK) err = mkdir_if_missing(BOARD_SD_SCREEN_BG_DIR);
    if (err == ESP_OK) err = mkdir_if_missing(BOARD_SD_LOCK_BG_DIR);
    if (err == ESP_OK) err = mkdir_if_missing(BOARD_SD_FONT_DIR);
    return err;
}

esp_err_t storage_sd_format_and_prepare(void)
{
    esp_err_t err = storage_sd_mount(true);
    if (err != ESP_OK) {
        return err;
    }
    esp_vfs_fat_mount_config_t format_config = MOUNT_CONFIG;
    err = esp_vfs_fat_sdcard_format_cfg(BOARD_SD_MOUNT_POINT, s_card, &format_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD format failed: %s", esp_err_to_name(err));
        return err;
    }
    return storage_sd_prepare_paths();
}

storage_sd_status_t storage_sd_get_status(void)
{
    storage_sd_status_t st = {
        .mounted = s_mounted,
        .card_size_bytes = 0,
        .total_bytes = 0,
        .free_bytes = 0,
    };
    if (s_mounted && s_card) {
        st.card_size_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
        esp_err_t err = esp_vfs_fat_info(BOARD_SD_MOUNT_POINT, &st.total_bytes, &st.free_bytes);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SD free space query failed: %s", esp_err_to_name(err));
        }
    }
    return st;
}
