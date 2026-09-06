#include "board.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <ctype.h>
#include <string.h>

static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t done;
static uint16_t *line;
/* Original compact 5x7 uppercase glyphs, columns encoded top-to-bottom. */
static const uint8_t letters[26][5] = {
 {126,9,9,9,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
 {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
 {65,65,127,65,65},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
 {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
 {62,65,81,33,94},{127,9,25,41,70},{38,73,73,73,50},{1,1,127,1,1},
 {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
 {3,4,120,4,3},{97,81,73,69,67}
};
static const uint8_t digits[10][5] = {
 {62,81,73,69,62},{0,66,127,64,0},{98,81,73,73,70},{34,65,73,73,54},
 {24,20,18,127,16},{39,69,69,69,57},{62,73,73,73,50},{1,113,9,5,3},
 {54,73,73,73,54},{38,73,73,73,62}
};
static uint8_t glyph(char c, unsigned column)
{
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'][column];
    if (c >= '0' && c <= '9') return digits[c - '0'][column];
    if (c == ':') return column == 2 ? 36 : 0;
    if (c == '.') return column == 2 ? 64 : 0;
    if (c == '-') return 8;
    if (c == '/') return 1u << (6 - column);
    if (c == '_') return 64;
    if (c == '%') return column == 0 || column == 4 ? 99 : (1u << (6 - column));
    return 0;
}
#ifdef CONFIG_RECORDER_SPI_LCD_CONFIRMED
static bool color_done(esp_lcd_panel_io_handle_t io,
    esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    (void)io; (void)event; (void)ctx;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(done, &wake);
    return wake == pdTRUE;
}
#endif
bool board_display_available(void) { return panel != NULL; }
esp_err_t board_display_init(void)
{
#ifndef CONFIG_RECORDER_SPI_LCD_CONFIRMED
    return ESP_ERR_NOT_SUPPORTED;
#else
    done = xSemaphoreCreateBinary();
    line = heap_caps_malloc(320 * 16 * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!done || !line) return ESP_ERR_NO_MEM;
    esp_err_t err = board_expander_update(0x0c00, 0x0800);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(30));
    if ((err = board_expander_update(0x0400, 0x0400)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t cfg = {.dc_gpio_num = 40, .cs_gpio_num = 21,
        .pclk_hz = 20000000, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 1,
        .on_color_trans_done = color_done};
    if ((err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                       &cfg, &io)) != ESP_OK) return err;
    esp_lcd_panel_dev_config_t dev = {.reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16};
    if ((err = esp_lcd_new_panel_st7789(io, &dev, &panel)) != ESP_OK) return err;
    if ((err = esp_lcd_panel_init(panel)) != ESP_OK) return err;
    if ((err = esp_lcd_panel_invert_color(panel, true)) != ESP_OK) return err;
    if ((err = esp_lcd_panel_swap_xy(panel, true)) != ESP_OK) return err;
    if ((err = esp_lcd_panel_mirror(panel, true, false)) != ESP_OK) return err;
    if ((err = esp_lcd_panel_disp_on_off(panel, true)) != ESP_OK) return err;
    for (unsigned row = 0; row < 15; ++row) {
        if ((err = board_display_line(row, "", false)) != ESP_OK) return err;
    }
    return ESP_OK;
#endif
}
esp_err_t board_display_line(unsigned row, const char *text, bool selected)
{
    if (!panel) return ESP_ERR_NOT_SUPPORTED;
    if (row >= 15 || !text) return ESP_ERR_INVALID_ARG;
    size_t n = strnlen(text, 26);
    uint16_t bg = selected ? 0x001f : 0x0000;
    for (unsigned y = 0; y < 16; ++y) {
        for (unsigned x = 0; x < 320; ++x) {
            unsigned c = x / 12, col = (x % 12) / 2;
            bool on = c < n && col < 5 && y < 14 && (glyph(text[c], col) & (1 << (y / 2)));
            uint16_t color = on ? 0xffff : bg;
            line[y * 320 + x] = (color << 8) | (color >> 8);
        }
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, row * 16, 320, row * 16 + 16, line);
    if (err != ESP_OK) return err;
    return xSemaphoreTake(done, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
