#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef void *esp_lcd_panel_io_handle_t;
typedef void *esp_lcd_spi_bus_handle_t;
typedef struct {int unused;} esp_lcd_panel_io_event_data_t;
typedef struct {
    int dc_gpio_num,cs_gpio_num,pclk_hz,lcd_cmd_bits,lcd_param_bits,spi_mode,trans_queue_depth;
    bool (*on_color_trans_done)(esp_lcd_panel_io_handle_t,esp_lcd_panel_io_event_data_t *,void *);
} esp_lcd_panel_io_spi_config_t;
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t,const esp_lcd_panel_io_spi_config_t *,esp_lcd_panel_io_handle_t *);
