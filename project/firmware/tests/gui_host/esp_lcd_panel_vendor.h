#pragma once
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#define LCD_RGB_ELEMENT_ORDER_RGB 0
typedef struct {int reset_gpio_num,rgb_ele_order,bits_per_pixel;} esp_lcd_panel_dev_config_t;
esp_err_t esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t,const esp_lcd_panel_dev_config_t *,esp_lcd_panel_handle_t *);
