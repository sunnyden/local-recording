#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include <sys/stat.h>
#include <errno.h>

static i2c_master_dev_handle_t expander, codec;
static SemaphoreHandle_t lock;
static uint16_t outputs = 0x000c; /* Touch reset low; beep off and amplifier shutdown high. */
static sdmmc_card_t *card;

esp_err_t board_codec_write(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(codec, data, sizeof(data), 100);
}
esp_err_t board_expander_update(uint16_t mask, uint16_t value)
{
    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(200)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    uint16_t next = (outputs & ~mask) | (value & mask);
    uint8_t data[] = {2, next, next >> 8};
    esp_err_t err = i2c_master_transmit(expander, data, sizeof(data), 100);
    if (err == ESP_OK) outputs = next;
    xSemaphoreGive(lock);
    return err;
}
esp_err_t board_init(void)
{
    lock = xSemaphoreCreateMutex();
    if (!lock) return ESP_ERR_NO_MEM;
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t cfg = {.i2c_port = I2C_NUM_0, .sda_io_num = 41,
        .scl_io_num = 42, .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true};
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) return err;
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x20, .scl_speed_hz = 400000};
    if ((err = i2c_master_bus_add_device(bus, &dev, &expander)) != ESP_OK) return err;
    dev.device_address = 0x10;
    if ((err = i2c_master_bus_add_device(bus, &dev, &codec)) != ESP_OK) return err;
    outputs = 0x000c;
    if ((err = board_expander_update(0xffff, outputs)) != ESP_OK) return err;
    uint8_t direction[] = {6, 0x83, 0xf0};
    if ((err = i2c_master_transmit(expander, direction, 3, 100)) != ESP_OK) return err;
    /* All inactive SPI chip selects high before SD enters SPI mode. */
    gpio_set_direction(2, GPIO_MODE_OUTPUT);
    gpio_set_level(2, 1);
    gpio_set_direction(21, GPIO_MODE_OUTPUT);
    gpio_set_level(21, 1);
    spi_bus_config_t spi = {.mosi_io_num = 11, .miso_io_num = 13, .sclk_io_num = 12,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 10240};
    return spi_bus_initialize(SPI2_HOST, &spi, SPI_DMA_CH_AUTO);
}
esp_err_t board_sd_mount(void)
{
    if (card) return ESP_OK;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = 2;
    esp_vfs_fat_sdmmc_mount_config_t mount = {.format_if_mount_failed = false,
        .max_files = 6, .allocation_unit_size = 16384};
#ifdef CONFIG_RECORDER_TEST_FORMAT_SD_ON_MOUNT_FAILURE
    mount.format_if_mount_failed = true;
    ESP_LOGW("board", "Owner-approved disposable-card test: SD formatting is enabled on mount failure");
#endif
    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &card);
    if (err != ESP_OK) return err;
    if (mkdir(RECORDING_DIR, 0700) && errno != EEXIST) return ESP_FAIL;
    return ESP_OK;
}
esp_err_t board_key_read(board_key_t *key)
{
    static uint8_t previous = 0, stable = 0;
    static TickType_t changed;
    uint8_t reg = 1, value;
    esp_err_t err = i2c_master_transmit_receive(expander, &reg, 1, &value, 1, 100);
    *key = KEY_NONE;
    if (err != ESP_OK) return err;
    value = (~value >> 4) & 15;
    TickType_t now = xTaskGetTickCount();
    if (value != previous) { previous = value; changed = now; }
    if (now - changed < pdMS_TO_TICKS(40) || value == stable) return ESP_OK;
    stable = value;
    switch (value) {
        case 1: *key = KEY_UP; break;
        case 2: *key = KEY_BACK; break;
        case 4: *key = KEY_DOWN; break;
        case 8: *key = KEY_ENTER; break;
        default: break;
    }
    return ESP_OK;
}
