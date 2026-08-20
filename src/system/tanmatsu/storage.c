// MIT License

// Copyright (c) 2017 Vadim Grigoruk @nesbox // grigoruk@gmail.com

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// The SD card power-up sequence follows the one in the Tanmatsu launcher
// (Nicolai Electronics, MIT licensed): the card has to be power cycled through
// the on-chip LDO or it comes up still in SDMMC mode from the previous boot.

#include "tanmatsu.h"

#include <stdio.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

static char const TAG[] = "tic80-storage";

#define INTERNAL_MOUNT_POINT  "/int"
#define INTERNAL_PARTITION    "locfd"
#define SD_MOUNT_POINT        "/sd"
#define TIC80_DIRECTORY       "tic80"

static wl_handle_t          wl_handle     = WL_INVALID_HANDLE;
static sdmmc_card_t*        card          = NULL;
static sd_pwr_ctrl_handle_t sd_pwr_handle = NULL;
static char                 root[64]      = INTERNAL_MOUNT_POINT "/" TIC80_DIRECTORY;

static esp_err_t mount_internal(void) {
    esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed   = false,
        .max_files                = 10,
        .allocation_unit_size     = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat              = false,
    };

    return esp_vfs_fat_spiflash_mount_rw_wl(INTERNAL_MOUNT_POINT, INTERNAL_PARTITION, &config, &wl_handle);
}

#if defined(CONFIG_BSP_TARGET_TANMATSU)

static esp_err_t power_cycle_sd_card(void) {
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = BIT64(GPIO_NUM_39) | BIT64(GPIO_NUM_40) | BIT64(GPIO_NUM_41) | BIT64(GPIO_NUM_42) |
                        BIT64(GPIO_NUM_43) | BIT64(GPIO_NUM_44),
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_cfg);

    static const gpio_num_t bus_pins[] = {GPIO_NUM_39, GPIO_NUM_40, GPIO_NUM_41,
                                          GPIO_NUM_42, GPIO_NUM_43, GPIO_NUM_44};
    for (size_t i = 0; i < sizeof(bus_pins) / sizeof(bus_pins[0]); i++) {
        gpio_set_level(bus_pins[i], 0);
    }

    sd_pwr_ctrl_set_io_voltage(sd_pwr_handle, 0);
    vTaskDelay(pdMS_TO_TICKS(150));

    gpio_cfg.mode = GPIO_MODE_INPUT;
    gpio_config(&gpio_cfg);
    sd_pwr_ctrl_set_io_voltage(sd_pwr_handle, 3300);
    vTaskDelay(pdMS_TO_TICKS(150));

    return ESP_OK;
}

static esp_err_t mount_sd_card(void) {
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };

    esp_err_t res = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &sd_pwr_handle);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "No on-chip LDO for the SD card: %s", esp_err_to_name(res));
        return res;
    }

    power_cycle_sd_card();

    sdmmc_host_t host    = SDMMC_HOST_DEFAULT();
    host.slot            = SDMMC_HOST_SLOT_0;
    host.max_freq_khz    = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = sd_pwr_handle;

    static uint8_t* dma_buf = NULL;
    if (dma_buf == NULL) {
        dma_buf = heap_caps_malloc(512 * 4, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (dma_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    host.dma_aligned_buffer = dma_buf;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk                 = GPIO_NUM_43;
    slot_config.cmd                 = GPIO_NUM_44;
    slot_config.d0                  = GPIO_NUM_39;
    slot_config.d1                  = GPIO_NUM_40;
    slot_config.d2                  = GPIO_NUM_41;
    slot_config.d3                  = GPIO_NUM_42;
    slot_config.width               = 4;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 10,
        .allocation_unit_size   = 16 * 1024,
    };

    return esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
}

#else

static esp_err_t mount_sd_card(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

static bool ensure_directory(const char* path) {
    struct stat info;
    if (stat(path, &info) == 0) {
        return true;
    }

    if (mkdir(path, 0777) != 0) {
        ESP_LOGW(TAG, "Could not create %s", path);
        return false;
    }

    return true;
}

esp_err_t tanmatsu_storage_init(void) {
    bool have_internal = false;

    esp_err_t res = mount_internal();
    if (res == ESP_OK) {
        have_internal = true;
        ESP_LOGI(TAG, "Internal filesystem mounted at " INTERNAL_MOUNT_POINT);
    } else {
        ESP_LOGW(TAG, "Failed to mount the internal filesystem: %s", esp_err_to_name(res));
    }

    // Carts live on the SD card when there is one; it is bigger and it is what
    // people can take out and copy files onto.
    if (mount_sd_card() == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted at " SD_MOUNT_POINT);
        snprintf(root, sizeof(root), SD_MOUNT_POINT "/" TIC80_DIRECTORY);
    } else if (have_internal) {
        snprintf(root, sizeof(root), INTERNAL_MOUNT_POINT "/" TIC80_DIRECTORY);
    } else {
        ESP_LOGE(TAG, "No writable filesystem, TIC-80 will not be able to save");
        return ESP_FAIL;
    }

    if (!ensure_directory(root)) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Using %s", root);

    return ESP_OK;
}

const char* tanmatsu_storage_root(void) {
    return root;
}
