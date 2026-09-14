#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../components/board/display.c"
static bool completed,automatic=true;
static unsigned clock_ms,alloc_size;
static const void *owned;
static esp_err_t submit_result;
void *heap_caps_malloc(size_t n,unsigned caps){assert(caps==(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL));alloc_size=(unsigned)n;return malloc(n);}
SemaphoreHandle_t xSemaphoreCreateBinary(void){return (void *)1;}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t ticks)
{(void)s;(void)ticks;if(completed){completed=false;owned=NULL;return pdTRUE;}return pdFALSE;}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s,BaseType_t *w){(void)s;(void)w;completed=true;return pdTRUE;}
TickType_t xTaskGetTickCount(void){return clock_ms;}
void vTaskDelay(TickType_t ticks){clock_ms+=ticks;}
esp_err_t board_expander_update(uint16_t mask,uint16_t value){(void)mask;(void)value;return ESP_OK;}
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,const esp_lcd_panel_io_spi_config_t *cfg,esp_lcd_panel_io_handle_t *out)
{(void)bus;assert(cfg->pclk_hz==20000000&&cfg->trans_queue_depth==1);*out=(void *)1;return ESP_OK;}
esp_err_t esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t io,const esp_lcd_panel_dev_config_t *cfg,esp_lcd_panel_handle_t *out)
{(void)io;(void)cfg;*out=(void *)1;return ESP_OK;}
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p){(void)p;return ESP_OK;}
esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t p,bool b){(void)p;assert(b);return ESP_OK;}
esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t p,bool b){(void)p;assert(b);return ESP_OK;}
esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t p,bool x,bool y){(void)p;assert(x&&!y);return ESP_OK;}
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p,bool b){(void)p;assert(b);return ESP_OK;}
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p,int x,int y,int right,int bottom,const void *pixels)
{(void)p;assert(x>=0&&y>=0&&right<=320&&bottom<=240);assert(!owned);owned=pixels;completed=automatic;return submit_result;}
int main(void)
{
    assert(board_display_init()==ESP_OK);assert(alloc_size==10240);
    automatic=false;
    uint16_t *pixels=board_display_strip();assert(pixels);
    pixels[0]=0xf800;pixels[1]=0x07e0;pixels[2]=0x001f;
    assert(board_display_submit(2,3,3,1)==ESP_OK);
    assert(pixels[0]==0x00f8&&pixels[1]==0xe007&&pixels[2]==0x1f00);
    assert(board_display_strip()==NULL);
    assert(board_display_submit(2,3,3,1)==ESP_ERR_INVALID_STATE);
    assert(board_display_line(0,"unsafe",false)==ESP_ERR_INVALID_STATE);
    completed=true;assert(board_display_poll()==ESP_OK);
    assert(board_display_strip()==pixels);
    assert(board_display_submit(319,239,2,1)==ESP_ERR_INVALID_ARG);
    assert(board_display_submit(0,0,320,9)==ESP_ERR_INVALID_ARG);
    assert(board_display_submit(0,0,0,1)==ESP_ERR_INVALID_ARG);
    assert(board_display_submit(0,0,320,8)==ESP_OK);
    for(unsigned i=0;i<5120;++i)pixels[i]=0xa55a;
    clock_ms+=1001;assert(board_display_poll()==ESP_ERR_TIMEOUT);
    assert(board_display_faulted()&&board_display_strip()==NULL);
    assert(board_display_line(0,"timeout",false)!=ESP_OK);
    assert(board_display_scrub()==ESP_ERR_INVALID_STATE);
    completed=true;assert(board_display_scrub()==ESP_OK);
    for(unsigned i=0;i<5120;++i)assert(pixels[i]==0);
    assert(board_display_poll()==ESP_ERR_TIMEOUT);
    assert(board_display_strip()==NULL);
    display_fault=false;pending=false;submit_result=ESP_FAIL;
    assert(board_display_submit(0,0,8,1)==ESP_FAIL);
    assert(board_display_strip()==NULL&&board_display_faulted());
    completed=true;board_display_poll();
    free(line);
    puts("PASS LCD single DMA ownership, byte order, bounds, timeout quarantine");
    return 0;
}
